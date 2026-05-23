#include "AntennaGeniusModel.h"

#include <QUdpSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QNetworkDatagram>
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QRegularExpression>

namespace MasterSDR {

// Antenna Genius uses port 9007 for both UDP discovery and TCP control.
static constexpr quint16 kAgPort = 9007;
// Keep-alive interval (seconds).
static constexpr int kKeepAliveMs = 30000;
// Discovery timeout: remove device if no broadcast in 5 seconds.
static constexpr int kDiscoveryTimeoutMs = 5000;

// ── AntennaGeniusModel ─────────────────────────────────────────────────────

AntennaGeniusModel::AntennaGeniusModel(QObject* parent)
    : QObject(parent)
{
    m_portA.portId = 1;
    m_portB.portId = 2;
}

AntennaGeniusModel::~AntennaGeniusModel()
{
    disconnectFromDevice();
    stopDiscovery();
}

// ── UDP Discovery ──────────────────────────────────────────────────────────

void AntennaGeniusModel::startDiscovery()
{
    if (m_udpSocket) return;

    m_udpSocket = new QUdpSocket(this);
    // Bind to 9007 to receive AG broadcast datagrams.
    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, kAgPort,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qCWarning(lcTuner) << "AntennaGenius: failed to bind UDP" << kAgPort
                    << m_udpSocket->errorString() << "— will retry in 5s";
        delete m_udpSocket;
        m_udpSocket = nullptr;
        // Retry bind every 5 seconds until it succeeds
        if (!m_retryTimer) {
            m_retryTimer = new QTimer(this);
            m_retryTimer->setInterval(5000);
            connect(m_retryTimer, &QTimer::timeout, this, [this]() {
                startDiscovery();
                if (m_udpSocket) {
                    // Bind succeeded — stop retrying
                    m_retryTimer->stop();
                }
            });
            m_retryTimer->start();
        }
        return;
    }
    connect(m_udpSocket, &QUdpSocket::readyRead,
            this, &AntennaGeniusModel::onDiscoveryDatagram);

    // Timeout timer: prune devices that stop broadcasting.
    m_discoveryTimeout = new QTimer(this);
    m_discoveryTimeout->setInterval(kDiscoveryTimeoutMs);
    connect(m_discoveryTimeout, &QTimer::timeout, this, [this]() {
        // In a full implementation we'd track last-seen timestamps.
        // For now we rely on the device broadcasting every 1 s.
    });
    m_discoveryTimeout->start();

    qCDebug(lcTuner) << "AntennaGenius: discovery started on UDP" << kAgPort;
}

void AntennaGeniusModel::stopDiscovery()
{
    if (m_udpSocket) {
        m_udpSocket->close();
        delete m_udpSocket;
        m_udpSocket = nullptr;
    }
    if (m_discoveryTimeout) {
        m_discoveryTimeout->stop();
        delete m_discoveryTimeout;
        m_discoveryTimeout = nullptr;
    }
}

void AntennaGeniusModel::onDiscoveryDatagram()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_udpSocket->receiveDatagram();
        QString data = QString::fromUtf8(dg.data()).trimmed();

        // AG discovery messages start with "AG "
        if (!data.startsWith("AG "))
            continue;

        // Parse: AG ip=192.168.1.39 port=9007 v=4.0.22 serial=9A-3A-DC name=Ranko_4O3A ...
        auto kvs = parseKeyValues(data.mid(3));  // skip "AG "

        AgDeviceInfo info;
        info.ip       = QHostAddress(kvs.value("ip"));
        info.port     = kvs.value("port", "9007").toUShort();
        info.version  = kvs.value("v");
        info.serial   = kvs.value("serial");
        info.name     = kvs.value("name").replace('_', ' ');
        info.radioPorts   = kvs.value("ports", "2").toInt();
        info.antennaPorts = kvs.value("antennas", "8").toInt();
        info.webPort      = kvs.value("webport", "0").toInt();
        info.mode     = kvs.value("mode");

        if (info.serial.isEmpty() || info.ip.isNull())
            continue;

        // Check if already known
        bool found = false;
        for (auto& d : m_discoveredDevices) {
            if (d.serial == info.serial) {
                d = info;  // refresh
                found = true;
                break;
            }
        }

        if (!found) {
            bool wasEmpty = m_discoveredDevices.isEmpty();
            m_discoveredDevices.append(info);
            qCDebug(lcTuner) << "AntennaGenius: discovered" << info.name
                     << "at" << info.ip.toString() << "serial" << info.serial;
            emit deviceDiscovered(info);
            if (wasEmpty)
                emit presenceChanged(true);
        }

        // Late enrichment: if a UDP beacon arrives after the prologue, update m_device
        // with the full serial/webPort/antennaPorts now that we have them.
        // Runs on every beacon so it works even if the device was already known.
        if (m_connected &&
            (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-")) &&
            !m_device.ip.isNull() && m_device.ip == info.ip) {
            m_device.serial       = info.serial;
            m_device.name         = info.name;
            m_device.webPort      = info.webPort;
            m_device.radioPorts   = info.radioPorts;
            m_device.antennaPorts = info.antennaPorts;
            emit antennasChanged();
        }
    }
}

