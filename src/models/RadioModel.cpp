#include "RadioModel.h"
#include "AntennaAliasStore.h"
#include "BandSettings.h"
#include "core/CommandParser.h"
#include "core/AppSettings.h"
#include "core/CwTrace.h"
#include "core/LogManager.h"
#include "core/PerfTelemetry.h"
#include "core/StreamStatus.h"
#include "core/UdpRegistrationPolicy.h"
#include "RadioStatusOwnership.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDateTime>
#include <QSysInfo>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <memory>

namespace AetherSDR {

namespace {

QJsonArray toJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

QJsonArray toJsonArray(const QVector<int>& values)
{
    QJsonArray array;
    for (int value : values)
        array.append(value);
    return array;
}

QJsonArray toJsonArray(const QSet<int>& values)
{
    QList<int> sorted = values.values();
    std::sort(sorted.begin(), sorted.end());

    QJsonArray array;
    for (int value : sorted)
        array.append(value);
    return array;
}

QString atuStatusToString(ATUStatus status)
{
    switch (status) {
    case ATUStatus::None:         return "None";
    case ATUStatus::NotStarted:   return "NotStarted";
    case ATUStatus::InProgress:   return "InProgress";
    case ATUStatus::Bypass:       return "Bypass";
    case ATUStatus::Successful:   return "Successful";
    case ATUStatus::OK:           return "OK";
    case ATUStatus::FailBypass:   return "FailBypass";
    case ATUStatus::Fail:         return "Fail";
    case ATUStatus::Aborted:      return "Aborted";
    case ATUStatus::ManualBypass: return "ManualBypass";
    }
    return "Unknown";
}

bool statusFlagSet(const QMap<QString, QString>& kvs, const QString& key)
{
    const QString value = kvs.value(key).trimmed();
    return value == QStringLiteral("1")
        || value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

void appendUniqueAntennaToken(QStringList& tokens, const QString& token)
{
    if (!token.isEmpty() && !tokens.contains(token))
        tokens.append(token);
}

QString cleanClientText(QString value)
{
    value.replace(QChar(0x7f), QLatin1Char(' '));
    return value.trimmed();
}

quint32 parseClientHandle(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text = text.mid(2);

    bool ok = false;
    const quint32 handle = text.toUInt(&ok, 16);
    return ok ? handle : 0;
}

// parseStreamToken — identical to parseStatusHandle; use the shared version.
inline quint32 parseStreamToken(QString text) { return parseStatusHandle(std::move(text)); }

QString hexId(quint32 value)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(value, 16).rightJustified(8, QLatin1Char('0')));
}

QString hexCode(int value)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(static_cast<quint32>(value), 16).rightJustified(8, QLatin1Char('0')));
}

QString normalizePanadapterId(QString text)
{
    const QString trimmed = text.trimmed();
    const QString normalized = RadioStatusOwnership::normalizedFlexId(trimmed);
    return normalized.isEmpty() ? trimmed : normalized;
}

QString parsePanadapterCreateId(const QString& body)
{
    return RadioStatusOwnership::parsePanafallCreatePanId(body);
}

struct StreamObjectParts {
    bool valid{false};
    quint32 streamId{0};
    QString action;
};

StreamObjectParts parseStreamObject(const QString& object, const QString& prefix)
{
    if (!object.startsWith(prefix + QLatin1Char(' ')))
        return {};

    const QString rest = object.mid(prefix.size() + 1).trimmed();
    const int firstSpace = rest.indexOf(QLatin1Char(' '));
    const QString idText = firstSpace >= 0 ? rest.left(firstSpace) : rest;

    StreamObjectParts parts;
    parts.streamId = parseStreamToken(idText);
    parts.valid = parts.streamId != 0;
    if (firstSpace >= 0)
        parts.action = rest.mid(firstSpace + 1).trimmed();
    return parts;
}

bool isDaxStreamType(const QString& type)
{
    return type == QStringLiteral("dax_rx")
        || type == QStringLiteral("dax_tx")
        || type == QStringLiteral("dax_mic")
        || type == QStringLiteral("dax_iq");
}

bool streamStatusRemoved(const StreamObjectParts& stream,
                         const QMap<QString, QString>& kvs)
{
    return stream.action == QStringLiteral("removed")
        || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QStringLiteral("0");
}

bool looksLikeClientId(const QString& value)
{
    static const QRegularExpression guidRe(
        QStringLiteral(R"(^\{?[0-9A-Fa-f]{8}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{12}\}?$)"));
    return guidRe.match(value.trimmed()).hasMatch();
}

QString clientConnectionSource(const QMap<QString, QString>& kvs)
{
    const QStringList keys = {
        QStringLiteral("ip"),
        QStringLiteral("client_ip"),
        QStringLiteral("remote_ip"),
        QStringLiteral("name")
    };

    for (const QString& key : keys) {
        const QString value = cleanClientText(kvs.value(key));
        if (!value.isEmpty() && !looksLikeClientId(value))
            return value;
    }

    return {};
}

QJsonObject panToJson(const PanadapterModel* pan, const QString& activePanId)
{
    QJsonObject obj;
    obj["pan_id"] = pan->panId();
    obj["active"] = pan->panId() == activePanId;
    obj["waterfall_id"] = pan->waterfallId();
    obj["center_mhz"] = pan->centerMhz();
    obj["bandwidth_mhz"] = pan->bandwidthMhz();
    obj["min_dbm"] = pan->minDbm();
    obj["max_dbm"] = pan->maxDbm();
    obj["antennas"] = toJsonArray(pan->antList());
    obj["rf_gain"] = pan->rfGain();
    obj["rf_gain_low"] = pan->rfGainLow();
    obj["rf_gain_high"] = pan->rfGainHigh();
    obj["rf_gain_step"] = pan->rfGainStep();
    obj["preamp"] = pan->preamp();
    obj["wnb_active"] = pan->wnbActive();
    obj["wnb_level"] = pan->wnbLevel();
    obj["resized"] = pan->isResized();
    obj["waterfall_configured"] = pan->isWaterfallConfigured();
    return obj;
}

QJsonObject xvtrToJson(const RadioModel::XvtrInfo& xvtr)
{
    QJsonObject obj;
    obj["index"] = xvtr.index;
    obj["order"] = xvtr.order;
    obj["name"] = xvtr.name;
    obj["rf_freq_mhz"] = xvtr.rfFreq;
    obj["if_freq_mhz"] = xvtr.ifFreq;
    obj["offset_mhz"] = xvtr.rfFreq - xvtr.ifFreq;
    obj["lo_error"] = xvtr.loError;
    obj["rx_gain"] = xvtr.rxGain;
    obj["max_power"] = xvtr.maxPower;
    obj["rx_only"] = xvtr.rxOnly;
    obj["is_valid"] = xvtr.isValid;
    obj["has_is_valid"] = xvtr.hasIsValid;
    return obj;
}

QJsonObject clientInfoToJson(quint32 handle,
                             quint32 ourHandle,
                             quint32 txHandle,
                             const RadioModel::ClientInfo& info)
{
    QJsonObject obj;
    obj["role"] = (handle == ourHandle) ? "current_app" : "other_client";
    obj["owns_tx"] = (txHandle != 0 && handle == txHandle);
    obj["program"] = info.program;
    obj["source"] = info.source;
    obj["local_ptt"] = info.localPtt;
    obj["tx_antenna"] = info.txAntenna;
    obj["tx_freq_mhz"] = info.txFreqMhz;
    return obj;
}

} // namespace

RadioModel::RadioModel(QObject* parent)
    : QObject(parent)
{
    // PanadapterStream runs on its own network thread (#502)
    m_networkThread = new QThread(this);
    m_networkThread->setObjectName("PanadapterStream");
    m_panStream = new PanadapterStream;  // no parent — will be moved to thread
    m_panStream->moveToThread(m_networkThread);
    connect(m_networkThread, &QThread::started, m_panStream, &PanadapterStream::init);
    m_networkThread->start();

    // RadioConnection runs on its own worker thread (#502) so TCP I/O
    // (including ping RTT measurement) is never blocked by paintEvent.
    m_connThread = new QThread(this);
    m_connThread->setObjectName("RadioConnection");
    m_connection = new RadioConnection;  // no parent — will be moved to thread
    m_connection->moveToThread(m_connThread);
    connect(m_connThread, &QThread::started, m_connection, &RadioConnection::init);
    m_connThread->start();

    // Signals from RadioConnection auto-queue to main thread (#502)
    connect(m_connection, &RadioConnection::statusReceived,
            this, &RadioModel::onStatusReceived);
    connect(m_connection, &RadioConnection::messageReceived,
            this, &RadioModel::onMessageReceived);
    connect(m_connection, &RadioConnection::connected,
            this, &RadioModel::onConnected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &RadioModel::onDisconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &RadioModel::onConnectionError);
    connect(m_connection, &RadioConnection::versionReceived,
            this, &RadioModel::onVersionReceived);

    // Response callbacks: RadioConnection emits commandResponse on worker thread,
    // we dispatch to the matching callback on the main thread. (#502)
    connect(m_connection, &RadioConnection::commandResponse,
            this, [this](quint32 seq, int code, const QString& body) {
        auto it = m_pendingCallbacks.find(seq);
        if (it != m_pendingCallbacks.end()) {
            it.value()(code, body);
            m_pendingCallbacks.erase(it);
        }
    });

    // Forward VITA-49 meter packets to MeterModel (cross-thread, auto-queued)
    connect(m_panStream, &PanadapterStream::meterDataReady,
            &m_meterModel, &MeterModel::updateValues);

    // Forward tuner commands to the radio — route through tune inhibit check
    connect(&m_tunerModel, &TunerModel::commandReady, this, [this](const QString& cmd){
        if (cmd.startsWith("tgxl autotune"))
            applyTuneInhibit();
        sendCmd(cmd);
    });

    // Forward DAX IQ commands to the radio
    connect(&m_daxIqModel, &DaxIqModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    m_transmitModel.setPttPreflight([this](TransmitModel::PttSource source) {
        m_pendingTransmitPreflightSource = source;
        return localPttInterlockMessage(source);
    });
    connect(&m_transmitModel, &TransmitModel::pttBlocked,
            this, [this](const QString& message) {
        m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
        emitInterlockNotification(
            message,
            QStringLiteral("local-ptt:%1").arg(message));
    });

    // Forward transmit model commands to the radio
    connect(&m_transmitModel, &TransmitModel::commandReady, this, [this](const QString& cmd){
        const QString trimmed = cmd.trimmed();
        if (trimmed.startsWith(QStringLiteral("transmit set "), Qt::CaseInsensitive)) {
            const QMap<QString, QString> kvs =
                CommandParser::parseKVs(trimmed.mid(QStringLiteral("transmit set ").size()));
            if (kvs.contains(QStringLiteral("filter_low"))
                && kvs.contains(QStringLiteral("filter_high"))) {
                const QString message = txFilterFrequencyLimitMessage(
                    kvs.value(QStringLiteral("filter_low")).toInt(),
                    kvs.value(QStringLiteral("filter_high")).toInt());
                if (!message.isEmpty()) {
                    emitInterlockNotification(
                        message,
                        QStringLiteral("tx-filter:%1:%2")
                            .arg(kvs.value(QStringLiteral("filter_low")),
                                 kvs.value(QStringLiteral("filter_high"))));
                }
            }
        }

        static const QRegularExpression xmitRe(R"(^xmit\s+([01])\s*$)", QRegularExpression::CaseInsensitiveOption);
        const auto match = xmitRe.match(trimmed);
        if (match.hasMatch()) {
            const bool tx = (match.captured(1) == "1");
            if (tx) {
                armInterlockNotification(m_pendingTransmitPreflightSource);
                m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
            }
            m_txRequested = tx;
            if (!tx && m_txAudioGate) {
                m_txAudioGate = false;
                emit txAudioGateChanged(false);
            }
        }
        if (cmd == "transmit tune 1" || cmd == "atu start") {
            armInterlockNotification(m_pendingTransmitPreflightSource);
            m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
            applyTuneInhibit();
        }
        sendCmd(cmd);
    });

    // Forward equalizer model commands to the radio
    connect(&m_equalizerModel, &EqualizerModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    // Forward TNF model commands to the radio
    connect(&m_tnfModel, &TnfModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });
    connect(&m_cwxModel, &CwxModel::commandReady, this, [this](const QString& cmd){
        // Track CWX send state so the interlock handler recognises local
        // CWX TX and doesn't force the audio gate off. (#2047, #2097)
        if (cmd.startsWith("cwx send") || cmd.startsWith("cwx macro send"))
            m_cwxActive = true;
        else if (cmd.startsWith("cwx clear"))
            m_cwxActive = false;
        sendCmd(cmd);
    });
    // When the radio signals its CWX buffer is drained, release TX. (#2450)
    // The radio's break-in timer fires but sync_cwx=1 still requires an
    // explicit xmit 0 from the client — without it the radio holds TX for
    // its full hardware interlock timeout (~60 s).
    connect(&m_cwxModel, &CwxModel::queueEmpty, this, [this]() {
        if (!m_cwxActive) return;
        m_cwxActive = false;
        m_transmitModel.setMox(false);
    });
    connect(&m_dvkModel, &DvkModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });
    connect(&m_navtexModel, &NavtexModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });
    connect(&m_navtexModel, &NavtexModel::replyCommandReady, this, [this](const QString& cmd, int seq){
        sendCmd(cmd, [this, seq](int respVal, const QString& body){
            m_navtexModel.handleSendResponse(seq, static_cast<uint>(respVal), body);
        });
    });
    connect(&m_usbCableModel, &UsbCableModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    // Tune PA inhibit: restore TX outputs when tune completes
    connect(&m_transmitModel, &TransmitModel::tuneChanged, this, [this](bool tuning) {
        if (!tuning && m_tuneInhibitActive && m_tuneInhibitBandId >= 0)
            restoreTuneInhibit();
    });

    m_reconnectTimer.setInterval(5000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_intentionalDisconnect && !m_lastInfo.address.isNull()) {
            qCDebug(lcProtocol) << "RadioModel: auto-reconnecting to" << m_lastInfo.address.toString();
            QMetaObject::invokeMethod(m_connection, [this] {
                m_connection->connectToRadio(m_lastInfo);
            });
        } else {
            m_reconnectTimer.stop();
        }
    });

}

RadioModel::~RadioModel()
{
    // Disconnect all signals BEFORE member destruction to prevent
    // use-after-free (ASAN). (#502)
    QObject::disconnect(m_connection, nullptr, this, nullptr);
    QObject::disconnect(m_panStream, nullptr, this, nullptr);

    // Stop connection thread (#502)
    if (m_connection && m_connThread && m_connThread->isRunning()) {
        RadioConnection* connection = m_connection;
        QMetaObject::invokeMethod(connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
        connection->deleteLater();
        m_connThread->quit();
        m_connThread->wait(3000);
    } else {
        delete m_connection;
    }
    if (m_connThread && m_connThread->isRunning()) {
        m_connThread->quit();
        m_connThread->wait(3000);
    }
    m_connection = nullptr;

    // Stop network thread (#502)
    if (m_panStream && m_networkThread && m_networkThread->isRunning()) {
        PanadapterStream* panStream = m_panStream;
        QMetaObject::invokeMethod(panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        panStream->deleteLater();
        m_networkThread->quit();
        m_networkThread->wait(3000);
    } else {
        delete m_panStream;
    }
    if (m_networkThread && m_networkThread->isRunning()) {
        m_networkThread->quit();
        m_networkThread->wait(3000);
    }
    m_panStream = nullptr;
}

bool RadioModel::isConnected() const
{
    return m_connection->isConnected() || (m_wanConn && m_wanConn->isConnected());
}

int RadioModel::maxSlicesForModel(const QString& model)
{
    const QString normalized = model.toUpper();
    // FlexRadio lists slice capacity as independent receivers per model family.
    if (normalized.contains("6700"))
        return 8;
    if (normalized.contains("6600") || normalized.contains("6500")
            || normalized.contains("8600") || normalized.contains("AU-520"))
        return 4;
    if (normalized.contains("6300") || normalized.contains("6400")
            || normalized.contains("8400") || normalized.contains("AU-510"))
        return 2;
    return 4;
}

SliceModel* RadioModel::slice(int id) const
{
    for (auto* s : m_slices)
        if (s->sliceId() == id) return s;
    return nullptr;
}

int RadioModel::activeTxSliceNum() const
{
    for (auto* s : m_slices) {
        if (s && s->isTxSlice())
            return s->sliceId();
    }
    return -1;
}

QString RadioModel::antennaAliasRadioKey() const
{
    QString key = m_chassisSerial.trimmed();
    if (key.isEmpty())
        key = serial().trimmed();
    if (key.isEmpty())
        key = m_model.trimmed();
    if (key.isEmpty())
        key = m_name.trimmed();
    if (key.isEmpty())
        key = m_nickname.trimmed();
    return key.isEmpty() ? QStringLiteral("unconnected") : key;
}

bool RadioModel::reloadAntennaAliases() const
{
    const QString key = antennaAliasRadioKey();
    if (key == m_antennaAliasRadioKey)
        return false;

    const QMap<QString, QString> aliases = AntennaAliasStore::load(key);
    const bool changed = aliases != m_antennaAliases
        || key != m_antennaAliasRadioKey;
    m_antennaAliasRadioKey = key;
    m_antennaAliases = aliases;
    return changed;
}

QString RadioModel::antennaAlias(const QString& token) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::alias(m_antennaAliases, token);
}

QString RadioModel::antennaDisplayName(const QString& token,
                                       bool includeTokenForDisambiguation) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::displayName(
        m_antennaAliases, token, includeTokenForDisambiguation);
}

QString RadioModel::antennaShortDisplayName(const QString& token, int maxChars) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::shortDisplayName(m_antennaAliases, token, maxChars);
}

QMap<QString, QString> RadioModel::antennaAliases() const
{
    reloadAntennaAliases();
    return m_antennaAliases;
}

bool RadioModel::antennaAliasNeedsDisambiguation(const QString& token,
                                                 const QStringList& tokens) const
{
    reloadAntennaAliases();
    const QString a = AntennaAliasStore::alias(m_antennaAliases, token);
    if (a.isEmpty())
        return false;

    int count = 0;
    for (const QString& other : tokens) {
        if (AntennaAliasStore::alias(m_antennaAliases, other) == a)
            ++count;
    }
    return count > 1;
}

void RadioModel::setAntennaAlias(const QString& token, const QString& alias)
{
    if (token.isEmpty())
        return;
    reloadAntennaAliases();

    const QString trimmedAlias = alias.trimmed();
    if (trimmedAlias.isEmpty()) {
        clearAntennaAlias(token);
        return;
    }

    if (m_antennaAliases.value(token) == trimmedAlias)
        return;

    m_antennaAliases.insert(token, trimmedAlias);
    AntennaAliasStore::save(m_antennaAliasRadioKey, m_antennaAliases);
    emit antennaAliasesChanged();
}

void RadioModel::clearAntennaAlias(const QString& token)
{
    if (token.isEmpty())
        return;
    reloadAntennaAliases();
    if (!m_antennaAliases.remove(token))
        return;

    AntennaAliasStore::save(m_antennaAliasRadioKey, m_antennaAliases);
    emit antennaAliasesChanged();
}

QStringList RadioModel::knownAntennaTokens() const
{
    reloadAntennaAliases();
    QStringList tokens;
    for (const QString& ant : m_antList)
        appendUniqueAntennaToken(tokens, ant);
    for (SliceModel* s : m_slices) {
        if (!s)
            continue;
        appendUniqueAntennaToken(tokens, s->rxAntenna());
        appendUniqueAntennaToken(tokens, s->txAntenna());
        for (const QString& ant : s->rxAntennaList())
            appendUniqueAntennaToken(tokens, ant);
        for (const QString& ant : s->txAntennaList())
            appendUniqueAntennaToken(tokens, ant);
    }
    for (auto it = m_antennaAliases.constBegin(); it != m_antennaAliases.constEnd(); ++it)
        appendUniqueAntennaToken(tokens, it.key());
    return tokens;
}

SliceModel* RadioModel::txSlice() const
{
    for (auto* s : m_slices) {
        if (s && s->isTxSlice())
            return s;
    }
    return nullptr;
}

QString RadioModel::localPttInterlockMessage(TransmitModel::PttSource source) const
{
    // CAT/DAX PTT callers acknowledge the request before the asynchronous
    // model path runs, so local preflight must not silently eat their PTT.
    // Let the radio be authoritative and report any resulting interlock.
    if (source == TransmitModel::PttSource::Dax)
        return QString();

    auto* s = txSlice();
    if (!s)
        return QStringLiteral("No transmit slice is assigned.");

    const QString mode = s->mode().toUpper();
    const bool nonVoiceSource = (source == TransmitModel::PttSource::Tune
                              || source == TransmitModel::PttSource::TciHardware
                              || source == TransmitModel::PttSource::Dax
                              || s->sliceId() == m_digitalVoiceTxSliceId);
    if (!nonVoiceSource
        && (mode == QStringLiteral("DIGU") || mode == QStringLiteral("DIGL"))) {
        return QStringLiteral("You cannot transmit voice in DIGU/DIGL mode.");
    }

    if (source == TransmitModel::PttSource::Tune)
        return QString();

    return txFilterFrequencyLimitMessage(m_transmitModel.txFilterLow(),
                                         m_transmitModel.txFilterHigh());
}

QString RadioModel::txFilterFrequencyLimitMessage(int lowHz, int highHz) const
{
    auto* s = txSlice();
    if (!s)
        return QString();

    const double carrierMhz = s->frequency();
    if (carrierMhz <= 0.0)
        return QString();

    const QString bandName = BandSettings::bandForFrequency(carrierMhz);
    if (bandName == QStringLiteral("GEN") || bandName == QStringLiteral("WWV"))
        return QString();

    const BandDef& band = BandSettings::bandDef(bandName);
    if (band.lowMhz <= 0.0 || band.highMhz <= band.lowMhz)
        return QString();

    lowHz = qBound(0, lowHz, 9950);
    highHz = qBound(lowHz + 50, highHz, 10000);

    const QString mode = s->mode().toUpper();
    double txLowMhz = carrierMhz;
    double txHighMhz = carrierMhz;
    if (mode == QStringLiteral("LSB") || mode == QStringLiteral("DIGL")) {
        txLowMhz = carrierMhz - highHz / 1.0e6;
        txHighMhz = carrierMhz - lowHz / 1.0e6;
    } else if (mode == QStringLiteral("USB") || mode == QStringLiteral("DIGU")) {
        txLowMhz = carrierMhz + lowHz / 1.0e6;
        txHighMhz = carrierMhz + highHz / 1.0e6;
    } else if (mode == QStringLiteral("AM") || mode == QStringLiteral("SAM")) {
        txLowMhz = carrierMhz - highHz / 1.0e6;
        txHighMhz = carrierMhz + highHz / 1.0e6;
    } else {
        return QString();
    }

    constexpr double kEdgeToleranceMhz = 0.0000005; // 0.5 Hz
    if (txLowMhz < band.lowMhz - kEdgeToleranceMhz
        || txHighMhz > band.highMhz + kEdgeToleranceMhz) {
        return QStringLiteral("Your TX filter overlaps your frequency limits.");
    }

    return QString();
}

QString RadioModel::radioInterlockNotificationMessage(const QMap<QString, QString>& kvs) const
{
    const QString reason = kvs.value(QStringLiteral("reason")).toUpper();
    const QString state = kvs.value(QStringLiteral("state")).toUpper();

    auto withDebugName = [reason, state](const QString& message) {
        const QString debugName = reason.isEmpty() ? state : reason;
        return debugName.isEmpty()
            ? message
            : QStringLiteral("%1 (%2)").arg(message, debugName);
    };

    if (reason == QStringLiteral("OUT_OF_BAND")
        || reason == QStringLiteral("TUNED_TOO_FAR")
        || reason == QStringLiteral("OUT_OF_PA_RANGE")
        || reason == QStringLiteral("XVTR_RX_ONLY")) {
        return withDebugName(QStringLiteral("You cannot transmit on this frequency."));
    }

    if (reason == QStringLiteral("BAD_MODE")) {
        if (auto* s = txSlice()) {
            const QString mode = s->mode().toUpper();
            const bool nonVoiceSource =
                (m_interlockNotificationSource == TransmitModel::PttSource::Tune
                 || m_interlockNotificationSource == TransmitModel::PttSource::TciHardware
                 || m_interlockNotificationSource == TransmitModel::PttSource::Dax
                 || s->sliceId() == m_digitalVoiceTxSliceId);
            if (!nonVoiceSource
                && (mode == QStringLiteral("DIGU") || mode == QStringLiteral("DIGL"))) {
                return withDebugName(
                    QStringLiteral("You cannot transmit voice in DIGU/DIGL mode."));
            }
        }
        return withDebugName(QStringLiteral("You cannot transmit in this mode."));
    }

    if (reason == QStringLiteral("CLIENT_TX_INHIBIT"))
        return withDebugName(QStringLiteral("Transmit is inhibited for this band."));
    if (reason == QStringLiteral("NO_TX_ASSIGNED"))
        return withDebugName(QStringLiteral("No transmit slice is assigned."));
    if (reason == QStringLiteral("RCA_TXREQ"))
        return withDebugName(QStringLiteral("External RCA TX request is holding transmit."));
    if (reason == QStringLiteral("ACC_TXREQ"))
        return withDebugName(QStringLiteral("External ACC TX request is holding transmit."));
    if (reason == QStringLiteral("AMP:TG") || reason.contains(QStringLiteral("PG-XL")))
        return withDebugName(QStringLiteral("Amplifier interlock is blocking transmit."));

    if (state == QStringLiteral("TIMEOUT"))
        return withDebugName(QStringLiteral("Transmit timed out."));
    if (state == QStringLiteral("STUCK_INPUT"))
        return withDebugName(QStringLiteral("PTT input is stuck active."));
    if (state == QStringLiteral("TX_FAULT"))
        return withDebugName(QStringLiteral("Transmit interlock fault."));

    return QString();
}