// ── TCP Connection ─────────────────────────────────────────────────────────

void AntennaGeniusModel::connectToDevice(const AgDeviceInfo& info)
{
    // Always clean up any existing socket — connected or still pending.
    // Multiple auto-connect triggers (radio connect + UDP beacon) can fire
    // in quick succession; without this the R4 accepts the first socket and
    // sends its greeting there while MasterSDR's active socket is a later one.
    if (m_tcpSocket) {
        m_tcpSocket->abort();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }

    m_device = info;
    m_gotPrologue = false;
    m_lineBuffer.clear();
    m_nextSeq = 1;
    m_pending.clear();

    m_tcpSocket = new QTcpSocket(this);
    connect(m_tcpSocket, &QTcpSocket::connected,
            this, &AntennaGeniusModel::onTcpConnected);
    connect(m_tcpSocket, &QTcpSocket::disconnected,
            this, &AntennaGeniusModel::onTcpDisconnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead,
            this, &AntennaGeniusModel::onTcpReadyRead);
    connect(m_tcpSocket, &QTcpSocket::errorOccurred,
            this, [this]() { onTcpError(); });

    qCDebug(lcTuner) << "AntennaGenius: connecting to" << info.ip.toString() << ":" << info.port;
    m_tcpSocket->connectToHost(info.ip, info.port);
}

void AntennaGeniusModel::connectToAddress(const QHostAddress& ip, quint16 port)
{
    AgDeviceInfo info;
    info.ip   = ip;
    info.port = port;
    info.name = ip.toString();
    info.serial = QString("manual-%1").arg(ip.toString());
    connectToDevice(info);
}

QString AntennaGeniusModel::peerAddress() const
{
    return m_tcpSocket ? m_tcpSocket->peerAddress().toString() : QString();
}

quint16 AntennaGeniusModel::peerPort() const
{
    return m_tcpSocket ? m_tcpSocket->peerPort() : 0;
}

void AntennaGeniusModel::disconnectFromDevice()
{
    if (m_keepAlive) {
        m_keepAlive->stop();
        delete m_keepAlive;
        m_keepAlive = nullptr;
    }
    if (m_tcpSocket) {
        m_tcpSocket->disconnectFromHost();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
    m_antennas.clear();
    m_bands.clear();
    m_portA = AgPortStatus{1};
    m_portB = AgPortStatus{2};
    m_pending.clear();
}

void AntennaGeniusModel::onTcpConnected()
{
    qCDebug(lcTuner) << "AntennaGenius: TCP connected to" << m_device.ip.toString();

    // Arduino WiFiServer::available() only returns a client once it has sent data.
    // For ShackSwitch (R4) the server speaks first ("V1.0 AG"), so we send an
    // empty line to trigger available() on the R4 side without affecting the
    // protocol (the R4 discards empty lines before the greeting is sent).
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch && m_tcpSocket) {
        m_tcpSocket->write("\r\n");
        m_tcpSocket->flush();
    }

    // Wait for prologue line ("V<version> AG") before sending commands.
}

void AntennaGeniusModel::onTcpDisconnected()
{
    qCDebug(lcTuner) << "AntennaGenius: TCP disconnected";
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
    if (m_keepAlive) {
        m_keepAlive->stop();
    }
}