void RadioModel::armInterlockNotification(TransmitModel::PttSource source)
{
    m_interlockNotificationArmedUntilMs = QDateTime::currentMSecsSinceEpoch() + 6000;
    m_interlockNotificationSource = source;
}

bool RadioModel::interlockNotificationArmed() const
{
    return QDateTime::currentMSecsSinceEpoch() <= m_interlockNotificationArmedUntilMs;
}

void RadioModel::emitInterlockNotification(const QString& message, const QString& key)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString effectiveKey = key.isEmpty() ? trimmed : key;
    if (effectiveKey == m_lastInterlockNotificationKey
        && now - m_lastInterlockNotificationMs < 5000) {
        return;
    }

    m_lastInterlockNotificationKey = effectiveKey;
    m_lastInterlockNotificationMs = now;
    emit interlockNotificationRequested(trimmed);
}

// ─── Actions ──────────────────────────────────────────────────────────────────

void RadioModel::connectToRadio(const RadioInfo& info)
{
    m_wanConn = nullptr;  // LAN mode
    m_lastInfo = info;
    m_intentionalDisconnect = false;
    m_forcedDisconnectInProgress = false;
    m_announcedClientConnections.clear();
    m_reconnectTimer.stop();
    m_name    = info.name;
    m_model   = info.model;
    m_version = info.version;
    m_maxSlices = maxSlicesForModel(m_model);
    if (reloadAntennaAliases())
        emit antennaAliasesChanged();
    setKnownGuiClients(info.guiClientHandles,
                       info.guiClientPrograms,
                       info.guiClientStations,
                       info.guiClientIps,
                       info.guiClientHosts);
    QMetaObject::invokeMethod(m_connection, [conn = m_connection, info] {
        conn->connectToRadio(info);
    });
}

void RadioModel::connectViaWan(WanConnection* wan, const QString& publicIp, quint16 udpPort)
{
    qCDebug(lcProtocol) << "RadioModel: connectViaWan publicIp=" << publicIp
             << "udpPort=" << udpPort
             << "wanHandle=0x" << QString::number(wan->clientHandle(), 16);

    // Disconnect any stale signal connections from a previous WAN session
    if (m_wanConn)
        m_wanConn->disconnect(this);

    m_wanConn = wan;
    m_wanPublicIp = publicIp;
    m_wanUdpPort = udpPort;
    m_intentionalDisconnect = false;
    m_forcedDisconnectInProgress = false;
    m_announcedClientConnections.clear();
    m_reconnectTimer.stop();

    // Wire WAN connection signals (same as RadioConnection)
    connect(wan, &WanConnection::connected, this, &RadioModel::onConnected);
    connect(wan, &WanConnection::disconnected, this, &RadioModel::onDisconnected);
    connect(wan, &WanConnection::errorOccurred, this, &RadioModel::onConnectionError);
    connect(wan, &WanConnection::versionReceived, this, &RadioModel::onVersionReceived);
    connect(wan, &WanConnection::messageReceived, this, &RadioModel::onMessageReceived);
    connect(wan, &WanConnection::statusReceived, this, &RadioModel::onStatusReceived);
    connect(wan, &WanConnection::pingRttMeasured, this, [this](int ms) {
        m_pingMissCount = 0;
        m_lastPingRtt = ms;
        evaluateNetworkQuality();
        emit pingReceived();
    });

    // The WAN connection is already established (TLS + wan validate done)
    // and has already received V/H. Trigger onConnected manually.
    if (wan->isConnected()) {
        qCDebug(lcProtocol) << "RadioModel: WAN already connected, triggering onConnected";
        onConnected();
    } else {
        qCDebug(lcProtocol) << "RadioModel: WAN not yet connected, waiting for connected signal";
    }
}

void RadioModel::setPendingClientDisconnects(const QList<quint32>& handles)
{
    m_pendingClientDisconnects.clear();
    for (quint32 handle : handles) {
        if (handle != 0 && !m_pendingClientDisconnects.contains(handle))
            m_pendingClientDisconnects.append(handle);
    }
}

void RadioModel::setKnownGuiClients(const QStringList& handles,
                                    const QStringList& programs,
                                    const QStringList& stations,
                                    const QStringList& ips,
                                    const QStringList& hosts)
{
    applyKnownGuiClients(handles, programs, stations, ips, hosts, true);
}

void RadioModel::mergeKnownGuiClients(const QStringList& handles,
                                      const QStringList& programs,
                                      const QStringList& stations,
                                      const QStringList& ips,
                                      const QStringList& hosts)
{
    applyKnownGuiClients(handles, programs, stations, ips, hosts, false);
}

void RadioModel::applyKnownGuiClients(const QStringList& handles,
                                      const QStringList& programs,
                                      const QStringList& stations,
                                      const QStringList& ips,
                                      const QStringList& hosts,
                                      bool replaceExisting)
{
    if (replaceExisting) {
        m_clientStations.clear();
        m_clientInfoMap.clear();
        m_startupClientConnections.clear();
    }

    for (int i = 0; i < handles.size(); ++i) {
        const quint32 handle = parseClientHandle(handles[i]);
        if (handle == 0)
            continue;
        if (replaceExisting)
            m_startupClientConnections.insert(handle);

        const QString program = i < programs.size()
            ? cleanClientText(programs[i])
            : QStringLiteral("Unknown");
        const QString station = i < stations.size()
            ? cleanClientText(stations[i])
            : program;
        QString source = i < ips.size()
            ? cleanClientText(ips[i])
            : QString();
        if (source.isEmpty() && i < hosts.size())
            source = cleanClientText(hosts[i]);

        ClientInfo client = m_clientInfoMap.value(handle);
        if (!station.isEmpty())
            client.station = station;
        if (!program.isEmpty() && program != QStringLiteral("Unknown"))
            client.program = program;
        if (!source.isEmpty())
            client.source = source;

        m_clientStations[handle] = client.station.isEmpty() ? client.program : client.station;
        m_clientInfoMap[handle] = client;
    }
}

bool RadioModel::shouldSuppressClientConnectionNotice(quint32 handle)
{
    if (handle == 0 || handle == clientHandle())
        return true;

    if (m_startupClientConnections.remove(handle)) {
        m_announcedClientConnections.insert(handle);
        return true;
    }

    if (m_clientConnectionNoticeTimer.isValid()
            && m_clientConnectionNoticeTimer.elapsed() < CLIENT_CONNECTION_STARTUP_SUPPRESS_MS) {
        m_announcedClientConnections.insert(handle);
        return true;
    }

    return false;
}

void RadioModel::announceClientConnection(quint32 handle,
                                          const QString& source,
                                          const QString& station,
                                          const QString& program)
{
    if (handle == clientHandle() || m_announcedClientConnections.contains(handle))
        return;

    m_announcedClientConnections.insert(handle);
    QTimer::singleShot(750, this, [this, handle, source, station, program] {
        if (!m_clientInfoMap.contains(handle))
            return;

        const auto client = m_clientInfoMap.value(handle);
        QString latestSource = client.source.isEmpty() ? source : client.source;
        QString latestStation = client.station.isEmpty() ? station : client.station;
        QString latestProgram = client.program.isEmpty() ? program : client.program;

        if (m_wanConn && (latestSource.isEmpty() || latestSource == QStringLiteral("SmartLink"))) {
            QTimer::singleShot(1250, this, [this, handle, latestSource, latestStation, latestProgram] {
                if (!m_clientInfoMap.contains(handle))
                    return;

                const auto client = m_clientInfoMap.value(handle);
                emit clientConnected(handle,
                                     client.source.isEmpty() ? latestSource : client.source,
                                     client.station.isEmpty() ? latestStation : client.station,
                                     client.program.isEmpty() ? latestProgram : client.program);
            });
            return;
        }

        emit clientConnected(handle, latestSource, latestStation, latestProgram);
    });
}

void RadioModel::disconnectFromRadio()
{
    m_intentionalDisconnect = true;
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    if (m_wanConn) {
        WanConnection* wan = m_wanConn;
        wan->disconnect(this);  // remove stale signal connections before adding the one-shot teardown (#224)
        connect(wan, &WanConnection::disconnected, this, [this, wan]() {
            if (m_wanConn == wan) {
                onDisconnected();
            }
        }, Qt::SingleShotConnection);
        wan->disconnectFromRadio();
        if (wan->isSocketIdle() && m_wanConn == wan) {
            onDisconnected();
        }
    } else if (m_connection->isConnected()) {
        // Graceful disconnect: remove our stream and wait for the radio reply
        // before closing. Self "client disconnect" is rejected by the radio.
        quint32 handle = clientHandle();
        QString streamId = RadioStatusOwnership::streamCommandId(m_rxAudio.streamId);
        const quint32 streamRemoveSeq = streamId.isEmpty() ? 0 : m_seqCounter.fetch_add(1);
        QMetaObject::invokeMethod(m_connection, [this, handle, streamId,
                                                 streamRemoveSeq]() {
            m_connection->gracefulDisconnect(handle, streamId, streamRemoveSeq);
        }, Qt::BlockingQueuedConnection);
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
    }
}

void RadioModel::forceDisconnect()
{
    // Close TCP/TLS without setting m_intentionalDisconnect so the UI can
    // start the normal unexpected-disconnect reconnect path.
    if (m_wanConn) {
        m_wanConn->disconnectFromRadio();
    } else if (m_connection->isConnected()) {
        quint32 handle = clientHandle();
        QMetaObject::invokeMethod(m_connection, [conn = m_connection, handle]() {
            conn->gracefulDisconnect(handle, QString(), 0);
        });
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio);
    }
}

void RadioModel::setTransmit(bool tx, TransmitModel::PttSource source)
{
    if (tx) {
        const QString message = localPttInterlockMessage(source);
        if (!message.isEmpty()) {
            emitInterlockNotification(
                message,
                QStringLiteral("local-ptt:%1").arg(message));
            m_transmitModel.setTransmitting(false);
            return;
        }
        armInterlockNotification(source);
    }

    // Track local intent so we can keep TX gating aligned with user/PTT edges
    // while radio interlock transitions through intermediate states.
    m_txRequested = tx;

    // Optimistic edge gating:
    // - TX on: start immediately to keep modem waveform aligned with PTT edge.
    // - TX off: stop immediately to avoid "stuck TX tail" during UNKEY_REQUESTED.
    m_transmitModel.setTransmitting(tx);
    if (!tx && m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }

    sendCmd(QString("xmit %1").arg(tx ? 1 : 0));
}

void RadioModel::setDigitalVoiceTxSlice(int sliceId)
{
    m_digitalVoiceTxSliceId = sliceId;
}

QString RadioModel::audioCompressionParam() const
{
    QString setting = AppSettings::instance().value("AudioCompression", "None").toString();
    if (setting == "Opus") return "opus";
    if (setting == "None") return "none";
    // Auto: use Opus on WAN, uncompressed on LAN
    return isWan() ? "opus" : "none";
}

void RadioModel::sendCwKey(bool down, const QString& debugSource,
                           quint64 debugTraceId, quint64 debugSourceMs)
{
    const bool prev = m_cwKeyActive;
    m_cwKeyActive = down;
    // Send only the key edge — the radio's break-in setting decides whether
    // it transmits.  With break_in=1 (QSK), `cw key 1` triggers TX and
    // break_in_delay holds the relay between elements.  With break_in=0,
    // the radio queues the key but doesn't transmit until the operator
    // explicitly asserts CW PTT (Space PTT, MOX, or hardware PTT) — the
    // standard semi-break-in workflow per FlexLib Radio.cs:8890–8965.
    sendNetCwCommand(QString("cw key %1").arg(down ? 1 : 0),
                     debugSource, debugTraceId, debugSourceMs);
    if (prev != down)
        emit cwKeyDownChanged(down);
}

void RadioModel::sendCwPaddle(bool dit, bool dah, const QString& debugSource,
                              quint64 debugTraceId, quint64 debugSourceMs)
{
    // The radio's CW protocol does NOT accept a 2-arg paddle form like
    // `cw key dit dah` — FlexLib only ever sends `cw key 1` or `cw key 0`
    // (single state) and expects the client to do iambic timing locally.
    // Treat any paddle press as a straight-key down so this path still
    // works when the local iambic keyer is disabled.  When the keyer IS
    // running it intercepts upstream and uses sendCwPtt + sendCwKeyEdge
    // directly, bypassing this method.
    sendCwKey(dit || dah, debugSource, debugTraceId, debugSourceMs);
}

void RadioModel::sendCwPtt(bool on, const QString& debugSource,
                           quint64 debugTraceId, quint64 debugSourceMs)
{
    sendNetCwCommand(on ? QStringLiteral("cw ptt 1") : QStringLiteral("cw ptt 0"),
                     debugSource, debugTraceId, debugSourceMs);
}

void RadioModel::sendCwKeyEdge(bool down, const QString& debugSource,
                               quint64 debugTraceId, quint64 debugSourceMs)
{
    const bool prev = m_cwKeyActive;
    m_cwKeyActive = down;
    sendNetCwCommand(QString("cw key %1").arg(down ? 1 : 0),
                     debugSource, debugTraceId, debugSourceMs);
    if (prev != down)
        emit cwKeyDownChanged(down);
}

// ── NetCW stream — VITA-49 UDP delivery with redundant sends ────────────────

QByteArray RadioModel::buildNetCwPacket(const QByteArray& payload)
{
    // VITA-49 header (28 bytes) + ASCII command payload.  Working Maestro
    // captures show the payload null-padded to a 32-bit word boundary; keep
    // the datagram length consistent with the VRT packet_size field.
    const int payloadBytes = payload.size();
    const int paddedPayloadBytes = (payloadBytes + 3) & ~3;
    const int packetWords = static_cast<int>(std::ceil(payloadBytes / 4.0) + 7); // 7 header words
    const int packetBytes = 28 + paddedPayloadBytes;

    QByteArray pkt(packetBytes, '\0');
    auto* w = reinterpret_cast<quint32*>(pkt.data());

    // Word 0: ExtDataWithStream, C=1, T=0, TSI=3(Other), TSF=1(SampleCount)
    static int pktCount = 0;
    quint32 hdr = (0x3u << 28)     // pkt_type = ExtDataWithStream
                | (1u << 27)       // C = 1 (class ID present)
                | (0x3u << 22)     // TSI = 3 (Other)
                | (0x1u << 20)     // TSF = 1 (SampleCount)
                | ((pktCount & 0x0F) << 16)
                | (packetWords & 0xFFFF);
    pktCount = (pktCount + 1) & 0x0F;

    w[0] = qToBigEndian(hdr);
    w[1] = qToBigEndian(m_netCwStreamId);
    w[2] = qToBigEndian<quint32>(0x00001C2D);      // OUI (FlexRadio)
    w[3] = qToBigEndian<quint32>(0x534C03E3);       // ICC=0x534C, PCC=0x03E3
    w[4] = 0; w[5] = 0; w[6] = 0;                  // timestamps

    // Payload: ASCII command string
    memcpy(pkt.data() + 28, payload.constData(), payloadBytes);

    return pkt;
}

void RadioModel::sendNetCwCommand(const QString& baseCmd, const QString& debugSource,
                                  quint64 debugTraceId, quint64 debugSourceMs)
{
    if (m_netCwStreamId == 0) {
        // No netcw stream — fall back to TCP immediate
        const QString fallbackCmd = baseCmd.contains("cw key")
            ? QString(baseCmd).replace("cw key", "cw key immediate")
            : baseCmd;
        if (lcCw().isDebugEnabled()) {
            const quint64 now = cwTraceNowMs();
            qCDebug(lcCw).noquote().nospace()
                << "CW netcw fallback trace=" << debugTraceId
                << " t=" << now << "ms"
                << " sinceSourceMs=" << (debugSourceMs ? static_cast<qint64>(now - debugSourceMs) : -1)
                << " source=" << (debugSource.isEmpty() ? QStringLiteral("unknown") : debugSource)
                << " cmd=\"" << fallbackCmd << "\"";
        }
        sendCmd(fallbackCmd);
        return;
    }

    // Build the full command with timing metadata and dedup index
    // FlexLib format: "cw key 1 time=0x<hex_ms> index=<N> client_handle=0x<handle>".
    // The time value is a 16-bit relative millisecond counter, not an epoch
    // timestamp.  Flex clients reset it after a short idle gap; the radio
    // accepts 0x0000 as a timing resync marker.
    constexpr qint64 kNetCwIdleResetMs = 3000;
    quint16 timeMs = 0;
    if (!m_netCwClock.isValid()
        || m_netCwLastSendMs < 0
        || (m_netCwClock.elapsed() - m_netCwLastSendMs) > kNetCwIdleResetMs) {
        if (m_netCwClock.isValid())
            m_netCwClock.restart();
        else
            m_netCwClock.start();
        m_netCwLastSendMs = 0;
    } else {
        const qint64 elapsed = m_netCwClock.elapsed();
        timeMs = static_cast<quint16>(elapsed & 0xFFFF);
        m_netCwLastSendMs = elapsed;
    }
    int index = m_netCwIndex++;

    // FlexLib formats hex values UPPERCASE (C# ToString("X")), and the
    // radio's status messages do too (e.g. `S23A59BDF|...`) — the netcw
    // parser appears to be case-sensitive on `client_handle`.  Match that
    // by formatting both hex values uppercase explicitly.
    const QString tsHex = QString("%1").arg(timeMs, 4, 16, QChar('0')).toUpper();
    const QString chHex = QString("%1").arg(clientHandle(), 0, 16).toUpper();
    QString fullCmd = QString("%1 time=0x%2 index=%3 client_handle=0x%4")
        .arg(baseCmd, tsHex, QString::number(index), chHex);

    QByteArray payload = fullCmd.toLatin1();

    // Redundant sends via UDP: 0ms, 5ms, 10ms, 15ms.  The radio dedupes by the
    // ASCII `index=N` field, but each datagram needs a UNIQUE VITA-49
    // packet_count — FlexLib's NetCWStream.AddTXData increments packet_count
    // after each ToBytesTX(), so the four redundant copies arrive with counts
    // N, N+1, N+2, N+3.  Reusing a single buffer (same packet_count on all
    // four) makes the radio's VITA stream layer drop them as duplicates.
    QByteArray packet0 = buildNetCwPacket(payload);
    QByteArray packet1 = buildNetCwPacket(payload);
    QByteArray packet2 = buildNetCwPacket(payload);
    QByteArray packet3 = buildNetCwPacket(payload);
    const quint64 scheduledMs = cwTraceNowMs();
    const QString source = debugSource.isEmpty() ? QStringLiteral("unknown") : debugSource;

    if (lcCw().isDebugEnabled()) {
        qCDebug(lcCw).noquote().nospace()
            << "CW netcw schedule trace=" << debugTraceId
            << " t=" << scheduledMs << "ms"
            << " sinceSourceMs=" << (debugSourceMs ? static_cast<qint64>(scheduledMs - debugSourceMs) : -1)
            << " source=" << source
            << " stream=0x" << QString::number(m_netCwStreamId, 16).toUpper()
            << " index=" << index
            << " time=0x" << tsHex
            << " cmd=\"" << baseCmd << "\""
            << " payloadBytes=" << payload.size()
            << " packetBytes=" << packet0.size()
            << " udpCopies=4 tcpBackstop=1";
    }

    auto logUdpSend = [debugTraceId, scheduledMs, source](int copy, int delayMs, int bytes) {
        if (!lcCw().isDebugEnabled())
            return;
        const quint64 now = cwTraceNowMs();
        qCDebug(lcCw).noquote().nospace()
            << "CW netcw udp-send trace=" << debugTraceId
            << " t=" << now << "ms"
            << " source=" << source
            << " copy=" << copy
            << " delayMs=" << delayMs
            << " actualDelayMs=" << static_cast<qint64>(now - scheduledMs)
            << " timerSlipMs=" << (static_cast<qint64>(now - scheduledMs) - delayMs)
            << " bytes=" << bytes;
    };

    QMetaObject::invokeMethod(m_panStream, [this, packet0, logUdpSend]() {
        logUdpSend(0, 0, packet0.size());
        m_panStream->sendToRadio(packet0);
    }, Qt::QueuedConnection);

    QTimer::singleShot(5, this, [this, packet1, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet1, logUdpSend]() {
            logUdpSend(1, 5, packet1.size());
            m_panStream->sendToRadio(packet1);
        }, Qt::QueuedConnection);
    });
    QTimer::singleShot(10, this, [this, packet2, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet2, logUdpSend]() {
            logUdpSend(2, 10, packet2.size());
            m_panStream->sendToRadio(packet2);
        }, Qt::QueuedConnection);
    });
    QTimer::singleShot(15, this, [this, packet3, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet3, logUdpSend]() {
            logUdpSend(3, 15, packet3.size());
            m_panStream->sendToRadio(packet3);
        }, Qt::QueuedConnection);
    });

    // FlexLib sends the same decorated netcw command over TCP after the UDP
    // copies.  With the 16-bit timestamp format above, the radio can dedupe
    // by index=N and the TCP path provides a reliable delivery backstop.
    sendCmd(fullCmd);
}

void RadioModel::cwAutoTune(int sliceId, bool intermittent)
{
    if (intermittent) {
        sendCmd(QString("slice auto_tune %1 int=1").arg(sliceId));
    } else {
        // int=0 stops the autotune engine (FlexLib: isIntermittent=false)
        sendCmd(QString("slice auto_tune %1 int=0").arg(sliceId));
    }
}

void RadioModel::cwAutoTuneOnce(int sliceId)
{
    // One-shot autotune (FlexLib: isIntermittent=null)
    sendCmd(QString("slice auto_tune %1").arg(sliceId));
}

void RadioModel::addSlice()
{
    if (m_activePanId.isEmpty()) {
        qCWarning(lcProtocol) << "RadioModel::addSlice: no panadapter, cannot create slice";
        return;
    }

    // Create a new slice offset from existing slices so VFO flags deconflict.
    // Use pan center, but if an existing slice is within 5 kHz, offset by
    // 20% of the visible bandwidth.
    auto* pan = activePanadapter();
    double newFreq = pan ? pan->centerMhz() : 14.1;
    const double offsetMhz = (pan ? pan->bandwidthMhz() : 0.2) * 0.2;  // 20% of visible BW
    for (auto* s : m_slices) {
        if (std::abs(s->frequency() - newFreq) < 0.005) {  // within 5 kHz
            newFreq += offsetMhz;
            break;
        }
    }
    const QString freq = QString::number(newFreq, 'f', 6);
    const QString cmd = QString("slice create pan=%1 freq=%2").arg(m_activePanId, freq);

    qCDebug(lcProtocol) << "RadioModel::addSlice:" << cmd;
    sendCmd(cmd, [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel: slice create failed, code"
                       << Qt::hex << code << "body:" << body;
            emit sliceCreateFailed(maxSlices(), m_model);
        } else {
            qCDebug(lcProtocol) << "RadioModel: new slice created, index =" << body;
        }
    });
}

void RadioModel::addSliceOnPan(const QString& panId)
{
    if (panId.isEmpty()) { addSlice(); return; }

    auto* pan = panadapter(panId);
    double newFreq = pan ? pan->centerMhz() : 14.1;
    const double offsetMhz = (pan ? pan->bandwidthMhz() : 0.2) * 0.2;
    for (auto* s : m_slices) {
        if (std::abs(s->frequency() - newFreq) < 0.005) {
            newFreq += offsetMhz;
            break;
        }
    }
    const QString freq = QString::number(newFreq, 'f', 6);
    const QString cmd = QString("slice create pan=%1 freq=%2").arg(panId, freq);

    qCDebug(lcProtocol) << "RadioModel::addSliceOnPan:" << cmd;
    sendCmd(cmd, [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel: slice create failed, code"
                       << Qt::hex << code << "body:" << body;
            emit sliceCreateFailed(maxSlices(), m_model);
        } else {
            qCDebug(lcProtocol) << "RadioModel: new slice created, index =" << body;
        }
    });
}

void RadioModel::createPanadapter()
{
    int limit = maxPanadapters();
    if (static_cast<int>(m_panadapters.size()) >= limit) {
        qCWarning(lcProtocol) << "RadioModel::createPanadapter: limit of" << limit
                              << "panadapters reached for model" << m_model;
        emit panadapterLimitReached(limit, m_model);
        return;
    }
    const auto handleCreatedPan = [this](const QString& source, int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel:" << source << "failed, code"
                                  << Qt::hex << code << "body:" << body;
            emit panadapterLimitReached(maxPanadapters(), m_model);
            return;
        }
        const QString panId = parsePanadapterCreateId(body);

        qCDebug(lcProtocol) << "RadioModel: new panadapter created, pan_id =" << panId;

        if (!panId.isEmpty()) {
            ensureOwnedPanadapter(panId);
            QTimer::singleShot(200, this, [this, panId]() {
                sendCmd(QString("display pan set %1 xpixels=1024 ypixels=700").arg(panId));
                sendCmd(QString("display pan set %1 min_dbm=-130 max_dbm=-40").arg(panId));
            });
        }
    };

    qCDebug(lcProtocol) << "RadioModel::createPanadapter: sending display panafall create";
    sendCmd("display panafall create x=100 y=100", [this, handleCreatedPan](int code, const QString& body) {
        if (code == 0) {
            handleCreatedPan(QStringLiteral("display panafall create"), code, body);
            return;
        }

        qCWarning(lcProtocol) << "RadioModel: display panafall create failed, code"
                              << Qt::hex << code << "body:" << body
                              << "- trying legacy panadapter create";
        sendCmd("panadapter create",
                [handleCreatedPan](int legacyCode, const QString& legacyBody) {
            handleCreatedPan(QStringLiteral("panadapter create"), legacyCode, legacyBody);
        });
    });
}

void RadioModel::removePanadapter(const QString& panId)
{
    qCDebug(lcProtocol) << "RadioModel::removePanadapter:" << panId;
    sendCmd(QString("display pan close %1").arg(panId));
    // Radio will send "display pan <id> removed" → handled in onStatusReceived
}

// ── Pan accessor implementations ──────────────────────────────────────────────

PanadapterModel* RadioModel::activePanadapter() const
{
    return m_panadapters.value(m_activePanId, nullptr);
}

PanadapterModel* RadioModel::panadapter(const QString& panId) const
{
    return m_panadapters.value(panId, nullptr);
}

double RadioModel::panCenterMhz() const
{
    auto* p = activePanadapter();
    return p ? p->centerMhz() : 14.1;
}

double RadioModel::panBandwidthMhz() const
{
    auto* p = activePanadapter();
    return p ? p->bandwidthMhz() : 0.2;
}

void RadioModel::setPanBandwidth(double bandwidthMhz)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 bandwidth=%2")
            .arg(m_activePanId).arg(bandwidthMhz, 0, 'f', 6));
}

void RadioModel::setPanCenter(double centerMhz)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 center=%2")
            .arg(m_activePanId).arg(centerMhz, 0, 'f', 6));
}

void RadioModel::setPanDbmRange(float minDbm, float maxDbm)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 min_dbm=%2 max_dbm=%3")
            .arg(m_activePanId)
            .arg(static_cast<double>(minDbm), 0, 'f', 2)
            .arg(static_cast<double>(maxDbm), 0, 'f', 2));
}

void RadioModel::setPanWnb(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 wnb=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

void RadioModel::setPanWnbLevel(int level)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 wnb_level=%2").arg(m_activePanId).arg(level));
}

void RadioModel::setPanRfGain(int gain)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 rfgain=%2").arg(m_activePanId).arg(gain));
}

// ── Display controls — FFT ─────────────────────────────────────────────────

void RadioModel::setPanAverage(int frames)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 average=%2").arg(m_activePanId).arg(frames));
}

void RadioModel::setPanFps(int fps)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 fps=%2").arg(m_activePanId).arg(fps));
}

void RadioModel::setPanWeightedAverage(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 weighted_average=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

// ── Display controls — Waterfall ──────────────────────────────────────────

void RadioModel::setWaterfallColorGain(int gain)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 color_gain=%2").arg(activeWfId()).arg(gain));
}

void RadioModel::setWaterfallBlackLevel(int level)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 black_level=%2").arg(activeWfId()).arg(level));
}

void RadioModel::setWaterfallAutoBlack(bool on)
{
    Q_UNUSED(on);
    // Auto-black is handled client-side. Always keep radio's auto_black off
    // because its algorithm targets SmartSDR's rendering, not ours.
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 auto_black=0").arg(activeWfId()));
}

void RadioModel::setWaterfallLineDuration(int ms)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 line_duration=%2").arg(activeWfId()).arg(ms));
}

void RadioModel::setPanNoiseFloorPosition(int pos)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 noise_floor_position=%2").arg(m_activePanId).arg(pos));
}

void RadioModel::setPanNoiseFloorEnable(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 noise_floor_position_enable=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

// ─── Connection slots ─────────────────────────────────────────────────────────

void RadioModel::onConnected()
{
    qCDebug(lcProtocol) << "RadioModel: connected";
    m_reconnectTimer.stop();
    m_clientConnectionNoticeTimer.restart();
    setActivePanResized(false);

    // Inhibit system sleep while connected if the user has opted in (#1420)
    if (AppSettings::instance().value("InhibitSleepWhileConnected", "False").toString() == "True")
        m_sleepInhibitor.acquire("AetherSDR connected to radio");

    emit connectionStateChanged(true);
    // Delay network monitor until after client gui registration
    // (pings sent before registration cause "Malformed command" on WAN)

    // Register as GUI client FIRST — required before subscriptions,
    // especially on WAN/SmartLink where the radio is stricter.
    const QString clientId = AppSettings::instance().value("GUIClientID").toString();

    disconnectPendingClientsThen([this, clientId] {
        if (m_wanConn) {
            // On WAN: wait for client ip response before sending client gui.
            // The radio needs time after wan validate to accept GUI registration.
            // multiFLEX conflict on WAN is caught pre-connection via licensedClients.
            sendCmd("client ip", [this, clientId](int, const QString& body) {
                qCDebug(lcProtocol) << "RadioModel: client ip ->" << body.trimmed();
                registerAsGuiClient(clientId);
            });
        } else {
            // On LAN: peek at radio/client status before sending client gui so we
            // can detect a multiFLEX conflict regardless of what discovery provided.
            peekForMultiFlexConflictThen([this, clientId] {
                registerAsGuiClient(clientId);
            });
        }
    });
}

void RadioModel::disconnectPendingClientsThen(std::function<void()> continuation)
{
    QList<quint32> handles;
    const quint32 ours = clientHandle();
    for (quint32 handle : m_pendingClientDisconnects) {
        if (handle != 0 && handle != ours && !handles.contains(handle))
            handles.append(handle);
    }
    m_pendingClientDisconnects.clear();

    if (handles.isEmpty()) {
        continuation();
        return;
    }

    auto remaining = std::make_shared<QList<quint32>>(handles);
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, remaining, continuation, step]() mutable {
        if (remaining->isEmpty()) {
            QTimer::singleShot(250, this, [continuation]() mutable {
                continuation();
            });
            return;
        }

        const quint32 handle = remaining->takeFirst();
        const QString command = QString("client disconnect 0x%1").arg(handle, 0, 16);
        qCDebug(lcProtocol) << "RadioModel: disconnecting occupied client" << Qt::hex << handle;
        sendCmd(command, [handle, step](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcProtocol) << "RadioModel: client disconnect failed for"
                                      << Qt::hex << handle
                                      << "code" << code
                                      << "body:" << body;
            }
            (*step)();
        });
    };

    (*step)();
}

void RadioModel::peekForMultiFlexConflictThen(std::function<void()> continuation)
{
    m_multiFlexContinuation = continuation;

    // Evict stale entries pre-populated from discovery or a previous session.
    // We rebuild the map from scratch using only what the radio confirms via
    // sub client all below, so we never show phantom handles in the dialog.
    const quint32 ours = clientHandle();
    for (auto it = m_clientInfoMap.begin(); it != m_clientInfoMap.end(); ) {
        it = (it.key() != ours) ? m_clientInfoMap.erase(it) : std::next(it);
    }
    m_clientStations.clear();

    // Subscribe to radio and client topics early — before client gui — to get
    // mf_enable and the live connected-client list directly from the radio.
    // 400 ms is enough for the radio's status burst to arrive on a LAN path.
    sendCmd("sub radio all", [this](int, const QString&) {
        sendCmd("sub client all", [this](int, const QString&) {
            QTimer::singleShot(400, this, [this] {
                const quint32 ours2 = clientHandle();
                bool hasOthers = false;
                for (auto it = m_clientInfoMap.cbegin(); it != m_clientInfoMap.cend(); ++it) {
                    if (it.key() != ours2) {
                        hasOthers = true;
                        break;
                    }
                }

                // If the map is still empty (only our own handle or nothing) after
                // waiting for the burst, the radio either hasn't sent any client
                // status yet or the burst arrived after the window closed.  Log so
                // field reports of missed conflicts are diagnosable.  The connection
                // proceeds — the alternative is a hang, which is worse than a miss.
                if (m_clientInfoMap.isEmpty() || (m_clientInfoMap.size() == 1 && m_clientInfoMap.contains(ours2)))
                    qCWarning(lcProtocol) << "RadioModel: peek window closed with no client status received —"
                                            " conflict detection may have been missed (busy radio or lossy LAN)";

                if (!m_multiFlexEnabled && hasOthers) {
                    qCDebug(lcProtocol) << "RadioModel: multiFLEX disabled, other clients present — pausing connection";
                    emit multiFlexConflictDetected();
                    return;
                }

                // No conflict — proceed with registration.
                if (m_multiFlexContinuation) {
                    auto cont = std::move(m_multiFlexContinuation);
                    m_multiFlexContinuation = nullptr;
                    cont();
                }
            });
        });
    });
}

void RadioModel::resolveMultiFlexConflict(quint32 handle)
{
    // Move the continuation out so peekForMultiFlexConflictThen can safely
    // re-assign m_multiFlexContinuation without trampling over it.
    auto continuation = std::move(m_multiFlexContinuation);
    m_multiFlexContinuation = nullptr;

    const QString cmd = QString("client disconnect 0x%1").arg(handle, 0, 16);
    qCDebug(lcProtocol) << "RadioModel: resolving multiFLEX conflict, disconnecting" << Qt::hex << handle;
    sendCmd(cmd, [this, continuation = std::move(continuation)](int code, const QString& body) mutable {
        if (code != 0)
            qCWarning(lcProtocol) << "RadioModel: conflict client disconnect failed:" << body.trimmed();
        // After eviction, re-run the full peek rather than jumping straight to the
        // continuation. This catches any remaining clients (e.g., two sessions, or
        // a stale handle that hadn't been cleaned up yet) and re-shows the dialog if
        // needed, preventing phantom slices from a partially-disconnected session.
        QTimer::singleShot(250, this, [this, continuation = std::move(continuation)]() mutable {
            peekForMultiFlexConflictThen(std::move(continuation));
        });
    });
}

void RadioModel::cancelMultiFlexConflict()
{
    m_multiFlexContinuation = nullptr;
    m_intentionalDisconnect = true;
    QMetaObject::invokeMethod(m_connection, [conn = m_connection] {
        conn->disconnectFromRadio();
    });
}

void RadioModel::handleForcedClientDisconnect()
{
    if (m_forcedDisconnectInProgress)
        return;

    m_forcedDisconnectInProgress = true;
    m_intentionalDisconnect = true;
    m_reconnectTimer.stop();

    qCWarning(lcProtocol) << "RadioModel: this GUI client was force-disconnected by another client";
    emit forcedDisconnectRequested();

    if (m_wanConn) {
        m_wanConn->disconnectFromRadio();
        return;
    }

    const quint32 handle = clientHandle();
    const QString streamId = RadioStatusOwnership::streamCommandId(m_rxAudio.streamId);
    if (m_connection->isConnected()) {
        const quint32 streamRemoveSeq = streamId.isEmpty() ? 0 : m_seqCounter.fetch_add(1);
        QMetaObject::invokeMethod(m_connection, [conn = m_connection, handle, streamId,
                                                 streamRemoveSeq]() {
            conn->gracefulDisconnect(handle, streamId, streamRemoveSeq);
        });
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio);
    }
}

void RadioModel::registerAsGuiClient(const QString& clientId)
{
    // Match FlexLib connect-sequence ordering (Radio.cs:2230-2247):
    //   client program <name>  →  client low_bw_connect  →  client gui
    // The radio's protocol state machine requires client identity (program)
    // BEFORE accepting client-mode configuration (low_bw_connect), and the
    // bandwidth mode must be set BEFORE GUI registration so it is baked
    // into the client session.  Sending these out of order — as we did
    // previously — caused the radio to silently ignore low_bw_connect
    // (#2447: "Low Bandwidth checkbox doesn't do anything meaningful").
    sendCmd("client program AetherSDR");
    if (AppSettings::instance().value("LowBandwidthConnect", "False").toString() == "True")
        sendCmd("client low_bw_connect");

    sendCmd(QString("client gui %1").arg(clientId), [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel: client gui failed, code" << Qt::hex << code;
        } else if (!body.trimmed().isEmpty()) {
            // Save our UUID for session persistence across restarts.
            // The radio restores slices/frequencies for a known UUID.
            auto& s = AppSettings::instance();
            s.setValue("GUIClientID", body.trimmed());
            s.save();
            qCDebug(lcProtocol) << "RadioModel: saved GUIClientID:" << body.trimmed();
        }

        QString station = AppSettings::instance().value("StationName", "").toString();
        if (station.isEmpty()) {
            station = QSysInfo::machineHostName();
        }
        sendCmd(QString("client station %1").arg(station));
        sendCmd("client set send_reduced_bw_dax=1");
        // Set network MTU for VITA-49 packets (matches FlexLib behavior)
        int mtu = AppSettings::instance().value("NetworkMtu", "1450").toInt();
        sendCmd(QString("client set enforce_network_mtu=1 network_mtu=%1").arg(mtu));
        // Enable keepalive (matches FlexLib behavior) — ping timer starts in startNetworkMonitor()
        sendCmd("keepalive enable");
        startNetworkMonitor();

    // Full command sequence — each step waits for its R response before sending the next.
    // sub slice all → sub pan all → sub tx all → sub atu all → sub amplifier all
    //   → sub meter all → sub audio all → ...
    sendCmd("sub slice all", [this](int, const QString&) {
      sendCmd("sub pan all", [this](int, const QString&) {
      sendCmd("sub tx all", [this](int, const QString&) {
        sendCmd("sub atu all", [this](int, const QString&) {
        sendCmd("sub amplifier all", [this](int, const QString&) {
          sendCmd("sub meter all", [this](int, const QString&) {
            sendCmd("sub audio all", [this](int, const QString&) {
            sendCmd("sub gps all", [this](int, const QString&) {
            sendCmd("sub apd all", [this](int, const QString&) {
            sendCmd("sub client all", [this](int, const QString&) {
            sendCmd("sub xvtr all", [this](int, const QString&) {
            // Memory status arrives via normal status handler — no subscription needed.
            // "sub memory all" returns 500000A3 (invalid subscription object).
        // Request available mic inputs (comma-separated response: "MIC,BAL,LINE,ACC")
        sendCmd("mic list", [this](int code, const QString& body) {
            if (code == 0) {
                QStringList inputs = body.trimmed().split(',', Qt::SkipEmptyParts);
                m_transmitModel.setMicInputList(inputs);
                qCDebug(lcProtocol) << "RadioModel: mic inputs:" << inputs;
            }
        });

        // Always (re)start on connect — re-binds socket and re-registers
        // UDP port with the radio. start() calls stop() internally if needed. (#561)
        bool streamOk = false;
        if (m_wanConn) {
            QMetaObject::invokeMethod(m_panStream, [this, &streamOk]() {
                streamOk = m_panStream->startWan(QHostAddress(m_wanPublicIp), m_wanUdpPort);
            }, Qt::BlockingQueuedConnection);
        } else {
            QMetaObject::invokeMethod(m_panStream, [this, &streamOk]() {
                streamOk = m_panStream->start(m_connection);
            }, Qt::BlockingQueuedConnection);
        }

        if (!streamOk) {
            qCWarning(lcProtocol) << "RadioModel: UDP stream setup failed — disconnecting gracefully (#894)";
            emit connectionError(tr("UDP stream setup failed. If connecting over VPN, "
                                    "ensure UDP traffic from the radio is routable."));
            QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
            return;
        }

        // Schedule a UDP stream health check: if no VITA-49 data arrives
        // within 10 seconds (e.g. VPN blocks UDP), warn the user. (#894)
        QTimer::singleShot(10000, this, [this]() {
            if (!isConnected()) return;
            if (!m_wanConn && !m_panStream->hasReceivedPackets()) {
                qCWarning(lcProtocol) << "RadioModel: no VITA-49 UDP data received after 10s"
                                      << "target=" << targetRadioIp()
                                      << "sourceMode=" << selectedSourceMode()
                                      << "sourcePath=" << selectedSourcePath()
                                      << "localTcp=" << localTcpEndpoint()
                                      << "localUdp=" << localUdpEndpoint()
                                      << "udpSeen=" << firstUdpPacketSeen();
                emit connectionError(
                    tr("No spectrum data received from %1. Source mode: %2. TCP: %3. UDP: %4. "
                       "UDP traffic from the radio may be blocked, or the wrong source path may be selected.")
                        .arg(targetRadioIp(), selectedSourceMode(),
                             localTcpEndpoint(), localUdpEndpoint()));
            }
        });

        // On WAN: use "client udp_register" via UDP (not TCP "client udpport").
        // The radio only accepts udp_register on WAN connections.
        if (m_wanConn) {
            QMetaObject::invokeMethod(m_panStream, [this]() {
                m_panStream->startWanUdpRegister(clientHandle());
            });
            qCDebug(lcProtocol) << "RadioModel: WAN — started UDP registration via udp_register";
        }

        const quint16 udpPort = m_panStream->localPort();
        qCInfo(lcProtocol).noquote()
            << "RadioModel: client udpport requested"
            << QStringLiteral("port=%1").arg(udpPort);
        sendCmd(
            QString("client udpport %1").arg(udpPort),
            [this, udpPort](int code2, const QString& body) {
                if (code2 == 0) {
                    qCInfo(lcProtocol).noquote()
                        << "RadioModel: client udpport registered"
                        << QStringLiteral("port=%1").arg(udpPort);
                } else if (shouldRetryLanUdpPortRegistration(m_wanConn != nullptr, code2, body)) {
                    bool rebound = false;
                    QMetaObject::invokeMethod(m_panStream, [this, &rebound]() {
                        rebound = m_panStream->rebindToEphemeralPort(m_connection);
                    }, Qt::BlockingQueuedConnection);

                    if (!rebound) {
                        qCWarning(lcProtocol) << "RadioModel: UDP port" << udpPort
                                              << "is already registered and AetherSDR could not rebind";
                        emit connectionError(tr("UDP port %1 is already in use by another Flex client, "
                                                "and AetherSDR could not switch to a free UDP port.")
                                                 .arg(udpPort));
                        QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
                        return;
                    }

                    const quint16 retryUdpPort = m_panStream->localPort();
                    qCWarning(lcProtocol).noquote()
                        << "RadioModel: client udpport collision"
                        << QStringLiteral("port=%1").arg(udpPort)
                        << QStringLiteral("code=%1").arg(hexCode(code2))
                        << QStringLiteral("retry_port=%1").arg(retryUdpPort);
                    qCInfo(lcProtocol).noquote()
                        << "RadioModel: client udpport requested"
                        << QStringLiteral("port=%1").arg(retryUdpPort);
                    sendCmd(
                        QString("client udpport %1").arg(retryUdpPort),
                        [this, retryUdpPort](int retryCode, const QString& retryBody) {
                            if (retryCode == 0) {
                                qCInfo(lcProtocol).noquote()
                                    << "RadioModel: client udpport retry registered"
                                    << QStringLiteral("port=%1").arg(retryUdpPort);
                            } else {
                                qCWarning(lcProtocol).noquote()
                                    << "RadioModel: client udpport retry failed"
                                    << QStringLiteral("code=%1").arg(hexCode(retryCode))
                                    << QStringLiteral("body=%1").arg(retryBody);
                                emit connectionError(tr("UDP port registration failed after switching to port %1: %2")
                                                         .arg(retryUdpPort)
                                                         .arg(retryBody));
                                QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
                            }
                        });
                } else {
                    qCDebug(lcProtocol) << "RadioModel: client udpport returned error" << Qt::hex << code2
                             << "(expected on WAN — using udp_register instead)";
                }

                // Query radio info (region, callsign, options, etc.)
                // Response is comma-separated key=value pairs.
                sendCmd("info",
                    [this](int code, const QString& body) {
                        if (code != 0) return;
                        for (const QString& kv : body.split(',')) {
                            const int eq = kv.indexOf('=');
                            if (eq < 0) continue;
                            const QString key = kv.left(eq).trimmed();
                            const QString val = kv.mid(eq + 1).trimmed()
                                .remove('\\').remove('"');
                            if (key == "callsign")        m_callsign = val;
                            else if (key == "name")        m_nickname = val;
                            else if (key == "region")      m_region = val;
                            else if (key == "options")     m_radioOptions = val;
                            else if (key == "model") {
                                m_model = val;
                                m_maxSlices = maxSlicesForModel(m_model);
                            }
                            else if (key == "chassis_serial") m_chassisSerial = val;
                            else if (key == "software_ver")   m_version = val;
                            else if (key == "ip")             m_ip = val;
                            else if (key == "netmask")        m_netmask = val;
                            else if (key == "gateway")        m_gateway = val;
                            else if (key == "mac")            m_mac = val;
                        }
                        qCDebug(lcProtocol) << "RadioModel: info — callsign:" << m_callsign
                                 << "region:" << m_region << "options:" << m_radioOptions;
                        emit infoChanged();
                        if (reloadAntennaAliases())
                            emit antennaAliasesChanged();
                    });

                sendCmd("slice list",
                    [this](int code3, const QString& body) {
                        if (code3 != 0) {
                            qCWarning(lcProtocol) << "RadioModel: slice list failed, code" << Qt::hex << code3;
                            return;
                        }
                        const QStringList ids = body.trimmed().split(' ', Qt::SkipEmptyParts);
                        qCDebug(lcProtocol) << "RadioModel: slice list ->" << (ids.isEmpty() ? "(empty)" : body);

                        if (ids.isEmpty()) {
                            // Radio has no slices at all — create one
                            qCDebug(lcProtocol) << "RadioModel: no slices on radio, creating default";
                            auto& settings = AppSettings::instance();
                            double lastFreq = settings.value("LastFrequency", "0").toDouble();
                            QString lastMode = settings.value("LastMode", "").toString();
                            if (lastFreq > 0.0) {
                                createDefaultSlice(
                                    QString::number(lastFreq, 'f', 6),
                                    lastMode.isEmpty() ? "USB" : lastMode);
                            } else {
                                createDefaultSlice();
                            }
                        } else if (m_slices.isEmpty()) {
                            // Radio has slices but we haven't matched any to our
                            // client_handle yet (status messages still in flight).
                            // Defer the decision to give status messages time to
                            // arrive and populate m_slices via handleSliceStatus.
                            qCDebug(lcProtocol) << "RadioModel: radio has" << ids.size()
                                     << "slice(s) but none matched yet — deferring 500ms";
                            QTimer::singleShot(500, this, [this]() {
                                if (m_slices.isEmpty() && isConnected()) {
                                    qCDebug(lcProtocol) << "RadioModel: deferred check — still no owned slices, creating default";
                                    auto& settings = AppSettings::instance();
                                    double lastFreq = settings.value("LastFrequency", "0").toDouble();
                                    QString lastMode = settings.value("LastMode", "").toString();
                                    if (lastFreq > 0.0) {
                                        createDefaultSlice(
                                            QString::number(lastFreq, 'f', 6),
                                            lastMode.isEmpty() ? "USB" : lastMode);
                                    } else {
                                        createDefaultSlice();
                                    }
                                } else if (!m_slices.isEmpty()) {
                                    qCDebug(lcProtocol) << "RadioModel: deferred check — adopted"
                                             << m_slices.size() << "existing slice(s)";
                                }
                            });
                        } else {
                            qCDebug(lcProtocol) << "RadioModel: SmartConnect — using our pan"
                                     << m_activePanId << "and" << m_slices.size() << "slice(s)";
                        }

                        for (auto* s : m_slices) {
                            for (const QString& cmd : s->drainPendingCommands())
                                sendCmd(cmd);
                        }

                        // Create remote_audio_rx if PC Audio is enabled. Defer briefly so
                        // SmartConnect/restored stream status can be adopted before
                        // we ask the radio for another stream. (#1014, #1051, #1137, #2037)
                        scheduleRxAudioStreamEnsure(QStringLiteral("connect"));

                        // Do not claim a dax_tx stream at GUI attach time. SmartSDR DAX
                        // owns that stream on Windows; AetherSDR creates one lazily only
                        // when its own bridge/TCI path needs to feed DAX TX audio.

                        // Request remote audio TX stream (voice mode, VOX monitoring)
                        // This stream carries mic audio to the radio for voice TX and
                        // VOX detection. met_in_rx=1 tells the radio to monitor it during RX.
                        // Create netcw stream for low-latency CW keying via UDP
                        sendCmd("stream create netcw",
                            [this](int code, const QString& body) {
                                if (code == 0) {
                                    m_netCwStreamId = body.trimmed().toUInt(nullptr, 16);
                                    m_netCwIndex = 1;
                                    m_netCwClock.invalidate();
                                    m_netCwLastSendMs = -1;
                                    qCDebug(lcProtocol) << "RadioModel: netcw stream created, id:"
                                             << Qt::hex << m_netCwStreamId;
                                } else {
                                    qCDebug(lcProtocol) << "RadioModel: netcw stream not supported, code"
                                             << Qt::hex << code << "— using cw key immediate fallback";
                                }
                            });

                        // Radio always forces Opus for remote_audio_tx regardless of
                        // what we request (confirmed by protocol testing, v1.4.0.0).
                        sendCmd(
                            "stream create type=remote_audio_tx compression=opus",
                            [this](int code, const QString& body) {
                                if (code == 0) {
                                    quint32 id = body.trimmed().toUInt(nullptr, 16);
                                    qCDebug(lcProtocol) << "RadioModel: remote_audio_tx stream created, id:"
                                             << Qt::hex << id;
                                    sendCmd("transmit set met_in_rx=1");
                                    emit remoteTxStreamReady(id);
                                } else {
                                    qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_tx failed, code"
                                               << Qt::hex << code << "body:" << body;
                                }
                            });
                    });
            });
            // Request global profile list
            sendCmd("profile global info");
            sendCmd("sub tnf all");
            sendCmd("sub memories all");
            // Additional subscriptions (matches SmartSDR connection sequence)
            sendCmd("sub cwx all");
            sendCmd("sub dax all");
            sendCmd("sub daxiq all");
            sendCmd("sub radio all");
            sendCmd("sub codec all");
            sendCmd("sub dvk all");
            sendCmd("sub navtex all");
            sendCmd("sub usb_cable all");
            sendCmd("sub spot all");
            sendCmd("sub license all");
            }); // sub xvtr all
            }); // sub client all
            }); // sub apd all
            }); // sub gps all
            }); // sub audio all
          }); // sub meter all
        }); // sub amplifier all
        }); // sub atu all
      }); // sub tx all
      }); // sub pan all
    }); // sub slice all
    }); // client gui
}