void AntennaGeniusModel::onTcpError()
{
    if (!m_tcpSocket) return;
    QString err = m_tcpSocket->errorString();
    qCWarning(lcTuner) << "AntennaGenius: TCP error:" << err;
    emit connectionError(err);
}

void AntennaGeniusModel::onTcpReadyRead()
{
    if (!m_tcpSocket) return;

    m_lineBuffer += QString::fromUtf8(m_tcpSocket->readAll());

    // Process complete lines (terminated by \r\n or \n).
    int pos;
    while ((pos = m_lineBuffer.indexOf('\n')) >= 0) {
        QString line = m_lineBuffer.left(pos).trimmed();
        m_lineBuffer.remove(0, pos + 1);

        if (line.isEmpty()) continue;

        // First line after connect is prologue: "V<version> AG"
        if (!m_gotPrologue) {
            if (line.startsWith('V') && line.contains("AG")) {
                m_gotPrologue = true;
                // Extract version from "V4.0.22 AG"
                auto parts = line.split(' ');
                if (!parts.isEmpty())
                    m_device.version = parts[0].mid(1);  // skip 'V'
                qCDebug(lcTuner) << "AntennaGenius: prologue received, version"
                         << m_device.version;
                // If connected via manual IP, enrich from UDP-discovered list.
                // Matches "ShackSwitch-manual" and legacy "manual-<ip>" serials.
                if (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-")) {
                    for (const auto& d : m_discoveredDevices) {
                        if (!d.ip.isNull() && d.ip == m_device.ip) {
                            m_device.serial       = d.serial;
                            m_device.name         = d.name;
                            m_device.webPort      = d.webPort;
                            m_device.radioPorts   = d.radioPorts;
                            m_device.antennaPorts = d.antennaPorts;
                            break;
                        }
                    }
                    // UDP beacon not yet received — infer port count from firmware version.
                    // Uno Q sends V2.0, R4 sends V1.0. This runs before emit connected()
                    // so the applet sees the correct radioPorts immediately.
                    if (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-"))
                        m_device.radioPorts = (m_device.version == "2.0") ? 2 : 1;
                }

                m_connected = true;
                emit connected();

                // Ensure manually-connected devices appear in the
                // discovered list so the UI button becomes visible.
                bool wasEmpty = m_discoveredDevices.isEmpty();
                bool found = false;
                for (const auto& d : m_discoveredDevices) {
                    if (d.serial == m_device.serial) { found = true; break; }
                }
                if (!found) {
                    m_discoveredDevices.append(m_device);
                    emit deviceDiscovered(m_device);
                    if (wasEmpty)
                        emit presenceChanged(true);
                }

                runInitSequence();
            }
            continue;
        }

        processLine(line);
    }
}

// ── Command sending ────────────────────────────────────────────────────────

int AntennaGeniusModel::sendCommand(const QString& cmd)
{
    if (!m_tcpSocket || !m_connected) return -1;

    int seq = m_nextSeq++;
    if (m_nextSeq > 255) m_nextSeq = 1;

    QString line = QString("C%1|%2\r\n").arg(seq).arg(cmd);
    m_tcpSocket->write(line.toUtf8());

    // Register pending response.
    m_pending[seq] = PendingResponse{cmd, {}};

    return seq;
}

// ── Line processing ────────────────────────────────────────────────────────

void AntennaGeniusModel::processLine(const QString& line)
{
    if (line.isEmpty()) return;

    QChar prefix = line[0];

    if (prefix == 'R') {
        // Response: R<seq>|<hex_code>|<body>
        // Find first and second '|'.
        int p1 = line.indexOf('|');
        int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
        if (p1 < 0 || p2 < 0) return;

        int seq = line.mid(1, p1 - 1).toInt();
        int code = line.mid(p1 + 1, p2 - p1 - 1).toInt(nullptr, 16);
        QString body = line.mid(p2 + 1);

        processResponse(seq, code, body);

    } else if (prefix == 'S') {
        // Status: S0|<body>
        int p1 = line.indexOf('|');
        if (p1 < 0) return;
        QString body = line.mid(p1 + 1);
        processStatus(body);
    }
}

void AntennaGeniusModel::processResponse(int seq, int code, const QString& body)
{
    if (!m_pending.contains(seq)) return;

    auto& pr = m_pending[seq];

    // Empty body marks end of multi-line response.
    if (body.isEmpty()) {
        // Process accumulated lines.
        QStringList lines = pr.lines;
        QString cmd = pr.command;
        m_pending.remove(seq);

        if (seq == m_seqAntennaList) {
            // Parse antenna list.
            m_antennas.clear();
            for (const QString& l : lines) {
                // "antenna 1 name=Antenna_1 tx=0000 rx=0001 inband=0000"
                static QRegularExpression reId(R"(antenna\s+(\d+)\s+)");
                auto mId = reId.match(l);
                if (!mId.hasMatch()) continue;

                AgAntennaInfo ant;
                ant.id = mId.captured(1).toInt();
                auto kvs = parseKeyValues(l.mid(mId.capturedEnd()));
                ant.name = kvs.value("name").replace('_', ' ');
                ant.txBandMask  = kvs.value("tx", "0").toUShort(nullptr, 16);
                ant.rxBandMask  = kvs.value("rx", "0").toUShort(nullptr, 16);
                ant.inbandMask  = kvs.value("inband", "0").toUShort(nullptr, 16);
                m_antennas.append(ant);
            }
            qCDebug(lcTuner) << "AntennaGenius:" << m_antennas.size() << "antennas loaded";
            emit antennasChanged();

        } else if (seq == m_seqBandList) {
            // Parse band list.
            m_bands.clear();
            for (const QString& l : lines) {
                // "band 0 name=None freq_start=0.000000 freq_stop=0.000000"
                static QRegularExpression reId(R"(band\s+(\d+)\s+)");
                auto mId = reId.match(l);
                if (!mId.hasMatch()) continue;

                AgBandInfo band;
                band.id = mId.captured(1).toInt();
                auto kvs = parseKeyValues(l.mid(mId.capturedEnd()));
                band.name = kvs.value("name").replace('_', ' ');
                band.freqStartMhz = kvs.value("freq_start", "0").toDouble();
                band.freqStopMhz  = kvs.value("freq_stop", "0").toDouble();
                m_bands.append(band);
            }
            qCDebug(lcTuner) << "AntennaGenius:" << m_bands.size() << "bands loaded";
            emit bandsChanged();

            // Bands just loaded — reprocess the cached radio frequency so
            // m_lastRadioBand is set correctly for antenna save/recall.
            if (m_lastRadioFreqMhz > 0.0) {
                qCDebug(lcTuner) << "AG-FREQ: reprocessing cached frequency"
                         << m_lastRadioFreqMhz << "MHz after bands loaded";
                double cached = m_lastRadioFreqMhz;
                m_lastRadioFreqMhz = 0.0;  // prevent infinite loop
                setRadioFrequency(cached);
            }

        } else if (seq == m_seqPortA || seq == m_seqPortB) {
            // Parse port status from response (single line).
            if (!lines.isEmpty()) {
                auto ps = parsePortStatus(lines.first());
                if (seq == m_seqPortA) {
                    m_portA = ps;
                    m_portA.portId = 1;
                    m_prevBandA = ps.band;
                    emit portStatusChanged(1);
                    // On initial connect, recall saved antenna for this band.
                    if (ps.band > 0) {
                        int saved = recallBandAntenna(1, ps.band);
                        if (saved > 0)
                            selectAntenna(1, saved);
                    }
                } else {
                    m_portB = ps;
                    m_portB.portId = 2;
                    m_prevBandB = ps.band;
                    emit portStatusChanged(2);
                    if (ps.band > 0) {
                        int saved = recallBandAntenna(2, ps.band);
                        if (saved > 0)
                            selectAntenna(2, saved);
                    }
                }
            }
        }
        return;
    }

    // Accumulate multi-line response body.
    if (code == 0) {
        pr.lines.append(body);
    } else {
        qCWarning(lcTuner) << "AntennaGenius: command" << pr.command
                    << "error code" << Qt::hex << code << body;
    }
}

void AntennaGeniusModel::processStatus(const QString& body)
{
    // "port 1 auto=1 source=AUTO band=5 rxant=3 txant=3 tx=0 inhibit=0"
    if (body.startsWith("port ")) {
        auto ps = parsePortStatus(body);
        if (ps.portId == 1) {
            int oldBand = m_portA.band;
            int oldAnt  = m_portA.rxAntenna;
            m_portA = ps;
            emit portStatusChanged(1);
            if (ps.band != oldBand)
                onBandChanged(1, oldBand, oldAnt, ps.band);
        } else if (ps.portId == 2) {
            int oldBand = m_portB.band;
            int oldAnt  = m_portB.rxAntenna;
            m_portB = ps;
            emit portStatusChanged(2);
            if (ps.band != oldBand)
                onBandChanged(2, oldBand, oldAnt, ps.band);
        }
    } else if (body.startsWith("antenna reload")) {
        // Antenna configuration changed on the device — re-fetch.
        qCDebug(lcTuner) << "AntennaGenius: antenna reload, re-fetching list";
        m_seqAntennaList = sendCommand("antenna list");
    }
    // "relay tx=00 rx=04 state=04" — informational, ignore for now.
}

// ── Init sequence ──────────────────────────────────────────────────────────

void AntennaGeniusModel::runInitSequence()
{
    m_seqAntennaList = sendCommand("antenna list");
    m_seqBandList    = sendCommand("band list");
    m_seqPortA       = sendCommand("port get 1");
    m_seqPortB       = sendCommand("port get 2");
    m_seqSubPort     = sendCommand("sub port all");
    m_seqSubRelay    = sendCommand("sub relay");

    // Start keep-alive timer.
    if (!m_keepAlive) {
        m_keepAlive = new QTimer(this);
        connect(m_keepAlive, &QTimer::timeout,
                this, &AntennaGeniusModel::onKeepAlive);
    }
    m_keepAlive->start(kKeepAliveMs);
}

void AntennaGeniusModel::onKeepAlive()
{
    if (m_connected)
        sendCommand("ping");
}

// ── Commands ───────────────────────────────────────────────────────────────

void AntennaGeniusModel::selectAntenna(int portId, int antennaId)
{
    if (!m_connected) return;

    // Prevent selecting an antenna already in use by the other port.
    // Antenna 0 (deselect) is always allowed.
    // ShackSwitch is a single-radio device — no port B conflict is possible.
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (!isShackSwitch) {
        const auto& otherPort = (portId == 1) ? m_portB : m_portA;
        if (antennaId > 0 && otherPort.rxAntenna == antennaId) {
            qCDebug(lcTuner) << "AntennaGenius: antenna" << antennaName(antennaId)
                     << "already in use on port" << otherPort.portId << "— blocked";
            return;
        }
    }

    const auto& ps = (portId == 1) ? m_portA : m_portB;

    // Determine the effective band: prefer the AG device's reported band,
    // fall back to our client-side radio-frequency-derived band.
    int effectiveBand = ps.band;
    if (effectiveBand <= 0 && portId == 1)
        effectiveBand = m_lastRadioBand;

    // Always set rxant to the selected antenna.
    // Deselect (antenna 0) clears both rxant and txant.
    // Otherwise set txant only if the antenna has TX permission for the
    // current band; keep the existing txant if not.
    int txAnt = ps.txAntenna;  // keep current TX antenna by default
    if (antennaId == 0) {
        txAnt = 0;
    } else if (canTxOnBand(antennaId, effectiveBand)) {
        txAnt = antennaId;
    } else if (effectiveBand > 0) {
        qCDebug(lcTuner) << "AntennaGenius: antenna" << antennaName(antennaId)
                 << "has no TX permission on band" << bandName(effectiveBand)
                 << "— keeping txant=" << txAnt;
    }

    QString cmd = QString("port set %1 rxant=%2 txant=%3")
                      .arg(portId).arg(antennaId).arg(txAnt);
    qCDebug(lcTuner) << "AntennaGenius:" << cmd;
    sendCommand(cmd);

    // Save band→antenna mapping for the effective band on this port.
    if (effectiveBand > 0) {
        saveBandAntenna(portId, effectiveBand, antennaId);
    } else {
        qCDebug(lcTuner) << "AntennaGenius: no band known for port" << portId
                 << "— cannot save antenna mapping";
    }
}

void AntennaGeniusModel::setAutoMode(int portId, bool on)
{
    if (!m_connected) return;
    QString cmd = QString("port set %1 auto=%2")
                      .arg(portId).arg(on ? 1 : 0);
    qCDebug(lcTuner) << "AntennaGenius:" << cmd;
    sendCommand(cmd);
}

void AntennaGeniusModel::setRadioFrequency(double freqMhz)
{
    // Always cache the frequency so we can reprocess when bands load later.
    m_lastRadioFreqMhz = freqMhz;

    if (!m_connected) {
        return;
    }
    if (m_bands.isEmpty()) {
        qCDebug(lcTuner) << "AG-FREQ: bands list empty, will reprocess when loaded";
        return;
    }

    // Find which AG band this frequency falls in.
    int matchedBand = 0;
    for (const auto& b : m_bands) {
        if (b.id == 0) continue;
        if (freqMhz >= b.freqStartMhz && freqMhz <= b.freqStopMhz) {
            matchedBand = b.id;
            break;
        }
    }

    if (matchedBand == m_lastRadioBand) return;  // no change

    // If the AG itself reports bands (BCD connection), defer save/recall
    // to onBandChanged() which uses the correctly-captured old antenna.
    if (m_portA.band > 0) {
        m_lastRadioBand = matchedBand;
        qCDebug(lcTuner) << "AG-FREQ:" << freqMhz << "MHz → band"
                 << matchedBand << bandName(matchedBand)
                 << "(AG has BCD, deferring to onBandChanged)";
        emit radioBandChanged(matchedBand);
        return;
    }

    int oldBand = m_lastRadioBand;
    m_lastRadioBand = matchedBand;

    qCDebug(lcTuner) << "AG-FREQ:" << freqMhz << "MHz → band"
             << matchedBand << bandName(matchedBand)
             << "(was" << oldBand << bandName(oldBand) << ")";

    emit radioBandChanged(matchedBand);

    // Save current antenna for old band, but only if no user selection
    // already exists.  selectAntenna() is the authoritative save point
    // for manual choices — here we just capture the default if the user
    // never explicitly picked an antenna for the old band.
    int oldAnt = m_portA.rxAntenna;
    if (oldBand > 0 && oldAnt > 0) {
        int existing = recallBandAntenna(1, oldBand);
        if (existing <= 0) {
            saveBandAntenna(1, oldBand, oldAnt);
        }
    }

    if (matchedBand == 0) return;

    // ShackSwitch manages its own band→antenna mapping — don't override it.
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch) {
        qCDebug(lcTuner) << "AG-FREQ: ShackSwitch connected — skipping auto-recall";
        return;
    }

    // Recall saved antenna for new band.
    int saved = recallBandAntenna(1, matchedBand);
    if (saved > 0 && saved != m_portA.rxAntenna) {
        qCDebug(lcTuner) << "AG-FREQ: recalling antenna" << antennaName(saved)
                 << "for" << bandName(matchedBand);
        selectAntenna(1, saved);
    } else if (saved <= 0 && m_portA.rxAntenna > 0) {
        // First visit to this band — save current antenna as default.
        saveBandAntenna(1, matchedBand, m_portA.rxAntenna);
    }
}