int RadioModel::bandIdForFrequency(double freqMhz) const
{
    // Standard amateur HF band ranges → band names matching radio's band_name field
    struct BandRange { double lo; double hi; const char* name; };
    static constexpr BandRange bands[] = {
        {1.8,    2.0,    "160"},
        {3.5,    4.0,    "80"},
        {5.0,    5.5,    "60"},
        {7.0,    7.3,    "40"},
        {10.1,   10.15,  "30"},
        {14.0,   14.35,  "20"},
        {18.068, 18.168, "17"},
        {21.0,   21.45,  "15"},
        {24.89,  24.99,  "12"},
        {28.0,   29.7,   "10"},
        {50.0,   54.0,   "6"},
        {144.0,  148.0,  "2m"},
    };

    for (const auto& b : bands) {
        if (freqMhz >= b.lo && freqMhz <= b.hi) {
            // Find the band ID with this name in m_txBandSettings
            for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it) {
                if (it->bandName == b.name)
                    return it->bandId;
            }
        }
    }
    // Out-of-band or GEN — check for GEN band
    for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it) {
        if (it->bandName == "GEN")
            return it->bandId;
    }
    return -1;
}

void RadioModel::applyTuneInhibit()
{
    auto& s = AppSettings::instance();
    double txFreq = 0.0;
    for (auto* sl : m_slices) {
        if (sl->isTxSlice()) { txFreq = sl->frequency(); break; }
    }
    int bandId = bandIdForFrequency(txFreq);
    if (bandId < 0) return;
    auto it = m_txBandSettings.find(bandId);
    if (it == m_txBandSettings.end()) return;

    QStringList inhibited;
    if (s.value("TuneInhibitAccTx", "False").toString() == "True" && it->accTx) {
        sendCmd(QString("interlock bandset %1 acc_tx_enabled=0").arg(bandId));
        inhibited << "ACC TX";
    }
    if (s.value("TuneInhibitTx1", "False").toString() == "True" && it->tx1) {
        sendCmd(QString("interlock bandset %1 tx1_enabled=0").arg(bandId));
        inhibited << "TX1";
    }
    if (s.value("TuneInhibitTx2", "False").toString() == "True" && it->tx2) {
        sendCmd(QString("interlock bandset %1 tx2_enabled=0").arg(bandId));
        inhibited << "TX2";
    }
    if (s.value("TuneInhibitTx3", "False").toString() == "True" && it->tx3) {
        sendCmd(QString("interlock bandset %1 tx3_enabled=0").arg(bandId));
        inhibited << "TX3";
    }
    if (!inhibited.isEmpty()) {
        m_tuneInhibitBandId = bandId;
        m_tuneInhibitActive = true;
        qDebug() << "Tune PA inhibit: disabled" << inhibited.join(", ")
                 << "on band" << bandId << "before tune";
    }
}