// ── Band→Antenna memory (AppSettings persistence) ────────────────────────

void AntennaGeniusModel::saveBandAntenna(int portId, int bandId, int antennaId)
{
    auto& s = AppSettings::instance();
    QString key = QString("AG_Port%1_Band%2").arg(portId).arg(bandId);
    s.setValue(key, QString::number(antennaId));
    qCDebug(lcTuner) << "AntennaGenius: saved band" << bandName(bandId)
             << "→" << antennaName(antennaId) << "for port" << portId;
}

int AntennaGeniusModel::recallBandAntenna(int portId, int bandId) const
{
    auto& s = AppSettings::instance();
    QString key = QString("AG_Port%1_Band%2").arg(portId).arg(bandId);
    return s.value(key, "0").toInt();
}

void AntennaGeniusModel::onBandChanged(int portId, int oldBand, int oldAnt, int newBand)
{
    qCDebug(lcTuner) << "AntennaGenius: port" << portId << "band changed"
             << bandName(oldBand) << "→" << bandName(newBand);

    // ShackSwitch manages its own band→antenna mapping — don't override it.
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch) {
        qCDebug(lcTuner) << "AntennaGenius: ShackSwitch connected — skipping band recall for port" << portId;
        return;
    }

    // Save the antenna for the old band, but only as a default if no
    // user selection already exists.  selectAntenna() is the authoritative
    // save point — we must not overwrite a manual choice with whatever the
    // AG's auto-mode happened to leave active.
    if (oldBand > 0 && oldAnt > 0) {
        int existing = recallBandAntenna(portId, oldBand);
        if (existing <= 0) {
            saveBandAntenna(portId, oldBand, oldAnt);
        }
    }

    if (newBand == 0) return;  // band "None" — nothing to recall

    // Recall the saved antenna for the new band.
    // Always send the command even if the AG already reports the correct
    // antenna — the AG's auto-mode can override it milliseconds later,
    // so we must assert our preference unconditionally (#1213).
    int saved = recallBandAntenna(portId, newBand);
    if (saved > 0) {
        qCDebug(lcTuner) << "AntennaGenius: recalling antenna" << antennaName(saved)
                 << "for band" << bandName(newBand) << "on port" << portId;
        selectAntenna(portId, saved);
    } else if (newBand > 0) {
        // First visit to this band — save current antenna as default.
        const auto& ps = (portId == 1) ? m_portA : m_portB;
        if (ps.rxAntenna > 0)
            saveBandAntenna(portId, newBand, ps.rxAntenna);
    }
}