void RadioModel::restoreTuneInhibit()
{
    auto& s = AppSettings::instance();
    int id = m_tuneInhibitBandId;
    QStringList restored;
    if (s.value("TuneInhibitAccTx", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 acc_tx_enabled=1").arg(id));
        restored << "ACC TX";
    }
    if (s.value("TuneInhibitTx1", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx1_enabled=1").arg(id));
        restored << "TX1";
    }
    if (s.value("TuneInhibitTx2", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx2_enabled=1").arg(id));
        restored << "TX2";
    }
    if (s.value("TuneInhibitTx3", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx3_enabled=1").arg(id));
        restored << "TX3";
    }
    qDebug() << "Tune PA inhibit: restored" << restored.join(", ") << "on band" << id;
    m_tuneInhibitActive = false;
    m_tuneInhibitBandId = -1;
}

void RadioModel::onDisconnected()
{
    qCDebug(lcProtocol) << "RadioModel: disconnected";

    // Release sleep inhibition on disconnect (#1420)
    m_sleepInhibitor.release();

    // Safety: restore TX outputs if we were inhibiting during tune
    if (m_tuneInhibitActive && m_tuneInhibitBandId >= 0)
        restoreTuneInhibit();

    m_txRequested = false;
    m_cwKeyActive = false;
    m_cwxActive = false;
    m_lastInterlockSource.clear();
    m_lastInterlockNotificationKey.clear();
    m_lastInterlockNotificationMs = 0;
    m_interlockNotificationArmedUntilMs = 0;
    m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
    m_interlockNotificationSource = TransmitModel::PttSource::Mox;
    m_digitalVoiceTxSliceId = -1;
    if (m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }
    m_radioTransmitting = false;
    emit radioTransmittingChanged(false);
    m_transmitModel.setTransmitting(false);
    m_transmitModel.resetState();
    m_meterModel.clear();
    m_daxStreamDebug.clear();
    m_daxTxStreamId = 0;
    m_daxTxActive = false;
    m_daxTxClientHandle = 0;
    m_daxTxCreatePending = false;
    m_deadDaxRxSeen.clear();
    m_externalDaxTxSeen.clear();
    m_externalDaxRxSeen.clear();

    // Reset radio-model-specific state — different radios have different
    // capabilities (APD, max power, pan count, TGXL, amplifier, XVTR, etc.)
    // Must re-derive everything from the new radio's status on next connect. (#359)
    m_tunerModel.setHandle({});       // clear TGXL presence
    m_xvtrList.clear();
    m_hasAmplifier = false;
    m_ampHandle.clear();
    m_ampOperate = false;
    m_fullDuplex = false;
    m_maxSlices = 4;
    m_model.clear();
    m_version.clear();
    m_chassisSerial.clear();
    m_callsign.clear();
    m_region.clear();
    m_rxAudio = {};
    m_netCwStreamId = 0;
    m_netCwIndex = 1;
    m_netCwClock.invalidate();
    m_netCwLastSendMs = -1;
    m_lineoutGain = 50;
    m_headphoneGain = 50;

    stopNetworkMonitor();
    // stop() must run on the network thread (socket lives there). (#561)
    QMetaObject::invokeMethod(m_panStream, &PanadapterStream::stop,
                              Qt::BlockingQueuedConnection);
    m_panStream->clearRegisteredStreams();
    // Clean up panadapter models
    qDeleteAll(m_panadapters);
    m_panadapters.clear();
    m_pendingPanStatuses.clear();
    m_activePanId.clear();
    m_ownedSliceIds.clear();
    m_tnfModel.clear();
    if (!m_memories.isEmpty()) {
        m_memories.clear();
        emit memoriesCleared();
    }
    m_clientStations.clear();
    m_clientInfoMap.clear();
    m_announcedClientConnections.clear();
    m_startupClientConnections.clear();
    m_clientConnectionNoticeTimer.invalidate();
    emit otherClientsChanged(0, {});
    emit connectionStateChanged(false);
    m_forcedDisconnectInProgress = false;

    if (m_wanConn) {
        qCDebug(lcProtocol) << "RadioModel: WAN disconnected";
        m_wanConn->disconnect(this);
        m_wanConn = nullptr;
    } else if (!m_intentionalDisconnect && !m_lastInfo.address.isNull()) {
        qCDebug(lcProtocol) << "RadioModel: unexpected disconnect — reconnecting in 3s";
        m_reconnectTimer.start();
    }
}

void RadioModel::onConnectionError(const QString& msg)
{
    qCWarning(lcProtocol) << "RadioModel: connection error:" << msg;
    emit connectionError(msg);
    // A refused connect may never emit disconnected, but the radio can recover
    // after expiring a stale session. Keep retrying the same discovered radio.
    if (!m_wanConn && !m_intentionalDisconnect && !m_lastInfo.address.isNull()
            && !m_reconnectTimer.isActive()) {
        qCDebug(lcProtocol) << "RadioModel: connection error — reconnecting in 5s";
        m_reconnectTimer.start();
    }
}

void RadioModel::onVersionReceived(const QString& v)
{
    // The V line from the radio is the protocol version (e.g. "1.4.0.0").
    // We prefer the software version from discovery (e.g. "4.1.5").
    // Only use protocol version as fallback if discovery didn't provide one.
    if (m_version.isEmpty())
        m_version = v;
    m_protocolVersion = v;
    emit infoChanged();
}

// ─── Network quality monitor ─────────────────────────────────────────────────

void RadioModel::startNetworkMonitor()
{
    m_pingTimer.stop();
    m_pingTimer.disconnect();
    m_netState = NetState::Excellent;
    m_networkQualityScore = 100.0;
    resetNetworkHealthSamples();
    m_lastPingRtt = 0;
    m_maxPingRtt = 0;
    m_pingMissCount = 0;
    m_pingDisconnectTriggered = false;

    // RTT is read from kernel TCP_INFO (smoothed RTT from TCP ACK timing),
    // completely independent of Qt event loop buffering. Falls back to
    // QElapsedTimer stopwatch if the platform kernel call is unavailable.
    if (m_networkPingConnection) {
        disconnect(m_networkPingConnection);
        m_networkPingConnection = {};
    }
    m_networkPingConnection = connect(m_connection, &RadioConnection::pingRttMeasured, this, [this](int ms) {
        m_pingMissCount = 0;
        m_lastPingRtt = ms;
        evaluateNetworkQuality();
        emit pingReceived();
    });

    connect(&m_pingTimer, &QTimer::timeout, this, [this]() {
        if (!isConnected()) {
            stopNetworkMonitor();
            return;
        }
        ++m_pingMissCount;
        if (m_pingMissCount >= PING_MISS_DISCONNECT) {
            if (m_pingDisconnectTriggered)
                return;

            m_pingDisconnectTriggered = true;
            m_pingTimer.stop();
            qDebug() << "RadioModel:" << PING_MISS_DISCONNECT
                     << "consecutive pings unanswered — forcing disconnect";
            forceDisconnect();
            return;
        }
        sendCmd("ping");  // RTT measured by RadioConnection::pingRttMeasured
    });
    m_pingTimer.start(1000);
}

void RadioModel::stopNetworkMonitor()
{
    m_pingTimer.stop();
    m_pingTimer.disconnect();
    if (m_networkPingConnection) {
        disconnect(m_networkPingConnection);
        m_networkPingConnection = {};
    }
    m_netState = NetState::Off;
}

void RadioModel::evaluateNetworkQuality()
{
    const int currentErrors = m_panStream->packetErrorCount();
    const int currentPackets = m_panStream->packetTotalCount();
    recordNetworkHealthSample(currentErrors, currentPackets);
    const int ping = m_lastPingRtt;

    const double targetScore = networkQualityTargetScore(ping);
    const double alpha = targetScore < m_networkQualityScore
                             ? (targetScore <= 45.0 ? 0.45 : 0.30)
                             : 0.12;
    m_networkQualityScore += (targetScore - m_networkQualityScore) * alpha;
    m_netState = networkStateForScore(m_networkQualityScore, m_netState);
    if (ping > m_maxPingRtt) m_maxPingRtt = ping;

    static const char* names[] = {"Off", "Excellent", "Very Good", "Good", "Fair", "Poor"};
    emit networkQualityChanged(names[static_cast<int>(m_netState)], ping);
}

void RadioModel::resetNetworkHealthSamples()
{
    m_lastErrorCount = m_panStream ? m_panStream->packetErrorCount() : 0;
    m_lastPacketCount = m_panStream ? m_panStream->packetTotalCount() : 0;
    for (int i = 0; i < NETWORK_LOSS_WINDOW_SAMPLES; ++i) {
        m_lossSamplePackets[i] = 0;
        m_lossSampleErrors[i] = 0;
    }
    m_lossSampleCursor = 0;
    m_lossSampleCount = 0;
    m_packetLossWindowPackets = 0;
    m_packetLossWindowErrors = 0;
}

void RadioModel::recordNetworkHealthSample(int currentErrors, int currentPackets)
{
    const int deltaErrors = std::max(0, currentErrors - m_lastErrorCount);
    const int deltaPackets = std::max(0, currentPackets - m_lastPacketCount);
    m_lastErrorCount = currentErrors;
    m_lastPacketCount = currentPackets;

    if (m_lossSampleCount < NETWORK_LOSS_WINDOW_SAMPLES) {
        ++m_lossSampleCount;
    } else {
        m_packetLossWindowPackets -= m_lossSamplePackets[m_lossSampleCursor];
        m_packetLossWindowErrors -= m_lossSampleErrors[m_lossSampleCursor];
    }

    m_lossSamplePackets[m_lossSampleCursor] = deltaPackets;
    m_lossSampleErrors[m_lossSampleCursor] = deltaErrors;
    m_packetLossWindowPackets += deltaPackets;
    m_packetLossWindowErrors += deltaErrors;
    m_lossSampleCursor = (m_lossSampleCursor + 1) % NETWORK_LOSS_WINDOW_SAMPLES;
}

double RadioModel::networkQualityTargetScore(int pingMs) const
{
    const bool remote = usesRemoteNetworkThresholds();
    const int fairPingMs = remote ? REMOTE_PING_FAIR_MS : LAN_PING_FAIR_MS;
    const int poorPingMs = remote ? REMOTE_PING_POOR_MS : LAN_PING_POOR_MS;
    const int goodJitterMs = remote ? 45 : 20;
    const int fairJitterMs = remote ? 90 : 45;
    const int poorJitterMs = remote ? 150 : 90;

    double score = 100.0;
    if (pingMs >= poorPingMs) {
        score = std::min(score, 45.0);
    } else if (pingMs >= fairPingMs) {
        score = std::min(score, 70.0);
    } else if (pingMs >= fairPingMs * 2 / 3) {
        score = std::min(score, 84.0);
    }

    if (m_packetLossWindowPackets >= NETWORK_MIN_LOSS_WINDOW_PACKETS) {
        const double lossPct = packetLossPercent();
        if (lossPct >= 3.0) {
            score = std::min(score, 35.0);
        } else if (lossPct >= 1.0) {
            score = std::min(score, 52.0);
        } else if (lossPct >= 0.35) {
            score = std::min(score, 70.0);
        } else if (lossPct >= 0.05) {
            score = std::min(score, 84.0);
        }
    }

    const int jitterMs = audioPacketJitterMs();
    if (jitterMs >= poorJitterMs) {
        score = std::min(score, 42.0);
    } else if (jitterMs >= fairJitterMs) {
        score = std::min(score, 58.0);
    } else if (jitterMs >= goodJitterMs) {
        score = std::min(score, 74.0);
    }

    return score;
}

RadioModel::NetState RadioModel::networkStateForScore(double score, NetState currentState) const
{
    switch (currentState) {
    case NetState::Excellent:
        return score < 89.0 ? NetState::VeryGood : NetState::Excellent;
    case NetState::VeryGood:
        if (score >= 94.0)
            return NetState::Excellent;
        if (score < 76.0)
            return NetState::Good;
        return NetState::VeryGood;
    case NetState::Good:
        if (score >= 83.0)
            return NetState::VeryGood;
        if (score < 60.0)
            return NetState::Fair;
        return NetState::Good;
    case NetState::Fair:
        if (score >= 68.0)
            return NetState::Good;
        if (score < 40.0)
            return NetState::Poor;
        return NetState::Fair;
    case NetState::Poor:
        return score >= 50.0 ? NetState::Fair : NetState::Poor;
    case NetState::Off:
        break;
    }

    if (score >= 92.0)
        return NetState::Excellent;
    if (score >= 80.0)
        return NetState::VeryGood;
    if (score >= 65.0)
        return NetState::Good;
    if (score >= 45.0)
        return NetState::Fair;
    return NetState::Poor;
}

bool RadioModel::usesRemoteNetworkThresholds() const
{
    return m_wanConn != nullptr || m_lastInfo.isRouted;
}

QString RadioModel::networkQuality() const
{
    static const char* names[] = {"Off", "Excellent", "Very Good", "Good", "Fair", "Poor"};
    return names[static_cast<int>(m_netState)];
}

double RadioModel::packetLossPercent() const
{
    if (m_packetLossWindowPackets <= 0)
        return 0.0;
    return (m_packetLossWindowErrors * 100.0) / m_packetLossWindowPackets;
}

int RadioModel::audioPacketGapMs() const
{
    return m_panStream ? m_panStream->audioPacketGapMs() : 0;
}

int RadioModel::audioPacketGapMaxMs() const
{
    return m_panStream ? m_panStream->audioPacketGapMaxMs() : 0;
}

int RadioModel::audioPacketJitterMs() const
{
    return m_panStream ? m_panStream->audioPacketJitterMs() : 0;
}

int RadioModel::packetDropCount() const
{
    return m_panStream->packetErrorCount();
}

int RadioModel::packetTotalCount() const
{
    return m_panStream->packetTotalCount();
}

qint64 RadioModel::rxBytes() const
{
    return m_panStream->totalRxBytes();
}

qint64 RadioModel::txBytes() const
{
    return m_panStream->totalTxBytes();
}

QString RadioModel::targetRadioIp() const
{
    return m_lastInfo.address.toString();
}

QString RadioModel::selectedSourceMode() const
{
    return m_lastInfo.bindSettings.modeString();
}

QString RadioModel::selectedSourcePath() const
{
    if (m_lastInfo.bindSettings.mode == RadioBindMode::Explicit)
        return m_lastInfo.bindSettings.selectionLabel();

    QHostAddress resolved = m_lastInfo.sessionBindAddress;
    if (resolved.isNull())
        resolved = m_connection->localAddress();
    if (!resolved.isNull() && resolved.protocol() == QAbstractSocket::IPv4Protocol)
        return QStringLiteral("Auto (%1)").arg(resolved.toString());
    return QStringLiteral("Auto");
}

QString RadioModel::localTcpEndpoint() const
{
    if (m_wanConn)
        return QStringLiteral("SmartLink/WAN");

    const QHostAddress localAddr = m_connection->localAddress();
    const quint16 localPort = m_connection->localTcpPort();
    if (localAddr.isNull() || localPort == 0)
        return QStringLiteral("Not connected");
    return QStringLiteral("%1:%2").arg(localAddr.toString()).arg(localPort);
}

QString RadioModel::localUdpEndpoint() const
{
    const QHostAddress localAddr = m_panStream->localAddress();
    const quint16 localPort = m_panStream->localPort();
    if (localAddr.isNull() || localPort == 0)
        return QStringLiteral("Not bound");
    return QStringLiteral("%1:%2").arg(localAddr.toString()).arg(localPort);
}

bool RadioModel::firstUdpPacketSeen() const
{
    return m_panStream->hasReceivedPackets();
}

PanadapterStream::CategoryStats RadioModel::categoryStats(PanadapterStream::StreamCategory cat) const
{
    return m_panStream->categoryStats(cat);
}

void RadioModel::handleMemoryStatus(int index, const QMap<QString, QString>& kvs)
{
    // Check for removal — radio sends either "in_use=0" or "removed" (no value)
    if (kvs.value("in_use") == "0" || kvs.contains("removed")) {
        m_memories.remove(index);
        emit memoryRemoved(index);
        return;
    }

    auto& m = m_memories[index];
    m.index = index;

    for (auto it = kvs.begin(); it != kvs.end(); ++it) {
        const QString& k = it.key();
        const QString& v = it.value();
        if      (k == "group")           m.group = QString(v).replace('\x7f', ' ');
        else if (k == "owner")           m.owner = QString(v).replace('\x7f', ' ');
        else if (k == "freq")            m.freq = v.toDouble();
        else if (k == "name")            m.name = QString(v).replace('\x7f', ' ');
        else if (k == "mode")            m.mode = v;
        else if (k == "step")            m.step = v.toInt();
        else if (k == "repeater")        m.offsetDir = v;
        else if (k == "repeater_offset") m.repeaterOffset = v.toDouble();
        else if (k == "tone_mode")       m.toneMode = v;
        else if (k == "tone_value")      m.toneValue = v.toDouble();
        else if (k == "squelch")         m.squelch = (v == "1");
        else if (k == "squelch_level")   m.squelchLevel = v.toInt();
        else if (k == "rx_filter_low")   m.rxFilterLow = v.toInt();
        else if (k == "rx_filter_high")  m.rxFilterHigh = v.toInt();
        else if (k == "rtty_mark")       m.rttyMark = v.toInt();
        else if (k == "rtty_shift")      m.rttyShift = v.toInt();
        else if (k == "digl_offset")     m.diglOffset = v.toInt();
        else if (k == "digu_offset")     m.diguOffset = v.toInt();
    }

    emit memoryChanged(index);
}

// ─── Raw message handler (for meter status with '#' separators) ──────────────

void RadioModel::onMessageReceived(const ParsedMessage& msg)
{
    // Meter status uses '#' as KV separator (not spaces), so the normal
    // parseKVs() in CommandParser doesn't handle it.  We intercept the raw
    // status line here and parse it ourselves.
    if (msg.type != MessageType::Status) return;

    // Raw line: "S<handle>|meter 7.src=SLC#7.num=0#7.nam=LEVEL#..."
    const QString& raw = msg.raw;
    const int pipe = raw.indexOf('|');
    if (pipe < 0) return;
    const QString body = raw.mid(pipe + 1);
    // Profile status: "profile tx list=Default^..." or "profile mic list=..."
    // Profile names contain spaces, so parseKVs() (which splits on spaces) breaks
    // the list value.  Handle raw here, same pattern as meter status.
    if (body.startsWith("profile tx ")) {
        handleProfileStatusRaw("tx", body.mid(11));  // skip "profile tx "
        return;
    }
    if (body.startsWith("profile mic ")) {
        handleProfileStatusRaw("mic", body.mid(12));  // skip "profile mic "
        return;
    }
    if (body.startsWith("profile global ")) {
        handleProfileStatusRaw("global", body.mid(15));  // skip "profile global "
        return;
    }

    // GPS status: "gps lat=...#lon=...#grid=...#tracked=...#visible=...#status=..."
    if (body.startsWith("gps ")) {
        handleGpsStatus(body.mid(4));  // skip "gps "
        return;
    }

    if (!body.startsWith("meter ")) return;

    handleMeterStatus(body.mid(6));  // skip "meter "
}

// ─── Status dispatch ──────────────────────────────────────────────────────────
//
// Object strings look like:
//   "radio"           → global radio properties
//   "slice 0"         → slice receiver
//   "panadapter 0"    → panadapter (spectrum)
//   "meter 1"         → meter reading (handled by onMessageReceived)
//   "removed=True"    → object was removed

void RadioModel::sendCommand(const QString& cmd)
{
    qCDebug(lcProtocol) << "RadioModel::sendCommand:" << cmd
             << "connected:" << isConnected() << "wan:" << (m_wanConn != nullptr);
    this->sendCmd(cmd);
}

void RadioModel::sendCmdPublic(const QString& cmd, ResponseCallback cb)
{
    sendCmd(cmd, cb);
}

void RadioModel::requestLocalPtt()
{
    // "enforce_local_ptt" returns 0x50001000; the settable key matches the
    // status key the radio broadcasts: local_ptt.  Firmware v1.4.0.0 quirk.
    sendCmd("client set local_ptt=1", [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "requestLocalPtt: radio returned error"
                                  << Qt::hex << code << body;
            return;
        }
        // Optimistic update: mark our own entry as having PTT in case the radio
        // doesn't echo a local_ptt=1 status message back to us.
        quint32 ours = clientHandle();
        if (m_clientInfoMap.contains(ours)) {
            m_clientInfoMap[ours].localPtt = true;
            emitOtherClientsChanged();
        }
    });
}

void RadioModel::setAmpOperate(bool on)
{
    if (m_ampHandle.isEmpty()) return;
    // FlexLib API: "amplifier set <handle> operate=0|1"
    sendCmd(QString("amplifier set %1 operate=%2").arg(m_ampHandle).arg(on ? 1 : 0));
}

void RadioModel::createRxAudioStream()
{
    if (m_rxAudio.streamId != 0) {
        logRemoteAudioRxSummary(QStringLiteral("create skipped: stream already known"));
        return;
    }
    if (m_rxAudio.createPending) {
        logRemoteAudioRxSummary(QStringLiteral("create skipped: request already pending"));
        return;
    }

    m_rxAudio.createPending = true;
    m_rxAudio.removeRequested = false;
    logRemoteAudioRxSummary(QStringLiteral("create requested"));
    sendCmd(QString("stream create type=remote_audio_rx compression=%1").arg(audioCompressionParam()),
        [this](int code, const QString& body) {
            m_rxAudio.createPending = false;
            if (code == 0) {
                const quint32 streamId = RadioStatusOwnership::parseCreateResponseStreamId(body);
                if (streamId == 0) {
                    qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_rx returned unparseable body:"
                                          << body;
                    logRemoteAudioRxSummary(QStringLiteral("create response unparseable"));
                    return;
                }

                if (m_rxAudio.streamId != 0 && m_rxAudio.streamId != streamId) {
                    const quint32 oldStreamId = m_rxAudio.streamId;
                    qCDebug(lcProtocol) << "RadioModel: replacing restored remote_audio_rx"
                                        << RadioStatusOwnership::hexId(oldStreamId)
                                        << "with create response"
                                        << RadioStatusOwnership::hexId(streamId);
                    sendCmd(QString("stream remove %1").arg(RadioStatusOwnership::hexId(oldStreamId)));
                }

                m_rxAudio.streamId = streamId;
                m_rxAudio.clientHandle = clientHandle();
                m_rxAudio.compression = audioCompressionParam();
                qCDebug(lcProtocol) << "RadioModel: remote_audio_rx stream created, id:"
                                    << RadioStatusOwnership::hexId(streamId);
                logRemoteAudioRxSummary(QStringLiteral("create response adopted"));
                if (m_rxAudio.removeRequested)
                    removeRxAudioStream();
            } else {
                qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_rx failed, code"
                           << Qt::hex << code << "body:" << body;
                logRemoteAudioRxSummary(QStringLiteral("create failed"));
            }
        });
}

void RadioModel::removeRxAudioStream()
{
    if (m_rxAudio.streamId == 0) {
        if (m_rxAudio.createPending) {
            m_rxAudio.removeRequested = true;
            logRemoteAudioRxSummary(QStringLiteral("remove deferred: create pending"));
        } else {
            logRemoteAudioRxSummary(QStringLiteral("remove skipped: no known stream"));
        }
        return;
    }

    const quint32 streamId = m_rxAudio.streamId;
    sendCmd(QString("stream remove %1").arg(RadioStatusOwnership::hexId(streamId)));
    qCDebug(lcProtocol) << "RadioModel: removed remote_audio_rx stream"
                        << RadioStatusOwnership::hexId(streamId);
    m_rxAudio.streamId = 0;
    m_rxAudio.clientHandle = 0;
    m_rxAudio.statusSeen = false;
    m_rxAudio.removeRequested = false;
    m_rxAudio.compression.clear();
    logRemoteAudioRxSummary(QStringLiteral("remove requested"));
}

void RadioModel::scheduleRxAudioStreamEnsure(const QString& reason)
{
    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    if (!pcAudio) {
        qCDebug(lcProtocol) << "RadioModel: PC audio disabled — skipping remote_audio_rx";
        if (m_rxAudio.streamId != 0) {
            qCDebug(lcProtocol) << "RadioModel: removing unexpected owned remote_audio_rx while PC audio is disabled";
            removeRxAudioStream();
        }
        logRemoteAudioRxSummary(QStringLiteral("ensure skipped: not needed"));
        return;
    }

    logRemoteAudioRxSummary(QStringLiteral("ensure scheduled: ") + reason);
    QTimer::singleShot(350, this, [this, reason]() {
        const bool pcAudioNow = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
        if (!isConnected()) {
            logRemoteAudioRxSummary(QStringLiteral("ensure canceled: disconnected"));
            return;
        }
        if (!pcAudioNow) {
            logRemoteAudioRxSummary(QStringLiteral("ensure canceled: no longer needed"));
            return;
        }

        qCDebug(lcProtocol) << "RadioModel: ensuring remote_audio_rx stream after status settle for"
                            << reason;
        createRxAudioStream();
    });
}

bool RadioModel::handleRemoteAudioRxStreamStatus(const QString& object,
                                                 const QMap<QString, QString>& kvs)
{
    const bool allowUnknownOwner = m_clientInfoMap.size() <= 1;
    const auto action = RadioStatusOwnership::applyRemoteAudioRxStatus(
        m_rxAudio, object, kvs, clientHandle(), allowUnknownOwner);

    if (action == RadioStatusOwnership::RemoteAudioRxAction::NotRemoteAudio)
        return false;

    const auto stream = RadioStatusOwnership::parseStreamObject(object);
    const QString streamText = stream.valid
        ? RadioStatusOwnership::hexId(stream.streamId)
        : QStringLiteral("(unknown)");

    switch (action) {
    case RadioStatusOwnership::RemoteAudioRxAction::DeferredUnknownOwner:
        qCDebug(lcProtocol) << "RadioModel: deferred remote_audio_rx status without client_handle"
                            << streamText;
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::IgnoredOtherClient:
        qCDebug(lcProtocol) << "RadioModel: ignored remote_audio_rx for another client"
                            << streamText;
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Adopted:
        qCDebug(lcProtocol) << "RadioModel: adopted owned remote_audio_rx status"
                            << streamText;
        logRemoteAudioRxSummary(QStringLiteral("status adopted"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Updated:
        qCDebug(lcProtocol) << "RadioModel: updated owned remote_audio_rx status"
                            << streamText;
        logRemoteAudioRxSummary(QStringLiteral("status updated"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Removed:
        qCDebug(lcProtocol) << "RadioModel: owned remote_audio_rx removed"
                            << streamText;
        logRemoteAudioRxSummary(QStringLiteral("status removed"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::NotRemoteAudio:
        break;
    }

    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    if (!pcAudio && m_rxAudio.streamId != 0
        && (action == RadioStatusOwnership::RemoteAudioRxAction::Adopted
            || action == RadioStatusOwnership::RemoteAudioRxAction::Updated)) {
        qCDebug(lcProtocol) << "RadioModel: removing restored remote_audio_rx because PC audio is disabled";
        removeRxAudioStream();
        return true;
    }

    if (m_rxAudio.removeRequested && m_rxAudio.streamId != 0)
        removeRxAudioStream();

    return true;
}

void RadioModel::logRemoteAudioRxSummary(const QString& reason) const
{
    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    const bool autoStartTci = AppSettings::instance().value("AutoStartTCI", "False").toString() == "True";
    const bool ownerKnown = m_rxAudio.clientHandle != 0;
    const bool ownedByUs = ownerKnown && m_rxAudio.clientHandle == clientHandle();

    QStringList fields;
    fields << QStringLiteral("reason=\"%1\"").arg(reason);
    fields << QStringLiteral("stream=%1").arg(
        m_rxAudio.streamId == 0 ? QStringLiteral("(none)")
                                : RadioStatusOwnership::hexId(m_rxAudio.streamId));
    fields << QStringLiteral("owner=%1").arg(
        ownerKnown ? RadioStatusOwnership::hexId(m_rxAudio.clientHandle)
                   : QStringLiteral("(unknown)"));
    fields << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                       : QStringLiteral("?"));
    fields << QStringLiteral("pc_audio=%1").arg(pcAudio ? 1 : 0);
    fields << QStringLiteral("auto_tci=%1").arg(autoStartTci ? 1 : 0);
    fields << QStringLiteral("pending=%1").arg(m_rxAudio.createPending ? 1 : 0);
    fields << QStringLiteral("remove_requested=%1").arg(m_rxAudio.removeRequested ? 1 : 0);
    fields << QStringLiteral("status_seen=%1").arg(m_rxAudio.statusSeen ? 1 : 0);
    if (!m_rxAudio.compression.isEmpty())
        fields << QStringLiteral("compression=%1").arg(m_rxAudio.compression);

    qCInfo(lcProtocol).noquote()
        << "RadioModel: remote_audio_rx summary" << fields.join(QLatin1Char(' '));
}

quint32 RadioModel::sendCmd(const QString& command, ResponseCallback cb)
{
    auto& perf = PerfTelemetry::instance();
    if (perf.enabled()
        && command.startsWith(QStringLiteral("display pan set "))
        && command.contains(QStringLiteral(" center="))) {
        perf.recordPanCenterCommand();
    }

    if (m_wanConn)
        return m_wanConn->sendCommand(command, std::move(cb));

    // Allocate seq on main thread, store callback locally. (#502)
    const quint32 seq = m_seqCounter.fetch_add(1);
    if (cb)
        m_pendingCallbacks.insert(seq, std::move(cb));

    // Queue the socket write on the connection's worker thread.
    QMetaObject::invokeMethod(m_connection, [conn = m_connection, seq, command] {
        conn->writeCommand(seq, command);
    });
    return seq;
}

quint32 RadioModel::clientHandle() const
{
    if (m_wanConn)
        return m_wanConn->clientHandle();
    return m_connection->clientHandle();
}

PanadapterModel* RadioModel::ensureOwnedPanadapter(const QString& panId)
{
    const QString normalizedPanId = normalizePanadapterId(panId);
    if (normalizedPanId.isEmpty())
        return nullptr;

    if (auto* existing = m_panadapters.value(normalizedPanId, nullptr))
        return existing;

    auto* pan = new PanadapterModel(normalizedPanId, this);
    pan->setClientHandle(QString::number(clientHandle(), 16));
    m_panadapters[normalizedPanId] = pan;
    if (m_activePanId.isEmpty())
        m_activePanId = normalizedPanId;

    connect(pan, &PanadapterModel::waterfallIdChanged,
            this, &RadioModel::updateStreamFilters);
    updateStreamFilters();

    sendCmd(QString("display pan rfgain_info %1").arg(normalizedPanId),
            [pan](int code, const QString& body) {
        if (code != 0 || body.isEmpty()) return;
        QStringList vals = body.split(',');
        if (vals.size() < 3) return;
        int low = vals[0].trimmed().toInt();
        int high = vals[1].trimmed().toInt();
        int step = vals[2].trimmed().toInt();
        if (step > 0)
            pan->setRfGainInfo(low, high, step);
    });

    qCDebug(lcProtocol) << "RadioModel: claimed panadapter" << normalizedPanId;
    emit panadapterAdded(pan);

    const auto pending = m_pendingPanStatuses.take(normalizedPanId);
    if (!pending.isEmpty()) {
        qCDebug(lcProtocol) << "RadioModel: applying deferred panadapter status for"
                            << normalizedPanId;
        handlePanadapterStatus(normalizedPanId, pending);
    }

    return pan;
}

quint32 RadioModel::ourClientHandle() const { return clientHandle(); }

void RadioModel::emitOtherClientsChanged()
{
    quint32 ours = clientHandle();
    QStringList names;
    for (auto it = m_clientStations.cbegin(); it != m_clientStations.cend(); ++it) {
        if (it.key() != ours)
            names << it.value();
    }
    emit otherClientsChanged(names.size(), names);
}

void RadioModel::traceDaxStreamStatus(const QString& object,
                                      const QMap<QString, QString>& kvs)
{
    const auto stream = parseStreamObject(object, QStringLiteral("stream"));
    if (stream.valid) {
        const QString incomingType = kvs.value(QStringLiteral("type"));
        const QString knownType = m_daxStreamDebug.value(stream.streamId).type;
        const bool daxRelated = isDaxStreamType(incomingType)
            || isDaxStreamType(knownType)
            || stream.streamId == m_daxTxStreamId;
        if (!daxRelated)
            return;

        const bool removed = streamStatusRemoved(stream, kvs);
        const QString type = incomingType.isEmpty() ? knownType : incomingType;
        if (removed) {
            qCDebug(lcDax).noquote()
                << "RadioModel: DAX stream removed"
                << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                << QStringLiteral("type=%1").arg(type.isEmpty() ? QStringLiteral("(unknown)") : type)
                << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
            if (stream.streamId == m_daxTxStreamId) {
                m_daxTxStreamId = 0;
                m_daxTxActive = false;
                m_daxTxClientHandle = 0;
                m_daxTxCreatePending = false;
            }
            m_deadDaxRxSeen.remove(stream.streamId);
            m_externalDaxTxSeen.remove(stream.streamId);
            m_externalDaxRxSeen.remove(stream.streamId);
            m_daxStreamDebug.remove(stream.streamId);
            return;
        }

        auto& state = m_daxStreamDebug[stream.streamId];
        if (!incomingType.isEmpty())
            state.type = incomingType;
        if (kvs.contains(QStringLiteral("client_handle")))
            state.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
        if (kvs.contains(QStringLiteral("dax_channel")))
            state.daxChannel = kvs.value(QStringLiteral("dax_channel")).toInt();
        if (kvs.contains(QStringLiteral("daxiq_channel")))
            state.daxIqChannel = kvs.value(QStringLiteral("daxiq_channel")).toInt();
        if (kvs.contains(QStringLiteral("slice")))
            state.sliceId = kvs.value(QStringLiteral("slice")).toInt();
        if (kvs.contains(QStringLiteral("daxiq_rate")))
            state.daxIqRate = kvs.value(QStringLiteral("daxiq_rate")).toInt();
        if (kvs.contains(QStringLiteral("pan")))
            state.panId = kvs.value(QStringLiteral("pan"));
        if (kvs.contains(QStringLiteral("ip")))
            state.ip = kvs.value(QStringLiteral("ip")).trimmed();
        if (kvs.contains(QStringLiteral("active"))) {
            state.active = kvs.value(QStringLiteral("active")) == QStringLiteral("1");
            state.activeKnown = true;
        }
        if (kvs.contains(QStringLiteral("tx"))) {
            state.tx = kvs.value(QStringLiteral("tx")) == QStringLiteral("1");
            state.txKnown = true;
        }

        if (state.type == QStringLiteral("dax_rx") && isDeadOrphanDaxRxStatus(kvs)) {
            if (!m_deadDaxRxSeen.contains(stream.streamId)) {
                m_deadDaxRxSeen.insert(stream.streamId);
                qCWarning(lcDax).noquote()
                    << "RadioModel: ignoring dead DAX RX stream status"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("dax_ch=%1").arg(state.daxChannel)
                    << QStringLiteral("slice=%1").arg(state.sliceId >= 0
                        ? QString::number(state.sliceId)
                        : QStringLiteral("?"))
                    << QStringLiteral("ip=%1").arg(state.ip);
            }
        }

        const bool ownerKnown = state.clientHandle != 0;
        const bool ownedByUs = ownerKnown && state.clientHandle == clientHandle();
        if (state.type == QStringLiteral("dax_tx") && ownerKnown && !ownedByUs) {
            if (!m_externalDaxTxSeen.contains(stream.streamId)) {
                m_externalDaxTxSeen.insert(stream.streamId);
                qCInfo(lcDax).noquote()
                    << "RadioModel: external DAX TX stream observed"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("owner=%1").arg(hexId(state.clientHandle));
            }
        } else if (state.type == QStringLiteral("dax_rx") && ownerKnown && !ownedByUs) {
            if (!m_externalDaxRxSeen.contains(stream.streamId)) {
                m_externalDaxRxSeen.insert(stream.streamId);
                qCInfo(lcDax).noquote()
                    << "RadioModel: external DAX RX stream observed"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("owner=%1").arg(hexId(state.clientHandle))
                    << QStringLiteral("dax_ch=%1").arg(state.daxChannel)
                    << QStringLiteral("slice=%1").arg(state.sliceId >= 0
                        ? QString::number(state.sliceId)
                        : QStringLiteral("?"))
                    << QStringLiteral("ip=%1").arg(state.ip.isEmpty()
                        ? QStringLiteral("?")
                        : state.ip);
            }
        }

        if (state.type == QStringLiteral("dax_tx")
            && daxTxStatusCanUpdateLocalState(stream.streamId, m_daxTxStreamId, kvs, clientHandle())) {
            m_daxTxStreamId = stream.streamId;
            m_daxTxActive = state.tx;
            m_daxTxClientHandle = state.clientHandle;
            if (ownedByUs)
                m_daxTxCreatePending = false;
        }

        QStringList fields;
        fields << QStringLiteral("stream=%1").arg(hexId(stream.streamId));
        fields << QStringLiteral("type=%1").arg(state.type.isEmpty() ? QStringLiteral("(unknown)") : state.type);
        fields << QStringLiteral("owner=%1").arg(ownerKnown ? hexId(state.clientHandle) : QStringLiteral("(unknown)"));
        fields << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                            : QStringLiteral("?"));
        if (state.daxChannel > 0)
            fields << QStringLiteral("dax_ch=%1").arg(state.daxChannel);
        if (state.daxIqChannel > 0)
            fields << QStringLiteral("daxiq_ch=%1").arg(state.daxIqChannel);
        if (state.sliceId >= 0)
            fields << QStringLiteral("slice=%1").arg(state.sliceId);
        if (state.daxIqRate > 0)
            fields << QStringLiteral("daxiq_rate=%1").arg(state.daxIqRate);
        if (!state.panId.isEmpty())
            fields << QStringLiteral("pan=%1").arg(state.panId);
        if (!state.ip.isEmpty())
            fields << QStringLiteral("ip=%1").arg(state.ip);
        if (state.activeKnown)
            fields << QStringLiteral("active=%1").arg(state.active ? 1 : 0);
        if (state.txKnown)
            fields << QStringLiteral("tx=%1").arg(state.tx ? 1 : 0);
        fields << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));

        qCDebug(lcDax).noquote()
            << "RadioModel: DAX stream status" << fields.join(QLatin1Char(' '));
        return;
    }

    const auto txAudioStream = parseStreamObject(object, QStringLiteral("tx_audio_stream"));
    if (!txAudioStream.valid)
        return;

    const bool removed = streamStatusRemoved(txAudioStream, kvs);
    if (removed) {
        qCDebug(lcDax).noquote()
            << "RadioModel: DAX tx_audio_stream removed"
            << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
            << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
        if (txAudioStream.streamId == m_daxTxStreamId) {
            m_daxTxStreamId = 0;
            m_daxTxActive = false;
            m_daxTxClientHandle = 0;
            m_daxTxCreatePending = false;
        }
        m_daxStreamDebug.remove(txAudioStream.streamId);
        m_externalDaxTxSeen.remove(txAudioStream.streamId);
        return;
    }

    auto& state = m_daxStreamDebug[txAudioStream.streamId];
    state.type = QStringLiteral("dax_tx");
    if (kvs.contains(QStringLiteral("client_handle")))
        state.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
    if (kvs.contains(QStringLiteral("tx"))) {
        state.tx = kvs.value(QStringLiteral("tx")) == QStringLiteral("1");
        state.txKnown = true;
    }
    const bool ownerKnown = state.clientHandle != 0;
    const bool ownedByUs = ownerKnown && state.clientHandle == clientHandle();
    if (ownerKnown && !ownedByUs && !m_externalDaxTxSeen.contains(txAudioStream.streamId)) {
        m_externalDaxTxSeen.insert(txAudioStream.streamId);
        qCInfo(lcDax).noquote()
            << "RadioModel: external DAX TX stream observed"
            << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
            << QStringLiteral("owner=%1").arg(hexId(state.clientHandle));
    }
    if (daxTxStatusCanUpdateLocalState(txAudioStream.streamId, m_daxTxStreamId, kvs, clientHandle())) {
        m_daxTxStreamId = txAudioStream.streamId;
        m_daxTxActive = state.tx;
        m_daxTxClientHandle = state.clientHandle;
        if (ownedByUs)
            m_daxTxCreatePending = false;
    }
    qCDebug(lcDax).noquote()
        << "RadioModel: DAX tx_audio_stream status"
        << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
        << QStringLiteral("owner=%1").arg(ownerKnown ? hexId(state.clientHandle) : QStringLiteral("(unknown)"))
        << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                    : QStringLiteral("?"))
        << QStringLiteral("tx=%1").arg(state.txKnown ? (state.tx ? QStringLiteral("1") : QStringLiteral("0"))
                                                     : QStringLiteral("?"))
        << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
}

void RadioModel::onStatusReceived(const QString& object,
                                  const QMap<QString, QString>& kvs)
{
    // Relay to listeners (e.g., MemoryDialog)
    emit statusReceived(object, kvs);

    handleRemoteAudioRxStreamStatus(object, kvs);
    traceDaxStreamStatus(object, kvs);

    if (object == "radio") {
        handleRadioStatus(kvs);
        return;
    }

    // Client connected/disconnected:
    //   object="client 0x7594C952"       kvs={connected, program=SmartSDR, station=W1AW}
    //   object="client 0x7594C952 disconnected"  kvs={forced=0, ...}
    static const QRegularExpression clientRe(R"(^client\s+(0x[0-9A-Fa-f]+)(?:\s+(\w+))?$)");
    if (object.startsWith("client 0x")) {
        const auto cm = clientRe.match(object);
        if (cm.hasMatch()) {
            quint32 handle = cm.captured(1).toUInt(nullptr, 16);
            QString action = cm.captured(2);  // "disconnected" or empty

            if (action == "disconnected") {
                if (handle == clientHandle() && statusFlagSet(kvs, QStringLiteral("forced")))
                    handleForcedClientDisconnect();
                m_clientStations.remove(handle);
                m_clientInfoMap.remove(handle);
                m_announcedClientConnections.remove(handle);
                m_startupClientConnections.remove(handle);
                emitOtherClientsChanged();
            } else if (action == "connected" || kvs.contains("connected")) {
                QString program = cleanClientText(kvs.value("program", "Unknown"));
                QString station = cleanClientText(kvs.value("station", program));
                QString source = clientConnectionSource(kvs);
                auto existing = m_clientInfoMap.constFind(handle);
                if (source.isEmpty() && existing != m_clientInfoMap.cend())
                    source = existing->source;
                if (source.isEmpty() && m_wanConn)
                    source = QStringLiteral("SmartLink");
                bool ptt = kvs.value("local_ptt", "0") == "1";
                m_clientStations[handle] = station;
                ClientInfo client;
                client.station = station;
                client.program = program;
                client.source = source;
                client.localPtt = ptt;
                m_clientInfoMap[handle] = client;
                emitOtherClientsChanged();
                if (!shouldSuppressClientConnectionNotice(handle))
                    announceClientConnection(handle, source, station, program);
            } else if (kvs.contains("local_ptt") && m_clientInfoMap.contains(handle)) {
                // Partial update: radio echoes local_ptt state change without
                // a full connected message (e.g. after enforce_local_ptt command).
                m_clientInfoMap[handle].localPtt = kvs.value("local_ptt") == "1";
                emitOtherClientsChanged();
            }
        }
        return;
    }

    // XVTR status: "xvtr 0 name=2m rf_freq=144.000000 if_freq=28.000000 ..."
    static const QRegularExpression xvtrRe(R"(^xvtr\s+(\d+)$)");
    if (object.startsWith("xvtr")) {
        const auto m = xvtrRe.match(object);
        if (m.hasMatch()) {
            int idx = m.captured(1).toInt();
            // "in_use=0" means the xvtr was removed
            if (kvs.contains("in_use") && kvs["in_use"] == "0") {
                const auto existing = m_xvtrList.constFind(idx);
                if (existing != m_xvtrList.cend()) {
                    qCDebug(lcProtocol).noquote().nospace()
                        << "RadioModel: xvtr removed idx=" << idx
                        << " name=" << (existing->name.isEmpty() ? QStringLiteral("(unnamed)") : existing->name)
                        << " order=" << existing->order
                        << " is_valid=" << existing->isValid
                        << " has_is_valid=" << existing->hasIsValid;
                } else {
                    qCDebug(lcProtocol).noquote().nospace()
                        << "RadioModel: xvtr removed idx=" << idx
                        << " name=(unknown)";
                }
                m_xvtrList.remove(idx);
                emit infoChanged();
                return;
            }
            auto& x = m_xvtrList[idx];
            x.index = idx;
            if (kvs.contains("order"))     x.order   = kvs["order"].toInt();
            if (kvs.contains("name"))      x.name     = kvs["name"];
            if (kvs.contains("rf_freq"))   x.rfFreq   = kvs["rf_freq"].toDouble();
            if (kvs.contains("if_freq"))   x.ifFreq   = kvs["if_freq"].toDouble();
            if (kvs.contains("lo_error"))  x.loError  = kvs["lo_error"].toDouble();
            if (kvs.contains("rx_gain"))   x.rxGain   = kvs["rx_gain"].toDouble();
            if (kvs.contains("max_power")) x.maxPower = kvs["max_power"].toDouble();
            if (kvs.contains("rx_only"))   x.rxOnly   = kvs["rx_only"] == "1";
            const bool statusHasIsValid = kvs.contains("is_valid");
            if (statusHasIsValid) {
                x.isValid = kvs["is_valid"] == "1";
                x.hasIsValid = true;
            }
            qCDebug(lcProtocol).noquote().nospace()
                << "RadioModel: xvtr status idx=" << x.index
                << " name=" << (x.name.isEmpty() ? QStringLiteral("(unnamed)") : x.name)
                << " order=" << x.order
                << " rf_mhz=" << x.rfFreq
                << " if_mhz=" << x.ifFreq
                << " offset_mhz=" << (x.rfFreq - x.ifFreq)
                << " rx_only=" << x.rxOnly
                << " max_power=" << x.maxPower
                << " is_valid=" << x.isValid
                << " status_has_is_valid=" << statusHasIsValid
                << " has_is_valid=" << x.hasIsValid;
            emit infoChanged();
        }
        return;
    }

    // Filter sharpness: "radio filter_sharpness VOICE level=3 auto_level=0"
    if (object.startsWith("radio filter_sharpness")) {
        int level = kvs.value("level", "-1").toInt();
        bool autoLvl = kvs.value("auto_level", "0") == "1";
        if (object.contains("VOICE"))       { m_filterVoice = level; m_filterVoiceAuto = autoLvl; }
        else if (object.contains("CW"))     { m_filterCw = level; m_filterCwAuto = autoLvl; }
        else if (object.contains("DIGITAL")){ m_filterDigital = level; m_filterDigitalAuto = autoLvl; }
        emit infoChanged();
        return;
    }

    // License info (fw v1.4.0.0): three sub-objects per FlexLib:
    //   "license"              — radio_id, issued, last_refreshed_date, highest_major_version, region
    //   "license subscription" — name=smartsdr+|smartsdr+_early_access, expiration=<date>
    //   "license feature"      — name, enabled, reason (BUILT_IN|LICENSE_FILE|PLUS|EA)
    if (object == "license" && !kvs.contains("name")) {
        if (kvs.contains("radio_id")) {
            m_licenseRadioId = kvs["radio_id"].toUpper();
        }
        if (kvs.contains("highest_major_version")) {
            m_licenseMaxVersion = kvs["highest_major_version"];
        }
        // Base subscription is always "SmartSDR" — upgraded by subscription messages
        if (m_licenseSubscription.isEmpty()) {
            m_licenseSubscription = "SmartSDR";
        }
        emit infoChanged();
        return;
    }
    if (object == "license subscription") {
        // Per FlexLib: name=smartsdr+ or name=smartsdr+_early_access
        // with expiration=<ISO-8601 date>
        QString name = kvs.value("name").toLower();
        QString expStr = kvs.value("expiration");
        QDate expDate = QDate::fromString(expStr.left(10), Qt::ISODate);
        bool active = expDate.isValid() && expDate >= QDate::currentDate();
        if (name == "smartsdr+_early_access" && active) {
            m_licenseSubscription = "SmartSDR+ Early Access";
            m_licenseExpirationDate = expDate.toString("MM/dd/yyyy");
        } else if (name == "smartsdr+" && active) {
            m_licenseSubscription = "SmartSDR+";
            m_licenseExpirationDate = expDate.toString("MM/dd/yyyy");
        }
        emit infoChanged();
        return;
    }
    if (object == "license feature") {
        // Feature-level parsing — not needed for display, but log for debugging
        emit infoChanged();
        return;
    }

    if (object == "radio oscillator") {
        if (kvs.contains("state"))        m_oscState    = kvs["state"];
        if (kvs.contains("setting"))      m_oscSetting  = kvs["setting"];
        if (kvs.contains("locked"))       m_oscLocked   = kvs["locked"] == "1";
        if (kvs.contains("ext_present"))  m_extPresent  = kvs["ext_present"] == "1";
        if (kvs.contains("gpsdo_present"))m_gpsdoPresent= kvs["gpsdo_present"] == "1";
        if (kvs.contains("tcxo_present")) m_tcxoPresent = kvs["tcxo_present"] == "1";
        if (kvs.contains("gnss_present")) m_gpsdoPresent= m_gpsdoPresent || kvs["gnss_present"] == "1";
        emit oscillatorChanged();
        emit infoChanged();
        return;
    }

    if (object == "radio static_net_params") {
        m_staticIp      = kvs.value("ip");
        m_staticNetmask = kvs.value("netmask");
        m_staticGateway = kvs.value("gateway");
        m_hasStaticIp   = !m_staticIp.isEmpty();
        emit infoChanged();
        return;
    }

    static const QRegularExpression sliceRe(R"(^slice\s+(\d+)$)");
    const auto sliceMatch = sliceRe.match(object);
    if (sliceMatch.hasMatch()) {
        // Extract per-client TX info for multiFLEX dashboard before
        // handleSliceStatus filters out other clients' slices
        if (kvs.contains("client_handle") && kvs.value("tx") == "1") {
            quint32 ch = kvs["client_handle"].toUInt(nullptr, 16);
            auto it = m_clientInfoMap.find(ch);
            if (it != m_clientInfoMap.end()) {
                it->txAntenna = kvs.value("txant");
                if (kvs.contains("RF_frequency"))
                    it->txFreqMhz = kvs["RF_frequency"].toDouble();
            }
        }
        const bool removed = kvs.value("in_use") == "0";
        handleSliceStatus(sliceMatch.captured(1).toInt(), kvs, removed);
        return;
    }

    // Memory channels: "memory <index> key=val ..." or "memory <index> removed"
    // When there are no KV pairs (e.g., "memory 7 removed"), the parser puts
    // everything into the object name. Extract the index from the first token.
    if (object.startsWith("memory ")) {
        const QString rest = object.mid(7);  // "7 removed" or "7"
        const int sp = rest.indexOf(' ');
        const QString idxStr = (sp >= 0) ? rest.left(sp) : rest;
        bool ok;
        int idx = idxStr.toInt(&ok);
        if (ok) {
            // Merge any trailing bare words into kvs
            QMap<QString, QString> merged = kvs;
            if (sp >= 0) {
                const QString extra = rest.mid(sp + 1);
                for (const auto& token : extra.split(' ', Qt::SkipEmptyParts)) {
                    const int eq = token.indexOf('=');
                    if (eq < 0)
                        merged.insert(token, QString{});
                    else
                        merged.insert(token.left(eq), token.mid(eq + 1));
                }
            }
            qCDebug(lcProtocol) << "RadioModel: memory status for index" << idx
                     << "keys:" << merged.keys();
            handleMemoryStatus(idx, merged);
            return;
        }
    }

    // Meter status uses '#'-separated tokens and is handled by onMessageReceived().

    // "display pan 0x40000000 center=14.1 bandwidth=0.2 ..."
    // Only process status for OUR panadapter (matching client_handle or first unclaimed).
    static const QRegularExpression panRe(R"(^display pan\s+(0x[0-9A-Fa-f]+))");
    if (object.startsWith("display pan")) {
        const auto m = panRe.match(object);
        if (m.hasMatch()) {
            const QString panId = m.captured(1);

            // Handle pan removal — "display pan 0x40000001 removed" arrives
            // with no '=' so the parser puts the whole string in 'object'
            if (kvs.contains("removed") || object.endsWith("removed")) {
                m_pendingPanStatuses.remove(panId);
                auto* pan = m_panadapters.take(panId);
                if (pan) {
                    m_panStream->unregisterPanStream(pan->panStreamId());
                    m_panStream->unregisterWfStream(pan->wfStreamId());
                    qCDebug(lcProtocol) << "RadioModel: panadapter removed" << panId;
                    emit panadapterRemoved(panId);
                    pan->deleteLater();
                }
                if (m_activePanId == panId) {
                    m_activePanId = m_panadapters.isEmpty() ? QString()
                                                            : m_panadapters.firstKey();
                }
                return;
            }

            // Preamp is shared antenna hardware — apply to ALL our pans
            // regardless of which client's pan status this came from.
            if (kvs.contains("pre")) {
                const QString pre = kvs["pre"];
                for (auto* pan : m_panadapters)
                    pan->setPreamp(pre);
            }

            const bool knownPan = m_panadapters.contains(panId);
            const auto ownershipAction = RadioStatusOwnership::classifyOwnedStatus(
                knownPan, kvs, false, clientHandle());
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Defer) {
                m_pendingPanStatuses[panId] = kvs;
                return;  // defer — can't confirm ownership yet
            }
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Ignore) {
                m_pendingPanStatuses.remove(panId);
                return;  // not our panadapter, ignore
            }
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Claim)
                ensureOwnedPanadapter(panId);
            handlePanadapterStatus(panId, kvs);
        }
        return;
    }

    // "display waterfall 0x42000000 auto_black=1 ..."
    // Only process status for OUR waterfall (matching client_handle).
    static const QRegularExpression wfRe(R"(^display waterfall\s+(0x[0-9A-Fa-f]+)$)");
    if (object.startsWith("display waterfall")) {
        const auto m = wfRe.match(object);
        if (m.hasMatch()) {
            const QString wfId = m.captured(1);
            // Check if this waterfall belongs to one of our panadapters.
            // The waterfallId is set on PanadapterModel by the "display pan" status
            // message which contains "waterfall=0x42xxxxxx".
            bool ours = false;
            PanadapterModel* ownerPan = nullptr;
            for (auto* pan : m_panadapters) {
                if (pan->waterfallId() == wfId) {
                    ours = true;
                    ownerPan = pan;
                    break;
                }
            }
            const QString parentPanId = normalizePanadapterId(kvs.value(QStringLiteral("panadapter")));
            if (!ownerPan && !parentPanId.isEmpty())
                ownerPan = m_panadapters.value(parentPanId, nullptr);
            if (!ours) {
                // Not yet associated via display pan status — check client_handle
                if (!kvs.contains("client_handle"))
                    return;  // defer — can't confirm ownership yet
                quint32 owner = parseClientHandle(kvs["client_handle"]);
                if (owner != clientHandle())
                    return;  // not our waterfall
                if (!ownerPan && !parentPanId.isEmpty())
                    ownerPan = ensureOwnedPanadapter(parentPanId);
                if (ownerPan && ownerPan->waterfallId().isEmpty())
                    ownerPan->setWaterfallId(wfId);
                ours = true;
            }

            if (ownerPan)
                ownerPan->applyWaterfallStatus(kvs);
            if (activeWfId().isEmpty() && ownerPan == activePanadapter())
                ownerPan->setWaterfallId(wfId);
            updateStreamFilters();
            qCDebug(lcProtocol) << "RadioModel: claimed waterfall" << wfId;
        }
        if (!activeWfConfigured() && !activeWfId().isEmpty() && isConnected()) {
            setActiveWfConfigured(true);
            configureWaterfall();
        }
        return;
    }

    // ATU status: "atu <handle> status=TUNE_SUCCESSFUL atu_enabled=1 ..."
    // Routes to TransmitModel for the TX applet ATU controls.
    // Also forwards to TunerModel if an external TGXL is connected.
    static const QRegularExpression atuRe(R"(^atu\s+(\S+)$)");
    if (object.startsWith("atu")) {
        const auto m = atuRe.match(object);
        if (m.hasMatch() && m_tunerModel.handle().isEmpty())
            m_tunerModel.setHandle(m.captured(1));
        m_transmitModel.applyAtuStatus(kvs);
        if (m_tunerModel.isPresent())
            m_tunerModel.applyStatus(kvs);
        return;
    }

    // APD status family.  Forms:
    //   "apd enable=1 configurable=1"               → object "apd"
    //   "apd equalizer_active=1 ant=ANT1 freq=... rfpower=..."  → object "apd"
    //   "apd equalizer_reset"                        → object "apd equalizer_reset"
    //   "apd sampler tx_ant=ANT1 selected_sampler=RX_A valid_samplers=..."
    //                                                → object "apd sampler"
    // Our parser splits object/kvs at the last space before the first '=',
    // so any leading bare flags get absorbed into the object name.
    if (object == "apd sampler") {
        m_transmitModel.applyApdSamplerStatus(kvs);
        return;
    }
    if (object == "apd" || object.startsWith("apd ")) {
        QMap<QString, QString> merged = kvs;
        if (object.length() > 3) {
            const QStringList flags = object.mid(4).split(' ', Qt::SkipEmptyParts);
            for (const auto& f : flags) merged.insert(f, QString{});
        }
        m_transmitModel.applyApdStatus(merged);
        return;
    }

    // Amplifier status: both TGXL and PGXL report via the amplifier API.
    // FlexLib distinguishes them by the "model" key:
    //   model=TunerGeniusXL  → antenna tuner (TGXL)
    //   model=PowerGeniusXL  → power amplifier (PGXL)
    // "amplifier <handle> model=TunerGeniusXL operate=1 relayC1=20 ..."
    //
    // Removal can arrive in two forms (matches FlexLib Radio.cs:14060/14073
    // which uses substring `s.Contains("removed")`):
    //   1) bare:  "amplifier <handle> removed"        → trailing token, no '='
    //   2) kvs:   "amplifier <handle> ... removed=1"  → key in kvs map
    // Form 1 lands in `object` because our parser treats a body with no '='
    // as all-object-name; form 2 lands in `kvs` as a normal key.
    static const QRegularExpression ampRe(R"(^amplifier\s+(\S+)$)");
    static const QRegularExpression ampRemovedRe(R"(^amplifier\s+(\S+)\s+removed$)");
    if (object.startsWith("amplifier")) {
        const auto rm = ampRemovedRe.match(object);
        if (rm.hasMatch()) {
            const QString handle = rm.captured(1);
            qCDebug(lcProtocol) << "RadioModel: amplifier removed (bare) handle=" << handle;
            if (handle == m_tunerModel.handle())
                m_tunerModel.setHandle({});
            if (handle == m_ampHandle) {
                m_ampHandle.clear();
                m_hasAmplifier = false;
                m_ampModel.clear();
                emit amplifierChanged(false);
            }
            return;
        }
        const auto m = ampRe.match(object);
        if (m.hasMatch()) {
            const QString handle = m.captured(1);
            const QString model = kvs.value("model");
            qCDebug(lcProtocol) << "RadioModel: amplifier status handle=" << handle << "model=" << model;

            // Handle removal (kvs form)
            if (kvs.contains("removed")) {
                if (handle == m_tunerModel.handle())
                    m_tunerModel.setHandle({});
                if (handle == m_ampHandle) {
                    m_ampHandle.clear();
                    m_hasAmplifier = false;
                    m_ampModel.clear();
                    emit amplifierChanged(false);
                }
                return;
            }

            // Route TunerGeniusXL to TunerModel
            if (model == "TunerGeniusXL" || handle == m_tunerModel.handle()) {
                // Always update handle — first status may arrive with 0x00000000
                // before the real handle is assigned
                if (handle != "0x00000000" && handle != m_tunerModel.handle()) {
                    m_tunerModel.setHandle(handle);
                    m_meterModel.setTgxlHandle(handle.toUInt(nullptr, 0));
                } else if (m_tunerModel.handle().isEmpty()) {
                    m_tunerModel.setHandle(handle);
                    m_meterModel.setTgxlHandle(handle.toUInt(nullptr, 0));
                }
                m_tunerModel.applyStatus(kvs);
            }

            // Track power amplifier state (PGXL or any non-TGXL amp).
            // PGXL uses "state=OPERATE|IDLE|BYPASS|..." (not operate=/bypass= like TGXL)
            if (!model.isEmpty() && model != "TunerGeniusXL") {
                m_ampHandle = handle;
                if (!m_hasAmplifier) {
                    m_hasAmplifier = true;
                    m_ampIp = kvs.value("ip");
                    m_ampModel = model;
                    qCDebug(lcProtocol) << "RadioModel: power amplifier detected, model=" << model
                                       << "ip=" << m_ampIp;
                    emit amplifierChanged(true);
                }
            }
            if (!m_ampHandle.isEmpty() && handle == m_ampHandle) {
                const QString state = kvs.value("state").toUpper();
                if (!state.isEmpty()) {
                    // PGXL states: IDLE = on/ready, STANDBY = off, POWERUP = transitioning
                    bool op = (state == "IDLE" || state == "OPERATE"
                               || state.startsWith("TRANSMIT"));
                    if (m_ampOperate != op) {
                        m_ampOperate = op;
                        emit ampStateChanged();
                    }
                }
                // Forward the full KVS so the GUI can update telemetry fields
                // (id/drain current, vac/mains voltage, meffa, temp) without
                // requiring a direct PGXL TCP connection.
                emit ampTelemetryUpdated(kvs);
            }
        }
        return;
    }

    // Transmit status: "transmit rfpower=93 tunepower=38 tune=0 ..."
    if (object == "transmit") {
        m_transmitModel.applyTransmitStatus(kvs);
        return;
    }

    // TX profile status: "profile tx list=DAX^Default^..." or "profile tx current=Default"
    if (object.startsWith("profile")) {
        handleProfileStatus(object, kvs);
        return;
    }

    // Per-band TX settings: "transmit band 9 band_name=20 rfpower=100 ..."
    static const QRegularExpression txBandRe(R"(^transmit band\s+(\d+)$)");
    if (object.startsWith("transmit band")) {
        const auto m = txBandRe.match(object);
        if (m.hasMatch()) {
            int id = m.captured(1).toInt();
            auto& b = m_txBandSettings[id];
            b.bandId = id;
            if (kvs.contains("band_name"))    b.bandName  = kvs["band_name"];
            if (kvs.contains("rfpower"))      b.rfPower   = kvs["rfpower"].toInt();
            if (kvs.contains("tunepower"))    b.tunePower = kvs["tunepower"].toInt();
            if (kvs.contains("inhibit"))      b.inhibit   = kvs["inhibit"] == "1";
            if (kvs.contains("hwalc_enabled"))b.hwAlc     = kvs["hwalc_enabled"] == "1";
        }
        return;
    }

    // Per-band interlock: "interlock band 9 band_name=20 acc_txreq_enable=0 ..."
    static const QRegularExpression ilBandRe(R"(^interlock band\s+(\d+)$)");
    if (object.startsWith("interlock band")) {
        const auto m = ilBandRe.match(object);
        if (m.hasMatch()) {
            int id = m.captured(1).toInt();
            auto& b = m_txBandSettings[id];
            b.bandId = id;
            if (kvs.contains("band_name"))       b.bandName = kvs["band_name"];
            if (kvs.contains("acc_txreq_enable"))b.accTxReq = kvs["acc_txreq_enable"] == "1";
            if (kvs.contains("rca_txreq_enable"))b.rcaTxReq = kvs["rca_txreq_enable"] == "1";
            if (kvs.contains("acc_tx_enabled"))  b.accTx    = kvs["acc_tx_enabled"] == "1";
            if (kvs.contains("tx1_enabled"))     b.tx1      = kvs["tx1_enabled"] == "1";
            if (kvs.contains("tx2_enabled"))     b.tx2      = kvs["tx2_enabled"] == "1";
            if (kvs.contains("tx3_enabled"))     b.tx3      = kvs["tx3_enabled"] == "1";
        }
        return;
    }

    // Spot status: "spot 42 callsign=W1AW rx_freq=14.074000 ..."
    //              "spot 42 removed"
    //              "spot 42 triggered pan=0x40000000"
    if (object.startsWith("spot ")) {
        static const QRegularExpression spotRe(R"(^spot\s+(\d+))");
        const auto sm = spotRe.match(object);
        if (sm.hasMatch()) {
            int idx = sm.captured(1).toInt();
            if (kvs.isEmpty() && object.contains("removed")) {
                m_spotModel.removeSpot(idx);
            } else if (kvs.isEmpty() && object.contains("triggered")) {
                // Parse pan= from the object string if present
                static const QRegularExpression panRe2(R"(pan=(0x[0-9A-Fa-f]+))");
                const auto pm = panRe2.match(object);
                emit m_spotModel.spotTriggered(idx, pm.hasMatch() ? pm.captured(1) : QString());
            } else {
                m_spotModel.applySpotStatus(idx, kvs);
            }
        }
        return;
    }

    // USB cable status: "usb_cable FTDI-1234 type=cat enable=1 ..."
    //                   "usb_cable FTDI-1234 bit 0 enable=1 source=active_slice ..."
    //                   "usb_cable FTDI-1234 removed"
    if (object.startsWith("usb_cable ")) {
        QString rest = object.mid(10);  // after "usb_cable "
        // Serial number is the first word
        int spaceIdx = rest.indexOf(' ');
        QString sn = (spaceIdx >= 0) ? rest.left(spaceIdx) : rest;

        if (rest.contains("removed")) {
            m_usbCableModel.handleRemoved(sn);
        } else {
            // Check for bit-level status: remaining object text is "bit <N>"
            // The CommandParser puts extra object words before the KV split.
            // "usb_cable FTDI-1234 bit 3" → object="usb_cable FTDI-1234 bit 3", kvs={enable=1,...}
            QMap<QString, QString> effectiveKvs = kvs;
            if (spaceIdx >= 0) {
                QString afterSn = rest.mid(spaceIdx + 1).trimmed();
                if (afterSn.startsWith("bit ")) {
                    int bitNum = afterSn.mid(4).trimmed().toInt();
                    effectiveKvs["_bit_number"] = QString::number(bitNum);
                }
            }
            m_usbCableModel.applyStatus(sn, effectiveKvs);
        }
        return;
    }

    // CWX status: "cwx sent=0", "cwx wpm=20", "cwx macro1=CQ\u007fCQ"
    if (object == "cwx") {
        m_cwxModel.applyStatus(kvs);
        return;
    }

    // DVK status: "dvk status=idle enabled=1" or "dvk added id=1 name="Recording 1" duration=0"
    if (object.startsWith("dvk")) {
        // Pass both the object string (may contain "added"/"deleted") and KVs
        m_dvkModel.applyStatus(object, kvs);
        return;
    }

    // NAVTEX status (v4.2.18): "navtex status=Active" or "navtex sent idx=1 serial_num=42"
    if (object.startsWith("navtex")) {
        m_navtexModel.parseStatus(object, kvs);
        return;
    }

    // Interlock status: "interlock tx_client_handle=0x... state=TRANSMITTING ..."
    if (object == "interlock") {
        // Track TX ownership — only show TX state if we own the transmitter
        if (kvs.contains("tx_client_handle")) {
            quint32 txOwner = kvs["tx_client_handle"].toUInt(nullptr, 16);
            m_txClientHandle = txOwner;
            m_txOwnedByUs = (txOwner == clientHandle() || txOwner == 0);
        }
        // Parse interlock timing fields into TransmitModel (#498)
        m_transmitModel.applyInterlockStatus(kvs);

        // Track PTT source (#2373). The radio reports source=SW for software
        // MOX/CAT/xmit, and source=MIC|ACC|RCA for hardware-keyed PTT (mic
        // PTT line, footswitch via ACC, RCA TXREQ). Field is not always
        // present on every interlock status update, so persist the last
        // seen value. Matches FlexLib v4.2.18 ParsePTTSource (Radio.cs:7932).
        if (kvs.contains("source")) {
            m_lastInterlockSource = kvs["source"].toUpper();
        }

        if (kvs.contains("state")) {
            const QString state = kvs["state"].toUpper();

            // Emit raw radio TX state regardless of ownership — used by DAX
            // passthrough when an external app triggers PTT (#752).
            const bool radioTx = (state == "TRANSMITTING");
            m_radioTransmitting = radioTx;
            emit radioTransmittingChanged(radioTx);

            // Hardware PTT into the radio (mic PTT line, ACC footswitch, RCA
            // TXREQ) keys the radio without us calling setTransmit(), so
            // m_txRequested stays false. Treat hardware sources as a
            // legitimate "we own this TX" path alongside CW key, CWX, and
            // tune. SW source still requires m_txRequested so the optimistic
            // local-unkey behaviour from the TX_SYNC_FIX_REPORT is preserved
            // (a stale state=TRANSMITTING after setTransmit(false) on SW
            // source still falls through to the force-off branch).
            const bool hardwarePtt = (m_lastInterlockSource == "MIC"
                                      || m_lastInterlockSource == "ACC"
                                      || m_lastInterlockSource == "RCA");

            if (!m_txOwnedByUs ||
                (!m_txRequested && !m_cwKeyActive && !m_cwxActive
                 && !m_transmitModel.isTuning() && !hardwarePtt)) {
                // Another client owns TX, or local unkey requested:
                // force local TX/audio gate off through all interlock states.
                m_transmitModel.setTransmitting(false);
                if (m_txAudioGate) {
                    m_txAudioGate = false;
                    emit txAudioGateChanged(false);
                }
            } else if (state == "TRANSMITTING") {
                // Radio confirms RF is keyed.
                m_transmitModel.setTransmitting(true);
                if (!m_txAudioGate) {
                    m_txAudioGate = true;
                    emit txAudioGateChanged(true);
                }
            } else {
                // Local key requested but radio is still in pre-TX transition
                // (e.g. PTT/TX delay). Keep optimistic TX-on gating for
                // modem/PTT edge alignment.
                const bool transitioningToTx =
                    state.contains("REQUESTED") || state.contains("DELAY");
                if (!transitioningToTx) {
                    m_transmitModel.setTransmitting(false);
                    m_cwxActive = false; // CWX send complete (#2097)
                }
                if (!transitioningToTx && m_txAudioGate) {
                    m_txAudioGate = false;
                    emit txAudioGateChanged(false);
                }
            }

            // Clear persisted PTT source once interlock confirms we're fully
            // out of TX, so a stale hardware-PTT source can't outlive the
            // actual key release if a later status update omits source=.
            // (#2373)
            if (state != "TRANSMITTING" && !state.contains("REQUESTED")
                && !state.contains("DELAY")) {
                m_lastInterlockSource.clear();
            }

            if (state == QStringLiteral("READY") || state == QStringLiteral("RECEIVE")) {
                m_lastInterlockNotificationKey.clear();
                m_lastInterlockNotificationMs = 0;
                m_interlockNotificationArmedUntilMs = 0;
                m_interlockNotificationSource = TransmitModel::PttSource::Mox;
            } else if (interlockNotificationArmed()) {
                const QString message = radioInterlockNotificationMessage(kvs);
                if (!message.isEmpty()) {
                    emitInterlockNotification(
                        message,
                        QStringLiteral("radio:%1:%2")
                            .arg(state, kvs.value(QStringLiteral("reason")).toUpper()));
                    m_interlockNotificationArmedUntilMs = 0;
                    m_interlockNotificationSource = TransmitModel::PttSource::Mox;
                }
            }
        }
        // Emit TX ownership state for title bar indicator
        // txOwnerChanged(otherIsTx, stationName) — true when ANOTHER client has TX
        if (!m_txOwnedByUs) {
            QString station = m_clientStations.value(m_txClientHandle, "TX Not Ready");
            emit txOwnerChanged(true, station);  // another client has TX
        } else {
            emit txOwnerChanged(false, {});  // we own TX (or nobody does)
        }
        m_transmitModel.applyInterlockStatus(kvs);
        return;
    }

    // EQ status: "eq txsc mode=1 63Hz=0 125Hz=5 ..." or "eq rxsc ..."
    if (object == "eq txsc") {
        m_equalizerModel.applyTxEqStatus(kvs);
        return;
    }
    if (object == "eq rxsc") {
        m_equalizerModel.applyRxEqStatus(kvs);
        return;
    }

    // TNF status: "tnf <id> freq=14.100000 width=100 depth=1 permanent=0"
    static const QRegularExpression tnfRe(R"(^tnf\s+(\d+)$)");
    auto tnfMatch = tnfRe.match(object);
    if (tnfMatch.hasMatch()) {
        int tnfId = tnfMatch.captured(1).toInt();
        m_tnfModel.applyTnfStatus(tnfId, kvs);
        return;
    }

    // WAN, etc. — informational, ignore for now.
}

QString RadioModel::serial() const
{
    return m_lastInfo.serial;
}

void RadioModel::setRemoteOnEnabled(bool on)
{
    m_remoteOnEnabled = on;
    sendCmd(QString("radio set remote_on_enabled=%1").arg(on ? 1 : 0));
    emit infoChanged();
}

void RadioModel::setMultiFlexEnabled(bool on)
{
    m_multiFlexEnabled = on;
    sendCmd(QString("radio set mf_enable=%1").arg(on ? 1 : 0));
    emit infoChanged();
}

void RadioModel::handleRadioStatus(const QMap<QString, QString>& kvs)
{
    bool changed = false;
    if (kvs.contains("model"))    { m_model = kvs["model"]; m_maxSlices = maxSlicesForModel(m_model); changed = true; }
    if (kvs.contains("slices")) {
        // slices=N reports available (unused) slots; total capacity = open + available
        const int available = kvs["slices"].toInt();
        const int currentSliceCount = static_cast<int>(m_slices.size());
        const int modelLimit = m_model.isEmpty() ? 0 : maxSlicesForModel(m_model);
        const int reportedTotal = currentSliceCount + available;
        if (modelLimit > 0 && reportedTotal > modelLimit) {
            qCWarning(lcProtocol) << "RadioModel: ignoring impossible slice capacity"
                                  << reportedTotal << "for model" << m_model
                                  << "limit" << modelLimit
                                  << "current slices" << currentSliceCount
                                  << "reported available" << available;
        }
        const int updatedMax = RadioStatusOwnership::boundedSliceCapacity(
            modelLimit,
            m_maxSlices,
            currentSliceCount,
            available);
        if (updatedMax != m_maxSlices) {
            m_maxSlices = updatedMax;
        }
        changed = true;
    }
    if (kvs.contains("callsign")) { m_callsign = kvs["callsign"]; changed = true; }
    if (kvs.contains("nickname")) { m_nickname = kvs["nickname"]; changed = true; }
    if (kvs.contains("region"))   { m_region = kvs["region"]; changed = true; }
    if (kvs.contains("radio_options")) { m_radioOptions = kvs["radio_options"]; changed = true; }
    if (kvs.contains("remote_on_enabled")) {
        m_remoteOnEnabled = kvs["remote_on_enabled"] == "1";
        changed = true;
    }
    if (kvs.contains("mf_enable")) {
        m_multiFlexEnabled = kvs["mf_enable"] == "1";
        changed = true;
    }
    if (kvs.contains("enforce_private_ip_connections")) {
        m_enforcePrivateIp = kvs["enforce_private_ip_connections"] == "1";
        changed = true;
    }
    if (kvs.contains("binaural_rx")) {
        m_binauralRx = kvs["binaural_rx"] == "1";
        changed = true;
    }
    if (kvs.contains("full_duplex_enabled")) {
        m_fullDuplex = kvs["full_duplex_enabled"] == "1";
        changed = true;
    }
    if (kvs.contains("mute_local_audio_when_remote")) {
        m_muteLocalWhenRemote = kvs["mute_local_audio_when_remote"] == "1";
        changed = true;
    }
    if (kvs.contains("auto_save")) {
        m_autoSave = kvs["auto_save"] == "1";
        changed = true;
    }
    if (kvs.contains("freq_error_ppb")) {
        m_freqErrorPpb = kvs["freq_error_ppb"].toInt();
        changed = true;
    }
    if (kvs.contains("cal_freq")) {
        m_calFreqMhz = kvs["cal_freq"].toDouble();
        changed = true;
    }
    if (kvs.contains("low_latency_digital_modes")) {
        m_lowLatencyDigital = kvs["low_latency_digital_modes"] == "1";
        changed = true;
    }
    if (kvs.contains("rtty_mark_default")) {
        m_rttyMarkDefault = kvs["rtty_mark_default"].toInt();
        for (SliceModel* s : m_slices)
            s->setRttyMarkDefault(m_rttyMarkDefault);
        changed = true;
    }
    if (kvs.contains("tnf_enabled")) {
        m_tnfModel.applyGlobalEnabled(kvs["tnf_enabled"] == "1");
    }
    // Audio outputs
    bool audioChanged = false;
    if (kvs.contains("lineout_gain")) {
        m_lineoutGain = kvs["lineout_gain"].toInt();
        audioChanged = true;
    }
    if (kvs.contains("lineout_mute")) {
        m_lineoutMute = kvs["lineout_mute"] == "1";
        audioChanged = true;
    }
    if (kvs.contains("headphone_gain")) {
        m_headphoneGain = kvs["headphone_gain"].toInt();
        audioChanged = true;
    }
    if (kvs.contains("headphone_mute")) {
        m_headphoneMute = kvs["headphone_mute"] == "1";
        audioChanged = true;
    }
    if (kvs.contains("front_speaker_mute")) {
        m_frontSpeakerMute = kvs["front_speaker_mute"] == "1";
        audioChanged = true;
    }
    if (kvs.contains("daxiq_capacity"))
        m_daxIqModel.setCapacity(kvs["daxiq_capacity"].toInt());
    if (kvs.contains("daxiq_available"))
        m_daxIqModel.setAvailable(kvs["daxiq_available"].toInt());

    if (audioChanged) emit audioOutputChanged();
    if (changed) emit infoChanged();
}

void RadioModel::setLineoutGain(int v)
{
    v = std::clamp(v, 0, 100);
    qCDebug(lcAudio) << "setLineoutGain:" << v;
    sendCmd(QString("mixer lineout gain %1").arg(v));
}

void RadioModel::setLineoutMute(bool m)
{
    qCDebug(lcAudio) << "setLineoutMute:" << m;
    sendCmd(QString("mixer lineout mute %1").arg(m ? 1 : 0));
}

void RadioModel::setHeadphoneGain(int v)
{
    v = std::clamp(v, 0, 100);
    qCDebug(lcAudio) << "setHeadphoneGain:" << v;
    sendCmd(QString("mixer headphone gain %1").arg(v));
}

void RadioModel::setHeadphoneMute(bool m)
{
    qCDebug(lcAudio) << "setHeadphoneMute:" << m;
    sendCmd(QString("mixer headphone mute %1").arg(m ? 1 : 0));
}

void RadioModel::setFrontSpeakerMute(bool m)
{
    qCDebug(lcAudio) << "setFrontSpeakerMute:" << m;
    sendCmd(QString("mixer front_speaker mute %1").arg(m ? 1 : 0));
}

void RadioModel::handleSliceStatus(int id,
                                    const QMap<QString, QString>& kvs,
                                    bool removed)
{
    // Track slice ownership via client_handle (only present in some messages)
    if (kvs.contains("client_handle")) {
        quint32 owner = kvs["client_handle"].toUInt(nullptr, 16);
        if (owner == clientHandle()) {
            m_ownedSliceIds.insert(id);
            qCDebug(lcProtocol) << "RadioModel: slice" << id << "is ours (client_handle match)";
        } else if (owner != 0) {
            qCDebug(lcProtocol) << "RadioModel: slice" << id << "belongs to another client"
                     << Qt::hex << owner << ", removing";
            m_ownedSliceIds.remove(id);
            // If we already have a SliceModel for this ID, remove it
            if (SliceModel* existing = slice(id)) {
                const bool wasTxSlice = existing->isTxSlice();
                m_slices.removeOne(existing);
                emit sliceRemoved(id);
                existing->deleteLater();
                if (wasTxSlice)
                    m_meterModel.setActiveTxSlice(activeTxSliceNum());
            }
            return;  // slice belongs to another client
        }
    }

    // If we've seen client_handle info and this slice isn't ours, skip it
    if (!m_ownedSliceIds.isEmpty() && !m_ownedSliceIds.contains(id)) {
        qCDebug(lcProtocol) << "RadioModel: ignoring slice" << id << "status (not in owned set)";
        return;
    }

    SliceModel* s = slice(id);

    if (removed) {
        if (s) {
            const bool wasTxSlice = s->isTxSlice();
            m_slices.removeOne(s);
            emit sliceRemoved(id);
            s->deleteLater();
            if (wasTxSlice)
                m_meterModel.setActiveTxSlice(activeTxSliceNum());
        }
        return;
    }

    if (!s) {
        // Only create SliceModel from a full status (has in_use=1 and RF_frequency).
        // Partial statuses (e.g. "slice 3 rit_on=0") arrive early without enough
        // data to initialize the VFO widget correctly.
        if (!kvs.contains("in_use") || kvs["in_use"] != "1")
            return;

        s = new SliceModel(id, this);
        s->setRttyMarkDefault(m_rttyMarkDefault);
        // Forward slice commands to the radio
        connect(s, &SliceModel::commandReady, this, [this](const QString& cmd){
            sendCmd(cmd);
        });
        connect(s, &SliceModel::txSliceChanged, this, [this](bool) {
            m_meterModel.setActiveTxSlice(activeTxSliceNum());
        });
        m_slices.append(s);
        s->applyStatus(kvs);  // populate frequency/mode before notifying UI
        m_meterModel.setActiveTxSlice(activeTxSliceNum());
        emit sliceAdded(s);
        return;                // applyStatus already called below; skip second call
    }

    s->applyStatus(kvs);
    m_meterModel.setActiveTxSlice(activeTxSliceNum());

    // Aurora/AU-520: max_internal_pa_power in slice status reports the true
    // system power capability (e.g. 500W) while transmit status max_power_level
    // only reports the exciter limit (100W). Use the higher value. (#484)
    if (kvs.contains("max_internal_pa_power")) {
        int internalMax = kvs["max_internal_pa_power"].toInt();
        if (internalMax > m_transmitModel.maxPowerLevel()) {
            m_transmitModel.setMaxPowerLevel(internalMax);
        }
    }

    // Send any queued commands (e.g. if GUI changed freq before status arrived)
    if (isConnected()) {
        for (const QString& cmd : s->drainPendingCommands())
            sendCmd(cmd);
    }
}

void RadioModel::handleMeterStatus(const QString& rawBody)
{
    // Meter status body format (from FlexLib Radio.cs ParseMeterStatus):
    //   Tokens separated by '#', each token is "index.key=value".
    //   Example: "7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0"
    //
    // Removal format: "7 removed"

    if (rawBody.contains("removed")) {
        const QStringList words = rawBody.split(' ', Qt::SkipEmptyParts);
        if (words.size() >= 1) {
            bool ok = false;
            const int idx = words[0].toInt(&ok);
            if (ok) m_meterModel.removeMeter(idx);
        }
        return;
    }

    // Group tokens by meter index
    QMap<int, QMap<QString, QString>> grouped;
    const QStringList tokens = rawBody.split('#', Qt::SkipEmptyParts);

    for (const QString& token : tokens) {
        const int dot = token.indexOf('.');
        if (dot < 0) continue;
        const int eq = token.indexOf('=', dot);
        if (eq < 0) continue;

        bool ok = false;
        const int idx = token.left(dot).toInt(&ok);
        if (!ok) continue;

        const QString key   = token.mid(dot + 1, eq - dot - 1);
        const QString value = token.mid(eq + 1);
        grouped[idx][key] = value;
    }

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        const auto& fields = it.value();

        MeterDef def;
        def.index = it.key();
        if (fields.contains("src"))  def.source      = fields["src"];
        if (fields.contains("num"))  def.sourceIndex  = fields["num"].toInt();
        if (fields.contains("nam"))  def.name         = fields["nam"];
        if (fields.contains("unit")) def.unit         = fields["unit"];
        if (fields.contains("low"))  def.low          = fields["low"].toDouble();
        if (fields.contains("hi"))   def.high         = fields["hi"].toDouble();
        if (fields.contains("desc")) def.description  = fields["desc"];

        m_meterModel.defineMeter(def);
    }
}

void RadioModel::handleGpsStatus(const QString& rawBody)
{
    // GPS status uses '#' as separator, same as meter status.
    // Example: "lat=48.27#lon=-116.56#grid=DN18rg#altitude=644 m#tracked=16#
    //           visible=31#speed=0 kts#freq_error=0 ppb#status=Fine Lock#time=05:25:20Z"
    const QStringList tokens = rawBody.split('#', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const int eq = token.indexOf('=');
        if (eq < 1) continue;
        const QString key   = token.left(eq).toLower();
        const QString value = token.mid(eq + 1);

        if      (key == "status")   m_gpsStatus   = value;
        else if (key == "tracked")  m_gpsTracked  = value.toInt();
        else if (key == "visible")  m_gpsVisible  = value.toInt();
        else if (key == "grid")     m_gpsGrid     = value;
        else if (key == "altitude") m_gpsAltitude = value;
        else if (key == "lat")      m_gpsLat      = value;
        else if (key == "lon")      m_gpsLon      = value;
        else if (key == "time")       m_gpsTime     = value;
        else if (key == "speed")      m_gpsSpeed    = value;
        else if (key == "freq_error") m_gpsFreqError = value;
    }

    emit gpsStatusChanged(m_gpsStatus, m_gpsTracked, m_gpsVisible,
                           m_gpsGrid, m_gpsAltitude, m_gpsLat, m_gpsLon,
                           m_gpsTime);
}