// ── TX/RX band permission checks ──────────────────────────────────────────

int AntennaGeniusModel::effectiveBand(int portId) const
{
    const auto& ps = (portId == 1) ? m_portA : m_portB;
    int band = ps.band;
    if (band <= 0 && portId == 1)
        band = m_lastRadioBand;
    return band;
}

bool AntennaGeniusModel::canTxOnBand(int antennaId, int bandId) const
{
    if (bandId <= 0 || bandId > 15) return false;
    for (const auto& a : m_antennas) {
        if (a.id == antennaId)
            return (a.txBandMask >> bandId) & 1;
    }
    return false;
}

bool AntennaGeniusModel::canRxOnBand(int antennaId, int bandId) const
{
    if (bandId <= 0 || bandId > 15) return false;
    for (const auto& a : m_antennas) {
        if (a.id == antennaId)
            return (a.rxBandMask >> bandId) & 1;
    }
    return false;
}

bool AntennaGeniusModel::isShackSwitch(const AgDeviceInfo& info)
{
    return info.name.contains(QStringLiteral("ShackSwitch"), Qt::CaseInsensitive);
}

// ── Helpers ────────────────────────────────────────────────────────────────

QString AntennaGeniusModel::bandName(int bandId) const
{
    for (const auto& b : m_bands) {
        if (b.id == bandId) return b.name;
    }
    return bandId == 0 ? "None" : QString::number(bandId);
}