void RadioModel::handlePanadapterStatus(const QString& panId, const QMap<QString, QString>& kvs)
{
    // Delegate to the specific PanadapterModel, not just the active one
    auto* pan = m_panadapters.value(panId, nullptr);
    if (!pan) pan = activePanadapter();  // fallback
    const float previousMinDbm = pan ? pan->minDbm() : 0.0f;
    const float previousMaxDbm = pan ? pan->maxDbm() : 0.0f;
    if (pan) {
        pan->applyPanStatus(kvs);
    }

    // Keep legacy signals for backward compat (MainWindow still uses these)
    if (kvs.contains("center") || kvs.contains("bandwidth")) {
        if (pan) emit panadapterInfoChanged(pan->centerMhz(), pan->bandwidthMhz());
    }
    if (kvs.contains("min_dbm") || kvs.contains("max_dbm")) {
        const float minDbm = pan ? pan->minDbm() : kvs.value("min_dbm", "-130").toFloat();
        const float maxDbm = pan ? pan->maxDbm() : kvs.value("max_dbm", "-20").toFloat();
        if (pan) {
            const bool levelChanged = (pan->minDbm() != previousMinDbm)
                || (pan->maxDbm() != previousMaxDbm);
            if (levelChanged) {
                m_panStream->setDbmRange(pan->panStreamId(), minDbm, maxDbm);
            }
        }
        emit panadapterLevelChanged(minDbm, maxDbm);
    }
    // Track ypixels from radio status — the radio encodes FFT bins as pixel
    // Y positions (0..ypixels-1), so PanadapterStream needs this for dBm conversion.
    // Also detect when the radio resets to default dimensions (e.g. after profile
    // load) and re-request correct dimensions from MainWindow.
    if (kvs.contains("y_pixels") && pan) {
        int yPix = kvs["y_pixels"].toInt();
        if (yPix > 0)
            m_panStream->setYPixels(pan->panStreamId(), yPix);
    }
    if ((kvs.contains("x_pixels") || kvs.contains("y_pixels")) && pan) {
        int xPix = kvs.value("x_pixels", "0").toInt();
        int yPix = kvs.value("y_pixels", "0").toInt();
        // Radio reset to defaults (profile load, reconnect) — re-push real dimensions
        if ((xPix > 0 && xPix <= 100) || (yPix > 0 && yPix <= 100))
            emit panDimensionsNeeded(pan->panId());
    }
    if (kvs.contains("ant_list")) {
        const QStringList ants = kvs["ant_list"].split(',', Qt::SkipEmptyParts);
        if (ants != m_antList) {
            m_antList = ants;
            emit antListChanged(m_antList);
        }
    }

    // Configure the panadapter once we know its ID.
    if (pan && !pan->isResized() && isConnected()) {
        pan->setResized(true);
        configurePan();
    }
}

void RadioModel::updateStreamFilters()
{
    // Register all known pan/wf stream IDs with PanadapterStream
    for (auto* pan : m_panadapters) {
        if (pan->panStreamId())
            m_panStream->registerPanStream(pan->panStreamId());
        if (pan->wfStreamId())
            m_panStream->registerWfStream(pan->wfStreamId());
    }
}

void RadioModel::configurePan()
{
    if (m_activePanId.isEmpty()) return;

    // Request MainWindow to push actual widget dimensions for this pan.
    // Do NOT hardcode xpixels/ypixels here — MainWindow knows the real sizes.
    emit panDimensionsNeeded(m_activePanId);

    sendCmd(
        QString("display pan set %1 min_dbm=-130 max_dbm=-40").arg(m_activePanId),
        [](int code, const QString&) {
            if (code != 0)
                qCWarning(lcProtocol) << "RadioModel: display pan set dbm failed, code" << Qt::hex << code;
        });
}

void RadioModel::configureWaterfall()
{
    if (activeWfId().isEmpty()) return;

    // Disable automatic black-level and set a fixed threshold.
    // FlexLib uses "display panafall set" addressed to the waterfall stream ID.
    const QString cmd = QString("display panafall set %1 auto_black=0 black_level=15 color_gain=50")
                            .arg(activeWfId());
    sendCmd(cmd, [this](int code, const QString&) {
        if (code != 0) {
            qCDebug(lcProtocol) << "RadioModel: display panafall set waterfall failed, code"
                     << Qt::hex << code << "— trying display waterfall set";
            // Fallback for firmware that doesn't support panafall addressing
            sendCmd(
                QString("display waterfall set %1 auto_black=0 black_level=15 color_gain=50")
                    .arg(activeWfId()),
                [](int code2, const QString&) {
                    if (code2 != 0)
                        qCWarning(lcProtocol) << "RadioModel: display waterfall set also failed, code"
                                   << Qt::hex << code2;
                    else
                        qCDebug(lcProtocol) << "RadioModel: waterfall configured via display waterfall set";
                });
        } else {
            qCDebug(lcProtocol) << "RadioModel: waterfall configured (auto_black=0 black_level=15 color_gain=50)";
        }
    });
}

// Standalone mode: create panadapter + slice.
// FlexLib v4.2.18 uses "display panafall create x=100 y=100"; keep the
// legacy "panadapter create" as a fallback for older firmware.

void RadioModel::createDefaultSlice(const QString& freqMhz,
                                     const QString& mode,
                                     const QString& antenna)
{
    qCDebug(lcProtocol) << "RadioModel: standalone mode — creating panadapter + slice"
             << freqMhz << mode << antenna;

    const auto handleCreatedPan =
        [this, freqMhz, mode, antenna](const QString& source,
                                       int code,
                                       const QString& body) -> bool {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel:" << source << "failed, code"
                                  << Qt::hex << code << "body:" << body;
            emit panadapterLimitReached(maxPanadapters(), m_model);
            return false;
        }

        qCDebug(lcProtocol) << "RadioModel:" << source << "response body:" << body;
        const QString panId = parsePanadapterCreateId(body);
        if (panId.isEmpty()) {
            qCWarning(lcProtocol) << "RadioModel:" << source
                                  << "returned empty pan_id";
            return false;
        }

        qCDebug(lcProtocol) << "RadioModel: panadapter created, pan_id =" << panId;
        createDefaultSliceOnPan(panId, freqMhz, mode, antenna);
        return true;
    };

    sendCmd("display panafall create x=100 y=100",
        [this, handleCreatedPan](int code, const QString& body) {
            if (code == 0) {
                handleCreatedPan(QStringLiteral("display panafall create"), code, body);
                return;
            }

            qCWarning(lcProtocol) << "RadioModel: display panafall create failed, code"
                                  << Qt::hex << code << "body:" << body
                                  << "- trying legacy panadapter create";
            sendCmd("panadapter create",
                [handleCreatedPan](int legacyCode, const QString& legacyBody) {
                    handleCreatedPan(QStringLiteral("panadapter create"),
                                     legacyCode,
                                     legacyBody);
                });
        });
}

void RadioModel::createDefaultSliceOnPan(const QString& panId,
                                         const QString& freqMhz,
                                         const QString& mode,
                                         const QString& antenna)
{
    auto* pan = ensureOwnedPanadapter(panId);
    if (!pan) {
        qCWarning(lcProtocol) << "RadioModel: cannot create slice without panadapter id";
        return;
    }

    const QString sliceCmd =
        QString("slice create pan=%1 freq=%2 antenna=%3 mode=%4")
            .arg(pan->panId(), freqMhz, antenna, mode);

    sendCmd(sliceCmd,
        [this, panId = pan->panId()](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcProtocol) << "RadioModel: slice create failed for pan"
                                      << panId << "code" << Qt::hex << code
                                      << "body:" << body;
                emit sliceCreateFailed(maxSlices(), m_model);
            } else {
                qCDebug(lcProtocol) << "RadioModel: slice created, index =" << body;
                // Radio now emits S|slice N ... status messages;
                // handleSliceStatus() picks them up automatically.
            }
        });
}

void RadioModel::handleProfileStatus(const QString& object,
                                      const QMap<QString, QString>& kvs)
{
    // Profile list/current with space-containing names are handled by
    // handleProfileStatusRaw() via onMessageReceived().  This fallback
    // handles any remaining profile status keys that don't have spaces
    // (e.g. "profile all unsaved_changes_tx=0").
    Q_UNUSED(object);
    Q_UNUSED(kvs);
}

void RadioModel::handleProfileStatusRaw(const QString& profileType,
                                         const QString& rawBody)
{
    // rawBody is everything after "profile tx " or "profile mic ", e.g.:
    //   "list=Default^Default FHM-1^Default FHM-1 DX^..."
    //   "current=Default FHM-1"
    // We parse key=value ourselves to avoid splitting on spaces in values.
    const int eq = rawBody.indexOf('=');
    if (eq < 0) return;

    const QString key = rawBody.left(eq).trimmed();
    const QString val = rawBody.mid(eq + 1).trimmed();

    if (profileType == "tx") {
        if (key == "list") {
            QStringList profiles = val.split('^', Qt::SkipEmptyParts);
            m_transmitModel.setProfileList(profiles);
            qCDebug(lcProtocol) << "RadioModel: TX profiles:" << profiles;
        } else if (key == "current") {
            m_transmitModel.setActiveProfile(val);
            qCDebug(lcProtocol) << "RadioModel: active TX profile:" << val;
        }
    } else if (profileType == "mic") {
        if (key == "list") {
            QStringList profiles = val.split('^', Qt::SkipEmptyParts);
            m_transmitModel.setMicProfileList(profiles);
            qCDebug(lcProtocol) << "RadioModel: mic profiles:" << profiles;
        } else if (key == "current") {
            m_transmitModel.setActiveMicProfile(val);
            qCDebug(lcProtocol) << "RadioModel: active mic profile:" << val;
        }
    } else if (profileType == "global") {
        if (key == "list") {
            m_globalProfiles = val.split('^', Qt::SkipEmptyParts);
            qCDebug(lcProtocol) << "RadioModel: global profiles:" << m_globalProfiles;
            emit globalProfilesChanged();
        } else if (key == "current") {
            m_activeGlobalProfile = val;
            qCDebug(lcProtocol) << "RadioModel: active global profile:" << val;
            emit globalProfilesChanged();
        }
    }
}

void RadioModel::loadGlobalProfile(const QString& name)
{
    sendCmd(QString("profile global load \"%1\"").arg(name));
}

void RadioModel::resetPanState()
{
    setActivePanResized(false);
    setActiveWfConfigured(false);
}

void RadioModel::createAudioStream()
{
    // Remove old audio stream first, then create new one in the callback
    if (m_rxAudio.streamId != 0) {
        const quint32 oldId = m_rxAudio.streamId;
        m_rxAudio = {};
        sendCmd(
            QString("stream remove %1").arg(RadioStatusOwnership::hexId(oldId)),
            [this](int, const QString&) {
                // Old stream removed — now create the new one
                createRxAudioStream();
            });
    } else {
        createRxAudioStream();
    }
}

bool RadioModel::ensureDaxTxStream(DaxTxRequestReason reason)
{
    const DaxTxPolicyContext policyContext = currentDaxTxPolicyContext(reason);
    const DaxTxPolicyDecision decision = evaluateDaxTxPolicy(policyContext);
    const quint32 ourHandle = clientHandle();
    qCInfo(lcDax).noquote()
        << "RadioModel: DAX TX policy"
        << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason))
        << QStringLiteral("platform=%1").arg(daxTxPlatformName(policyContext.platform))
        << QStringLiteral("allowed=%1").arg(decision.allowed ? 1 : 0)
        << QStringLiteral("mode=%1").arg(daxTxModeName(policyContext.mode))
        << QStringLiteral("stream=%1").arg(m_daxTxStreamId != 0
            ? hexId(m_daxTxStreamId)
            : QStringLiteral("none"))
        << QStringLiteral("owner=%1").arg(m_daxTxClientHandle != 0
            ? hexId(m_daxTxClientHandle)
            : QStringLiteral("unknown"));

    if (!decision.allowed) {
        qCInfo(lcDax).noquote()
            << "RadioModel: DAX TX stream not created"
            << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason))
            << QStringLiteral("mode=%1").arg(daxTxModeName(policyContext.mode))
            << QStringLiteral("note=%1").arg(decision.note);
        return false;
    }

    const bool existingStreamIsOurs = m_daxTxStreamId != 0
        && (m_daxTxClientHandle == 0 || m_daxTxClientHandle == ourHandle);
    if (existingStreamIsOurs || m_daxTxCreatePending)
        return true;

    m_daxTxCreatePending = true;
    qCInfo(lcDax).noquote()
        << "RadioModel: DAX TX create requested"
        << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
    sendCmd(
        "stream create type=dax_tx",
        [this, reason](int code, const QString& body) {
            m_daxTxCreatePending = false;
            if (code != 0) {
                qCWarning(lcDax).noquote()
                    << "RadioModel: DAX TX create failed"
                    << QStringLiteral("code=%1").arg(hexCode(code))
                    << QStringLiteral("body=%1").arg(body)
                    << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
                return;
            }

            const quint32 id = RadioStatusOwnership::parseCreateResponseStreamId(body);
            if (id == 0) {
                qCWarning(lcDax).noquote()
                    << "RadioModel: DAX TX create failed"
                    << QStringLiteral("code=0x00000000")
                    << QStringLiteral("body=%1").arg(body)
                    << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
                return;
            }

            m_daxTxStreamId = id;
            m_daxTxActive = false;
            m_daxTxClientHandle = clientHandle();
            qCInfo(lcDax).noquote()
                << "RadioModel: DAX TX create succeeded"
                << QStringLiteral("stream=%1").arg(hexId(id));
            emit txAudioStreamReady(id);
        });
    return true;
}

QJsonObject RadioModel::troubleshootingSnapshot() const
{
    QJsonObject snapshot;
    snapshot["schema_version"] = 1;
    snapshot["captured_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    snapshot["captured_from"] = "AetherSDR in-memory application state";
    snapshot["note"] =
        "This snapshot is built from the app's cached radio, panadapter, slice, "
        "and meter models. It does not query the radio directly.";
    snapshot["privacy"] =
        "Sensitive identifiers are omitted by design, including radio name, "
        "nickname, callsign, serial numbers, MAC/IP addresses, GPS data, and "
        "client station names.";

    QJsonObject app;
    app["name"] = QCoreApplication::applicationName();
    app["version"] = QCoreApplication::applicationVersion();
    app["qt_version"] = qVersion();
    app["os"] = QSysInfo::prettyProductName();
    app["cpu_arch"] = QSysInfo::currentCpuArchitecture();
    snapshot["app"] = app;

    QJsonObject radio;
    radio["connected"] = isConnected();
    radio["transport"] = isWan() ? "WAN" : "LAN";
    radio["model"] = m_model;
    radio["software_version"] = m_version;
    radio["protocol_version"] = m_protocolVersion;
    radio["region"] = m_region;
    radio["radio_options"] = m_radioOptions;
    radio["max_slices"] = m_maxSlices;
    radio["full_duplex_enabled"] = m_fullDuplex;
    radio["binaural_rx"] = m_binauralRx;
    radio["mute_local_audio_when_remote"] = m_muteLocalWhenRemote;
    radio["low_latency_digital_modes"] = m_lowLatencyDigital;
    radio["enforce_private_ip_connections"] = m_enforcePrivateIp;
    radio["remote_on_enabled"] = m_remoteOnEnabled;
    radio["mf_enable"] = m_multiFlexEnabled;
    radio["rtty_mark_default"] = m_rttyMarkDefault;
    radio["antenna_list"] = toJsonArray(m_antList);
    radio["owned_slice_ids"] = toJsonArray(m_ownedSliceIds);
    radio["global_profile_count"] = m_globalProfiles.size();
    radio["active_global_profile_set"] = !m_activeGlobalProfile.trimmed().isEmpty();

    QJsonObject oscillator;
    oscillator["setting"] = m_oscSetting;
    oscillator["locked"] = m_oscLocked;
    oscillator["ext_present"] = m_extPresent;
    oscillator["tcxo_present"] = m_tcxoPresent;
    radio["oscillator"] = oscillator;

    QJsonObject audioOutputs;
    audioOutputs["lineout_gain"] = m_lineoutGain;
    audioOutputs["lineout_mute"] = m_lineoutMute;
    audioOutputs["headphone_gain"] = m_headphoneGain;
    audioOutputs["headphone_mute"] = m_headphoneMute;
    audioOutputs["front_speaker_mute"] = m_frontSpeakerMute;
    radio["audio_outputs"] = audioOutputs;

    const bool pcAudioSetting = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    QJsonObject remoteAudioRx;
    remoteAudioRx["stream_id"] = m_rxAudio.streamId == 0
        ? QJsonValue()
        : QJsonValue(RadioStatusOwnership::hexId(m_rxAudio.streamId));
    remoteAudioRx["stream_id_known"] = m_rxAudio.streamId != 0;
    remoteAudioRx["create_pending"] = m_rxAudio.createPending;
    remoteAudioRx["remove_requested"] = m_rxAudio.removeRequested;
    remoteAudioRx["status_seen"] = m_rxAudio.statusSeen;
    remoteAudioRx["owner_known"] = m_rxAudio.clientHandle != 0;
    remoteAudioRx["owned_by_us"] = m_rxAudio.clientHandle != 0 && m_rxAudio.clientHandle == ourClientHandle();
    remoteAudioRx["compression"] = m_rxAudio.compression;
    remoteAudioRx["pc_audio_setting"] = pcAudioSetting;
    remoteAudioRx["stream_expected"] = pcAudioSetting;
    remoteAudioRx["routing_note"] = pcAudioSetting
        ? QStringLiteral("PC Audio is enabled; an owned remote_audio_rx stream should exist and the local RX sink should be running.")
        : QStringLiteral("PC Audio is disabled; no remote_audio_rx stream is expected. TCI clients route audio via DAX, not via remote_audio_rx (#1137).");
    radio["remote_audio_rx"] = remoteAudioRx;

    QJsonObject filterSharpness;
    filterSharpness["voice_level"] = m_filterVoice;
    filterSharpness["voice_auto"] = m_filterVoiceAuto;
    filterSharpness["cw_level"] = m_filterCw;
    filterSharpness["cw_auto"] = m_filterCwAuto;
    filterSharpness["digital_level"] = m_filterDigital;
    filterSharpness["digital_auto"] = m_filterDigitalAuto;
    radio["filter_sharpness"] = filterSharpness;

    QJsonObject amplifier;
    amplifier["present"] = m_hasAmplifier;
    amplifier["handle"] = m_ampHandle;
    amplifier["model"] = m_ampModel;
    amplifier["operate"] = m_ampOperate;
    radio["amplifier"] = amplifier;

    QJsonObject ownership;
    ownership["tx_owned_by_us"] = m_txOwnedByUs;
    QJsonArray clients;
    QSet<quint32> seenHandles;
    const quint32 ourHandle = ourClientHandle();
    for (auto it = m_clientStations.cbegin(); it != m_clientStations.cend(); ++it) {
        clients.append(clientInfoToJson(it.key(), ourHandle, m_txClientHandle,
                                        m_clientInfoMap.value(it.key())));
        seenHandles.insert(it.key());
    }
    for (auto it = m_clientInfoMap.cbegin(); it != m_clientInfoMap.cend(); ++it) {
        if (seenHandles.contains(it.key()))
            continue;
        clients.append(clientInfoToJson(it.key(), ourHandle, m_txClientHandle,
                                        it.value()));
    }
    ownership["clients"] = clients;
    ownership["client_count"] = clients.size();
    ownership["multiple_clients_present"] = clients.size() > 1;
    radio["ownership"] = ownership;

    auto categoryStatsToJson = [this](PanadapterStream::StreamCategory cat) {
        const auto stats = categoryStats(cat);
        QJsonObject obj;
        obj["bytes"] = static_cast<qint64>(stats.bytes);
        obj["packets"] = stats.packets;
        obj["errors"] = stats.errors;
        return obj;
    };

    QJsonObject network;
    network["quality"] = networkQuality();
    network["last_ping_rtt_ms"] = m_lastPingRtt;
    network["max_ping_rtt_ms"] = m_maxPingRtt;
    network["packet_drop_count"] = packetDropCount();
    network["packet_total_count"] = packetTotalCount();
    network["packet_loss_window_seconds"] = packetLossWindowSeconds();
    network["packet_loss_window_drops"] = packetLossWindowDrops();
    network["packet_loss_window_packets"] = packetLossWindowPackets();
    network["packet_loss_window_percent"] = packetLossPercent();
    network["audio_packet_gap_ms"] = audioPacketGapMs();
    network["audio_packet_gap_max_ms"] = audioPacketGapMaxMs();
    network["audio_packet_jitter_ms"] = audioPacketJitterMs();
    network["rx_bytes"] = static_cast<qint64>(rxBytes());
    network["tx_bytes"] = static_cast<qint64>(txBytes());
    QJsonObject streamCategories;
    streamCategories["audio"] = categoryStatsToJson(PanadapterStream::CatAudio);
    streamCategories["fft"] = categoryStatsToJson(PanadapterStream::CatFFT);
    streamCategories["waterfall"] = categoryStatsToJson(PanadapterStream::CatWaterfall);
    streamCategories["meter"] = categoryStatsToJson(PanadapterStream::CatMeter);
    streamCategories["dax"] = categoryStatsToJson(PanadapterStream::CatDAX);
    network["stream_categories"] = streamCategories;
    radio["network"] = network;

    QJsonObject telemetry;
    telemetry["pa_temp_c"] = m_meterModel.paTemp();
    telemetry["supply_volts"] = m_meterModel.supplyVolts();
    telemetry["tx_forward_power_w"] = m_meterModel.fwdPower();
    telemetry["tx_swr"] = m_meterModel.swr();
    // SliceTroubleshootingDialog renders this with the literal label
    // "HWALC", so keep it pointed at the external Hardware ALC RCA voltage
    // (m_hwAlc) — the gauge in the Phone/CW applet now uses swAlc().
    telemetry["alc"] = m_meterModel.hwAlc();
    telemetry["mic_level_dbfs"] = m_meterModel.micLevel();
    telemetry["mic_peak_dbfs"] = m_meterModel.micPeak();
    telemetry["comp_level_db"] = m_meterModel.compLevel();
    telemetry["comp_peak_db"] = m_meterModel.compPeak();
    radio["telemetry"] = telemetry;

    snapshot["radio"] = radio;

    QJsonObject transmit;

    QJsonObject txPower;
    txPower["rf_power"] = m_transmitModel.rfPower();
    txPower["tune_power"] = m_transmitModel.tunePower();
    txPower["max_power_level"] = m_transmitModel.maxPowerLevel();
    txPower["tune_mode"] = m_transmitModel.tuneMode();
    txPower["show_tx_in_waterfall"] = m_transmitModel.showTxInWaterfall();
    txPower["tuning"] = m_transmitModel.isTuning();
    txPower["mox"] = m_transmitModel.isMox();
    txPower["transmitting"] = m_transmitModel.isTransmitting();
    transmit["power"] = txPower;

    QJsonObject mic;
    mic["selection"] = m_transmitModel.micSelection();
    mic["level"] = m_transmitModel.micLevel();
    mic["mic_acc"] = m_transmitModel.micAcc();
    mic["speech_processor_enable"] = m_transmitModel.speechProcessorEnable();
    mic["speech_processor_level"] = m_transmitModel.speechProcessorLevel();
    mic["compander_on"] = m_transmitModel.companderOn();
    mic["compander_level"] = m_transmitModel.companderLevel();
    mic["dax_on"] = m_transmitModel.daxOn();
    mic["sb_monitor"] = m_transmitModel.sbMonitor();
    mic["mon_gain_sb"] = m_transmitModel.monGainSb();
    mic["mic_boost"] = m_transmitModel.micBoost();
    mic["mic_bias"] = m_transmitModel.micBias();
    mic["met_in_rx"] = m_transmitModel.metInRx();
    mic["sync_cwx"] = m_transmitModel.syncCwx();
    mic["am_carrier_level"] = m_transmitModel.amCarrierLevel();
    mic["dexp_on"] = m_transmitModel.dexpOn();
    mic["dexp_level"] = m_transmitModel.dexpLevel();
    mic["tx_filter_low"] = m_transmitModel.txFilterLow();
    mic["tx_filter_high"] = m_transmitModel.txFilterHigh();
    transmit["mic"] = mic;

    QJsonObject vox;
    vox["enabled"] = m_transmitModel.voxEnable();
    vox["level"] = m_transmitModel.voxLevel();
    vox["delay"] = m_transmitModel.voxDelay();
    transmit["vox"] = vox;

    QJsonObject cw;
    cw["speed_wpm"] = m_transmitModel.cwSpeed();
    cw["pitch_hz"] = m_transmitModel.cwPitch();
    cw["break_in"] = m_transmitModel.cwBreakIn();
    cw["delay_ms"] = m_transmitModel.cwDelay();
    cw["sidetone"] = m_transmitModel.cwSidetone();
    cw["iambic"] = m_transmitModel.cwIambic();
    cw["iambic_mode"] = m_transmitModel.cwIambicMode();
    cw["swap_paddles"] = m_transmitModel.cwSwapPaddles();
    cw["cwl_enabled"] = m_transmitModel.cwlEnabled();
    cw["monitor_gain"] = m_transmitModel.monGainCw();
    transmit["cw"] = cw;

    QJsonObject interlock;
    interlock["acc_tx_delay"] = m_transmitModel.accTxDelay();
    interlock["tx1_delay"] = m_transmitModel.tx1Delay();
    interlock["tx2_delay"] = m_transmitModel.tx2Delay();
    interlock["tx3_delay"] = m_transmitModel.tx3Delay();
    interlock["tx_delay"] = m_transmitModel.txDelay();
    interlock["timeout"] = m_transmitModel.interlockTimeout();
    interlock["acc_tx_req_polarity"] = m_transmitModel.accTxReqPolarity();
    interlock["rca_tx_req_polarity"] = m_transmitModel.rcaTxReqPolarity();
    transmit["interlock"] = interlock;

    QJsonObject atu;
    atu["enabled"] = m_transmitModel.atuEnabled();
    atu["status"] = atuStatusToString(m_transmitModel.atuStatus());
    atu["memories_enabled"] = m_transmitModel.memoriesEnabled();
    atu["using_memory"] = m_transmitModel.usingMemory();
    transmit["atu"] = atu;

    QJsonObject apd;
    apd["enabled"] = m_transmitModel.apdEnabled();
    apd["configurable"] = m_transmitModel.apdConfigurable();
    apd["equalizer_active"] = m_transmitModel.apdEqualizerActive();
    transmit["apd"] = apd;

    QJsonObject profiles;
    profiles["tx_profile_count"] = m_transmitModel.profileList().size();
    profiles["active_tx_profile_set"] = !m_transmitModel.activeProfile().trimmed().isEmpty();
    profiles["mic_profile_count"] = m_transmitModel.micProfileList().size();
    profiles["active_mic_profile_set"] = !m_transmitModel.activeMicProfile().trimmed().isEmpty();
    profiles["mic_inputs"] = toJsonArray(m_transmitModel.micInputList());
    transmit["profiles"] = profiles;

    snapshot["transmit"] = transmit;

    QJsonArray panadapters;
    for (auto it = m_panadapters.cbegin(); it != m_panadapters.cend(); ++it)
        panadapters.append(panToJson(it.value(), m_activePanId));
    snapshot["panadapters"] = panadapters;

    QJsonArray xvtrs;
    for (auto it = m_xvtrList.cbegin(); it != m_xvtrList.cend(); ++it)
        xvtrs.append(xvtrToJson(it.value()));
    snapshot["xvtrs"] = xvtrs;

    auto txBandInfoToJson = [](const TxBandInfo& band) {
        QJsonObject obj;
        obj["band_id"] = band.bandId;
        obj["band_name"] = band.bandName;
        obj["rf_power"] = band.rfPower;
        obj["tune_power"] = band.tunePower;
        obj["inhibit"] = band.inhibit;
        obj["hw_alc"] = band.hwAlc;
        obj["acc_tx_req"] = band.accTxReq;
        obj["rca_tx_req"] = band.rcaTxReq;
        obj["acc_tx"] = band.accTx;
        obj["tx1"] = band.tx1;
        obj["tx2"] = band.tx2;
        obj["tx3"] = band.tx3;
        return obj;
    };

    QJsonArray txBands;
    for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it)
        txBands.append(txBandInfoToJson(it.value()));
    snapshot["tx_band_settings"] = txBands;

    QJsonArray allMeters = m_meterModel.allMeters();
    QJsonArray globalMeters;
    int sliceMeterCount = 0;
    for (const QJsonValue& value : allMeters) {
        const QJsonObject meter = value.toObject();
        if (meter["source"].toString() == "SLC")
            ++sliceMeterCount;
        else
            globalMeters.append(meter);
    }
    snapshot["global_meters"] = globalMeters;

    QList<SliceModel*> sortedSlices = m_slices;
    std::sort(sortedSlices.begin(), sortedSlices.end(), [](SliceModel* lhs, SliceModel* rhs) {
        return lhs->sliceId() < rhs->sliceId();
    });

    QJsonArray slices;
    for (SliceModel* sliceModel : sortedSlices) {
        QJsonObject slice;
        slice["slice_id"] = sliceModel->sliceId();
        slice["pan_id"] = sliceModel->panId();
        slice["frequency_mhz"] = sliceModel->frequency();
        slice["mode"] = sliceModel->mode();
        slice["mode_list"] = toJsonArray(sliceModel->modeList());
        slice["active"] = sliceModel->isActive();
        slice["tx_slice"] = sliceModel->isTxSlice();

        QJsonObject filter;
        filter["low_hz"] = sliceModel->filterLow();
        filter["high_hz"] = sliceModel->filterHigh();
        slice["filter"] = filter;

        QJsonObject audio;
        audio["gain"] = sliceModel->audioGain();
        audio["pan"] = sliceModel->audioPan();
        audio["mute"] = sliceModel->audioMute();
        slice["audio"] = audio;

        slice["rf_gain"] = sliceModel->rfGain();

        QJsonObject antennas;
        antennas["rx"] = sliceModel->rxAntenna();
        antennas["tx"] = sliceModel->txAntenna();
        slice["antennas"] = antennas;

        QJsonObject control;
        control["locked"] = sliceModel->isLocked();
        control["qsk"] = sliceModel->qskOn();
        control["record_on"] = sliceModel->recordOn();
        control["play_on"] = sliceModel->playOn();
        control["play_enabled"] = sliceModel->playEnabled();
        slice["control"] = control;

        QJsonObject dsp;
        dsp["agc_mode"] = sliceModel->agcMode();
        dsp["agc_threshold"] = sliceModel->agcThreshold();
        dsp["nb"] = QJsonObject{{"enabled", sliceModel->nbOn()}, {"level", sliceModel->nbLevel()}};
        dsp["nr"] = QJsonObject{{"enabled", sliceModel->nrOn()}, {"level", sliceModel->nrLevel()}};
        dsp["anf"] = QJsonObject{{"enabled", sliceModel->anfOn()}, {"level", sliceModel->anfLevel()}};
        dsp["lms_nr"] = QJsonObject{{"enabled", sliceModel->nrlOn()}, {"level", sliceModel->nrlLevel()}};
        dsp["speex_nr"] = QJsonObject{{"enabled", sliceModel->nrsOn()}, {"level", sliceModel->nrsLevel()}};
        dsp["rnnoise"] = sliceModel->rnnOn();
        dsp["nrf"] = QJsonObject{{"enabled", sliceModel->nrfOn()}, {"level", sliceModel->nrfLevel()}};
        dsp["lms_anf"] = QJsonObject{{"enabled", sliceModel->anflOn()}, {"level", sliceModel->anflLevel()}};
        dsp["anft"] = sliceModel->anftOn();
        dsp["apf"] = QJsonObject{{"enabled", sliceModel->apfOn()}, {"level", sliceModel->apfLevel()}};
        slice["dsp"] = dsp;

        QJsonObject diversity;
        diversity["enabled"] = sliceModel->diversity();
        diversity["is_parent"] = sliceModel->isDiversityParent();
        diversity["is_child"] = sliceModel->isDiversityChild();
        diversity["index"] = sliceModel->diversityIndex();
        diversity["esc_enabled"] = sliceModel->escEnabled();
        diversity["esc_gain"] = sliceModel->escGain();
        diversity["esc_phase_shift_deg"] = sliceModel->escPhaseShift();
        slice["diversity"] = diversity;

        QJsonObject tuning;
        tuning["squelch_on"] = sliceModel->squelchOn();
        tuning["squelch_level"] = sliceModel->squelchLevel();
        tuning["rit_on"] = sliceModel->ritOn();
        tuning["rit_hz"] = sliceModel->ritFreq();
        tuning["xit_on"] = sliceModel->xitOn();
        tuning["xit_hz"] = sliceModel->xitFreq();
        tuning["step_hz"] = sliceModel->stepHz();
        tuning["step_list"] = toJsonArray(sliceModel->stepList());
        slice["tuning"] = tuning;

        QJsonObject digital;
        digital["dax_channel"] = sliceModel->daxChannel();
        digital["rtty_mark_hz"] = sliceModel->rttyMark();
        digital["rtty_shift_hz"] = sliceModel->rttyShift();
        digital["digl_offset_hz"] = sliceModel->diglOffset();
        digital["digu_offset_hz"] = sliceModel->diguOffset();
        slice["digital"] = digital;

        QJsonObject fm;
        fm["tone_mode"] = sliceModel->fmToneMode();
        fm["tone_value"] = sliceModel->fmToneValue();
        fm["repeater_offset_dir"] = sliceModel->repeaterOffsetDir();
        fm["repeater_offset_mhz"] = sliceModel->fmRepeaterOffsetFreq();
        fm["tx_offset_mhz"] = sliceModel->txOffsetFreq();
        fm["deviation_hz"] = sliceModel->fmDeviation();
        slice["fm"] = fm;

        if (PanadapterModel* pan = panadapter(sliceModel->panId()))
            slice["panadapter_state"] = panToJson(pan, m_activePanId);

        slice["meters"] = m_meterModel.metersForSource("SLC", sliceModel->sliceId());
        slices.append(slice);
    }
    snapshot["slices"] = slices;

    QJsonObject counts;
    counts["panadapters"] = panadapters.size();
    counts["slices"] = slices.size();
    counts["meters_total"] = allMeters.size();
    counts["global_meters"] = globalMeters.size();
    counts["slice_meters"] = sliceMeterCount;
    snapshot["counts"] = counts;

    return snapshot;
}

} // namespace AetherSDR