QString AntennaGeniusModel::antennaName(int antennaId) const
{
    for (const auto& a : m_antennas) {
        if (a.id == antennaId) return a.name;
    }
    return antennaId == 0 ? "None" : QString("Ant %1").arg(antennaId);
}

QMap<QString, QString> AntennaGeniusModel::parseKeyValues(const QString& text)
{
    QMap<QString, QString> result;
    // Match key=value pairs (value may contain no spaces, or be quoted).
    static QRegularExpression re(R"((\w+)=(\S+))");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        result[m.captured(1)] = m.captured(2);
    }
    return result;
}

AgPortStatus AntennaGeniusModel::parsePortStatus(const QString& text) const
{
    AgPortStatus ps;
    // Extract port ID: "port 1 ..."
    static QRegularExpression rePort(R"(port\s+(\d+)\s+)");
    auto m = rePort.match(text);
    if (m.hasMatch()) {
        ps.portId = m.captured(1).toInt();
    }

    auto kvs = parseKeyValues(text);
    ps.autoMode     = kvs.value("auto", "1") == "1";
    ps.source       = kvs.value("source", "AUTO");
    ps.band         = kvs.value("band", "0").toInt();
    ps.rxAntenna    = kvs.value("rxant", "0").toInt();
    ps.txAntenna    = kvs.value("txant", "0").toInt();
    ps.transmitting = kvs.value("tx", "0") == "1";
    ps.inhibited    = kvs.value("inhibit", "0") == "1";
    return ps;
}

} // namespace MasterSDR
