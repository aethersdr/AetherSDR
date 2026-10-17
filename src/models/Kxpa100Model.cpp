#include "Kxpa100Model.h"
#include "core/Kxpa100Protocol.h"
#include "core/Kxpa100Maintenance.h"
#include <QTcpSocket>
#include <QIODevice>
#include <QLoggingCategory>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcKxpa100, "aether.kxpa100")
namespace AetherSDR {
using namespace Kxpa100;
namespace {
const QList<QByteArray> kPolls{"^OP;","^MD;","^AN;","^BY;","^AT;","^CR;","^LR;","^SI;","^SB;",
    "^PV;","^PD;","^PC;","^SV;","^TM;","^F;","^BN;","^AD;","^TU;","^I;","^RV;","^SN;"};
QByteArray hexMask(int mask) { return QByteArray::number(mask,16).rightJustified(2,'0').toUpper(); }
}
Kxpa100Model::Kxpa100Model(QObject* parent) : QObject(parent), m_parser(std::make_unique<FrameParser>()),
    m_socket(std::make_unique<QTcpSocket>())
{
    m_socket->setReadBufferSize(4096);
    m_clock.start(); m_now = [this] { return m_clock.elapsed(); };
    m_timer.setInterval(50); m_reconnectTimer.setSingleShot(true); m_reconnectTimer.setInterval(5000);
    connect(&m_timer,&QTimer::timeout,this,&Kxpa100Model::tick);
    connect(&m_reconnectTimer,&QTimer::timeout,this,&Kxpa100Model::reconnect);
    m_parser->setCallback([this](const QByteArray& frame) { receiveFrame(frame); });
    connect(m_socket.get(),&QTcpSocket::connected,this,&Kxpa100Model::transportOpened);
    connect(m_socket.get(),&QTcpSocket::readyRead,this,[this] { receiveBytes(m_socket->readAll()); });
    connect(m_socket.get(),&QTcpSocket::disconnected,this,[this] { transportClosed(); });
    connect(m_socket.get(),&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError) {
        transportClosed(m_socket->errorString());
    });
#ifdef HAVE_SERIALPORT
    m_serial = std::make_unique<QSerialPort>();
    connect(m_serial.get(),&QSerialPort::readyRead,this,[this] { receiveBytes(m_serial->readAll()); });
    connect(m_serial.get(),&QSerialPort::errorOccurred,this,[this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError || error == QSerialPort::PermissionError) {
            transportClosed(m_serial->errorString());
        }
    });
#endif
}
Kxpa100Model::~Kxpa100Model() { m_autoReconnect = false; disconnectDevice(); }
void Kxpa100Model::connectToHost(const QString& host, quint16 port)
{
    if (m_connected || m_connecting) { return; }
    m_reconnectTimer.stop();
    const QString trimmedHost = host.trimmed();
    bool invalidHost = trimmedHost.isEmpty() || trimmedHost.size() > 253 || trimmedHost.contains('/');
    for (QChar character : trimmedHost) {
        invalidHost = invalidHost || character.isSpace() || !character.isPrint();
    }
    if (invalidHost || port == 0) {
        emit connectionFailed(tr("Enter a TCP host and a port from 1 to 65535."));
        return;
    }
    m_network = true; m_host = trimmedHost; m_port = port; m_connecting = true;
    m_deliberateDisconnect = false; m_connectStarted = m_now(); m_timer.start();
    m_socket->connectToHost(m_host,m_port); emit availabilityChanged();
}
void Kxpa100Model::connectSerial(const QString& portName, int baudRate)
{
#ifdef HAVE_SERIALPORT
    if (m_connected || m_connecting || portName.trimmed().isEmpty() ||
        (baudRate != 4800 && baudRate != 9600 && baudRate != 19200 && baudRate != 38400)) { return; }
    m_network = false; m_portName = portName; m_baud = baudRate; m_deliberateDisconnect = false;
    m_serial->setPortName(portName); m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8); m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop); m_serial->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_serial->open(QIODevice::ReadWrite)) { transportClosed(m_serial->errorString()); return; }
    transportOpened();
#else
    Q_UNUSED(portName); Q_UNUSED(baudRate);
    emit connectionFailed(QStringLiteral("This build does not include serial-port support."));
#endif
}
void Kxpa100Model::disconnectDevice()
{
    m_deliberateDisconnect = true; m_reconnectTimer.stop();
    transportClosed();
}
void Kxpa100Model::reconnect()
{
    if (!m_autoReconnect || m_deliberateDisconnect || m_connected || m_connecting) { return; }
    if (m_network) { connectToHost(m_host,m_port); } else { connectSerial(m_portName,m_baud); }
}
void Kxpa100Model::transportOpened()
{
    ++m_generation; m_connected = true; m_connecting = false; m_detected = false;
    m_deferredTuneCancel = false; m_tuneEnded = false; m_status = {}; m_configuration.reset();
    m_received.clear(); m_queue.clear(); m_active.reset(); m_parser->reset();
    m_operation.clear(); m_settleGetter.clear(); m_settleVerify = {}; m_tunePermit = {};
    m_lastReply = -10000; m_nextPoll = 0; m_pollIndex = 0; m_timer.start(); queueIdentity();
    report(QStringLiteral("Transport connected: %1").arg(description())); emit connected(); emit availabilityChanged();
}
void Kxpa100Model::transportClosed(const QString& reason)
{
    if (m_closing) { return; }
    m_closing = true;
    const bool wasOpen = m_connected || m_connecting;
    m_connected = false; m_connecting = false; m_timer.stop(); ++m_generation;
    m_socket->abort();
#ifdef HAVE_SERIALPORT
    if (m_serial->isOpen()) { m_serial->close(); }
#endif
    if (operationBusy()) { fail(reason.isEmpty() ? QStringLiteral("Transport closed; pending operations discarded.") : reason); }
    m_queue.clear(); m_active.reset(); m_parser->reset(); m_received.clear(); m_configuration.reset();
    m_deferredTuneCancel = false;
    if (m_detected) { m_detected = false; emit detectedChanged(false); }
    if (wasOpen) { report(QStringLiteral("Transport closed.")); emit disconnected(); }
    if (!reason.isEmpty()) { report(reason); emit connectionFailed(reason); }
    m_closing = false; emit availabilityChanged();
    if (m_autoReconnect && !m_deliberateDisconnect) { m_reconnectTimer.start(); }
}
bool Kxpa100Model::isResponding() const { return m_connected && m_now() - m_lastReply <= 5000; }
QString Kxpa100Model::description() const
{
    return m_network ? QStringLiteral("NETWORK %1:%2 (bridge controls baud)").arg(m_host).arg(m_port)
                     : QStringLiteral("DIRECT %1 at %2 baud").arg(m_portName).arg(m_baud);
}
qint64 Kxpa100Model::fieldAgeMs(const QString& command) const
{
    const auto it = m_received.constFind(command);
    return it == m_received.cend() ? -1 : std::max<qint64>(0, m_now() - *it);
}
bool Kxpa100Model::fresh(const QString& key, int maxAge) const
{
    const qint64 age = fieldAgeMs(key);
    return age >= 0 && age <= maxAge;
}
bool Kxpa100Model::idle() const
{
    return m_status.tuning == false && m_status.inputWatts == 0 && m_status.forwardWatts == 0 &&
        fresh(QStringLiteral("^TP")) && fresh(QStringLiteral("^PI")) && fresh(QStringLiteral("^PF"));
}
QString Kxpa100Model::unavailableReason(bool writing, bool tuner) const
{
    if (!m_connected) { return QStringLiteral("Connect the KXPA100 first."); }
    if (!m_detected) { return QStringLiteral("Waiting for KXPA100 identity, serial and firmware."); }
    if (!isResponding()) { return QStringLiteral("KXPA100 readback is stale."); }
    if (operationBusy() || m_deferredTuneCancel) { return QStringLiteral("Another KXPA100 operation is in progress."); }
    if (tuner && m_status.tunerPresent != true) { return QStringLiteral("The optional KXAT100 tuner is absent or not yet identified."); }
    if (writing && !idle()) { return QStringLiteral("Fresh idle tuner and zero input/output power are required."); }
    return {};
}
bool Kxpa100Model::tunerEligible() const
{
    return m_connected && m_detected && isResponding() && m_status.tunerPresent == true &&
        fresh(QStringLiteral("^TU")) && fresh(QStringLiteral("^MD")) && fresh(QStringLiteral("^AN")) &&
        m_status.mode && m_status.mode != Mode::Bypass && m_status.antenna.has_value();
}
bool Kxpa100Model::amplifierMeterEligible() const
{
    return m_connected && m_detected && isResponding() && m_status.operate == true && m_status.tuning == false &&
        fresh(QStringLiteral("^OP")) && fresh(QStringLiteral("^TP")) && fresh(QStringLiteral("^PF"));
}

void Kxpa100Model::report(const QString& text, bool polling)
{
    if (polling && !m_verbose) { return; }
    qCDebug(lcKxpa100).noquote() << text;
    const QString line = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + text;
    m_debug.append(line); while (m_debug.size() > 1000) { m_debug.removeFirst(); }
    emit diagnostic(line); emit debugChanged();
}
bool Kxpa100Model::write(const QByteArray& bytes)
{
    if (!m_connected || bytes.isEmpty()) { return false; }
    bool success;
    if (m_write) { success = m_write(bytes); }
    else {
        QIODevice* device = m_socket.get();
#ifdef HAVE_SERIALPORT
        if (!m_network) { device = m_serial.get(); }
#endif
        success = device->bytesToWrite() < 1024 && device->write(bytes) == bytes.size();
    }
    if (!success) { transportClosed(QStringLiteral("KXPA100 transport write failed.")); return false; }
    report(QStringLiteral("TX %1").arg(QString::fromLatin1(bytes)), !m_operationWriting); return true;
}
void Kxpa100Model::enqueue(Request request)
{
    if (!m_connected || (request.mutation && !operationBusy())) { return; }
    if (request.wire.isEmpty() || m_queue.size() >= 64) { fail(QStringLiteral("Invalid or excessive KXPA100 request queue.")); return; }
    m_queue.enqueue(std::move(request));
}
void Kxpa100Model::query(const QByteArray& wire, std::function<void()> complete)
{
    enqueue({wire, wire.chopped(1), [wire](const QByteArray& frame) { return matchesStatusQuery(wire,frame); },
        [complete = std::move(complete)](const QByteArray&) { if (complete) { complete(); } }});
}
void Kxpa100Model::queueIdentity()
{
    for (const QByteArray& command : {QByteArray("^I;"),QByteArray("^RV;"),QByteArray("^SN;"),QByteArray("^TU;"),
         QByteArray("^TP;"),QByteArray("^PI;"),QByteArray("^PF;"),QByteArray("^MD;"),QByteArray("^AN;")}) { query(command); }
}
void Kxpa100Model::tick()
{
    const qint64 now = m_now();
    if (m_connecting && now - m_connectStarted > 10000) { transportClosed(QStringLiteral("Connection timed out.")); return; }
    if (!m_connected) { return; }
    emit availabilityChanged();
    if (!m_connected) { return; }
    if (m_active) {
        if (now - m_sentAt >= m_active->timeoutMs) {
            // This protocol has no transaction IDs. Closing the transport is the
            // only way to keep a timed-out response from satisfying a later GET.
            transportClosed(QStringLiteral("KXPA100 reply timed out; completion is uncertain. No write was retried."));
        }
        return;
    }
    if (!m_settleGetter.isEmpty()) {
        if (now >= m_settleDeadline) { fail(QStringLiteral("Requested value was not confirmed; no write was retried.")); return; }
        if (m_queue.isEmpty() && now >= m_settleNext) { pollSettle(); }
    }
    if (m_deferredTuneCancel && !operationBusy()) {
        m_deferredTuneCancel = false; cancelTune();
    }
    if (m_queue.isEmpty()) {
        if (operationBusy() || now < m_nextPoll) { return; }
        const QList<QPair<QByteArray,int>> priority{{"^TP;",750},{"^PI;",750},{"^PF;",750},{"^SW;",1500},
            {"^MD;",2500},{"^AN;",2500},{"^OP;",2500},{"^TU;",2500},{"^FL;",2500}};
        for (const auto& [command, age] : priority) {
            if (!fresh(QString::fromLatin1(command.chopped(1)),age)) { query(command); break; }
        }
        if (m_queue.isEmpty()) { query(kPolls[m_pollIndex++ % kPolls.size()]); }
        m_nextPoll = now + 50;
    }
    if (m_queue.isEmpty()) { return; }
    m_active = m_queue.dequeue(); m_sentAt = now;
    if (m_active->mutation && ((m_active->requiresIdle && !idle()) ||
        m_status.serial != m_operationSerial || m_status.firmware != m_operationFirmware)) {
        fail(QStringLiteral("Write stopped: device identity or idle/RF readback is unsafe or stale.")); return;
    }
    if (m_active->mutation && m_tunePermit && !m_tunePermit()) { fail(QStringLiteral("Operation intent expired; request discarded.")); return; }
    const QByteArray wire = m_active->wire; write(wire);
}
void Kxpa100Model::receiveBytes(const QByteArray& bytes) { m_parser->feed(bytes); }
void Kxpa100Model::receiveFrame(const QByteArray& frame)
{
    if (!m_connected) { return; }
    report(QStringLiteral("RX %1;").arg(QString::fromLatin1(frame)), true);
    if (frame == "^FE") {
        m_tuneEnded = true; emit telemetryReceived(QStringLiteral("^FE"), m_status); return;
    }
    Status parsed = m_status;
    const bool telemetry = applyStatus(frame,parsed);
    if (telemetry && ((!m_status.identity.isEmpty() && parsed.identity != m_status.identity) ||
        (!m_status.serial.isEmpty() && parsed.serial != m_status.serial) ||
        (!m_status.firmware.isEmpty() && parsed.firmware != m_status.firmware))) {
        transportClosed(QStringLiteral("KXPA100 identity changed; pending operations discarded.")); return;
    }
    if (!m_active || !m_active->matches(frame)) { return; }
    if (m_now() - m_sentAt >= m_active->timeoutMs) {
        transportClosed(QStringLiteral("KXPA100 response arrived after its deadline; completion discarded.")); return;
    }
    Request request = std::move(*m_active); m_active.reset();
    const quint64 generation = m_generation;
    m_lastReply = m_now();
    if (telemetry && !request.telemetryKey.isEmpty()) {
        m_status = parsed; m_received[QString::fromLatin1(request.telemetryKey)] = m_now();
        if (!m_detected && m_status.identity == QStringLiteral("KXPA100") && !m_status.serial.isEmpty() && !m_status.firmware.isEmpty()) {
            m_detected = true; emit detectedChanged(true);
        }
        emit statusUpdated(m_status); emit telemetryReceived(QString::fromLatin1(request.telemetryKey),m_status);
    }
    if (generation == m_generation && m_connected && request.complete) { request.complete(frame); }
}
bool Kxpa100Model::begin(const QString& name, bool writing, bool tuner)
{
    const QString reason = unavailableReason(writing,tuner);
    if (!reason.isEmpty()) { emit operationFailed(reason); return false; }
    ++m_generation; m_queue.clear(); m_operation = name; m_operationWriting = writing;
    m_operationSerial = m_status.serial; m_operationFirmware = m_status.firmware;
    m_progress = 0; m_total = 0;
    const quint64 generation = m_generation;
    report(name); emit operationBusyChanged(true); emit availabilityChanged();
    return m_connected && generation == m_generation && operationBusy();
}
void Kxpa100Model::finish(const QString& message)
{
    m_operation.clear(); m_operationWriting = false; m_tunePermit = {};
    m_settleGetter.clear(); m_settleVerify = {}; m_edits.clear(); m_regionComplete = {};
    m_nextPoll = m_now() + 50;
    report(message); emit operationBusyChanged(false); emit operationFinished(message); emit availabilityChanged();
}
void Kxpa100Model::fail(const QString& reason)
{
    ++m_generation;
    const bool busy = operationBusy();
    m_queue.clear(); m_active.reset(); m_operation.clear(); m_operationWriting = false;
    m_tunePermit = {}; m_settleGetter.clear(); m_settleVerify = {}; m_edits.clear();
    m_regionComplete = {}; m_regionData.clear(); m_pendingBackup = {};
    m_nextPoll = m_now() + 50;
    qCWarning(lcKxpa100).noquote() << reason;
    report(reason); if (busy) { emit operationBusyChanged(false); } emit operationFailed(reason); emit availabilityChanged();
}
void Kxpa100Model::cancelOperation()
{
    if (!operationBusy()) { return; }
    if (m_active) { transportClosed(QStringLiteral("Operation canceled during a request. Already applied writes remain; reconnect to inspect.")); }
    else { fail(QStringLiteral("Operation canceled. Already confirmed writes remain applied.")); }
}
void Kxpa100Model::preflight(std::function<void()> ready, bool requireIdle)
{
    query("^I;"); query("^RV;"); query("^SN;"); query("^TP;"); query("^PI;");
    query("^PF;",[this,ready=std::move(ready),requireIdle] {
        if (!operationBusy()) { return; }
        if (m_status.serial != m_operationSerial || m_status.firmware != m_operationFirmware || (requireIdle && !idle())) {
            fail(QStringLiteral("Operation stopped: device identity changed or RF/tuning is active.")); return;
        }
        ready();
    });
}
bool Kxpa100Model::requestValue(const QByteArray& setter, const QByteArray& getter,
    std::function<bool(const Status&)> verify, const QString& name, bool tuner)
{
    if (setter.isEmpty() || !begin(name,true,tuner)) { return false; }
    preflight([this,setter,getter,verify=std::move(verify)] { settle(setter,getter,verify); }); return true;
}
void Kxpa100Model::settle(const QByteArray& setter, const QByteArray& getter,
    std::function<bool(const Status&)> verify, bool requireIdle)
{
    m_settleGetter = getter; m_settleVerify = std::move(verify); m_settleDeadline = m_now() + 2500;
    m_settleRequiresIdle = requireIdle;
    enqueue({setter + getter,getter.chopped(1),[getter](const QByteArray& frame) { return matchesStatusQuery(getter,frame); },
        [this](const QByteArray&) { confirmSettle(); },2000,true,requireIdle});
}
void Kxpa100Model::confirmSettle()
{
    if (m_settleGetter.isEmpty() || !operationBusy()) { return; }
    if (m_now() >= m_settleDeadline) { fail(QStringLiteral("Readback arrived after the operation deadline.")); return; }
    if (m_settleVerify(m_status)) { finish(QStringLiteral("Device readback confirmed.")); return; }
    m_settleNext = m_now() + 100;
}
void Kxpa100Model::pollSettle()
{
    if (m_settleRequiresIdle && !idle()) { fail(QStringLiteral("RF or tuning became active while confirming the change.")); return; }
    query(m_settleGetter,[this] { confirmSettle(); });
}
bool Kxpa100Model::setOperate(bool value)
{
    return requestValue(value ? "^OP1;" : "^OP0;","^OP;",[value](const Status& s) { return s.operate == value; },QStringLiteral("Set PA operation"));
}
bool Kxpa100Model::setMode(Mode value)
{
    const QByteArray command = value == Mode::Auto ? "^MDA;" : value == Mode::Manual ? "^MDM;" : "^MDB;";
    return requestValue(command,"^MD;",[value](const Status& s) { return s.mode == value; },QStringLiteral("Set tuner mode"),true);
}
bool Kxpa100Model::setAntenna(int value)
{
    if (value < 1 || value > 2) { return false; }
    if (m_configuration && m_status.band && !((*m_configuration)[*m_status.band] & (1 << (value - 1)))) {
        emit operationFailed(QStringLiteral("This antenna is disabled on the current band.")); return false;
    }
    return requestValue("^AN" + QByteArray::number(value) + ';',"^AN;",[value](const Status& s) { return s.antenna == value; },QStringLiteral("Select antenna"),true);
}
bool Kxpa100Model::setCapacitorMask(int value)
{
    if (value < 0 || value > 255) { return false; }
    return requestValue("^CR" + hexMask(value) + ';',"^CR;",[value](const Status& s) { return s.capacitorMask == value; },QStringLiteral("Set capacitance"),true);
}
bool Kxpa100Model::setInductorMask(int value)
{
    if (value < 0 || value > 255) { return false; }
    return requestValue("^LR" + hexMask(value) + ';',"^LR;",[value](const Status& s) { return s.inductorMask == value; },QStringLiteral("Set inductance"),true);
}
bool Kxpa100Model::setCapacitorSide(CapacitorSide value)
{
    return requestValue(value == CapacitorSide::Transmitter ? "^SIT;" : "^SIA;","^SI;",
        [value](const Status& s) { return s.side == value; },QStringLiteral("Set capacitor side"),true);
}
bool Kxpa100Model::setBypassed(bool value)
{
    return requestValue(value ? "^BYB;" : "^BYN;","^BY;",[value](const Status& s) { return s.bypassed == value; },QStringLiteral("Set physical ATU bypass"),true);
}
bool Kxpa100Model::setAttenuator(bool value)
{
    if (m_status.attenuator == 2) { emit operationFailed(QStringLiteral("The rear-panel switch holds the attenuator on.")); return false; }
    return requestValue(value ? "^AT1;" : "^AT0;","^AT;",[value](const Status& s) { return s.attenuator == int(value); },QStringLiteral("Set attenuator"));
}
bool Kxpa100Model::clearFault()
{
    return requestValue("^FLC;","^FL;",[](const Status& s) { return s.faultCode == 'N'; },QStringLiteral("Clear current fault"));
}
bool Kxpa100Model::setBand(int band)
{
    if (band < 0 || band > 10) { return false; }
    return requestValue("^BN" + QByteArray::number(band).rightJustified(2,'0') + ';',"^BN;",
        [band](const Status& s) { return s.band == band; },QStringLiteral("Select band"));
}
bool Kxpa100Model::startTune(std::function<bool()> permit)
{
    if (!begin(QStringLiteral("Start fresh tune"),true,true)) { return false; }
    m_tunePermit = std::move(permit); m_tuneEnded = false;
    preflight([this] { settle("^FT;","^TP;",[this](const Status& s) { return s.tuning == true || m_tuneEnded; }); }); return true;
}
bool Kxpa100Model::cancelTune()
{
    if (!begin(QStringLiteral("Cancel tuner search"),false,true)) { return false; }
    m_operationWriting = true;
    settle("^FE;","^TP;",[](const Status& s) { return s.tuning == false; },false); return true;
}
void Kxpa100Model::cancelTuneAfterPendingOperation()
{
    if (!m_connected || !m_detected) { return; }
    if (operationBusy()) { m_deferredTuneCancel = true; return; }
    cancelTune();
}
void Kxpa100Model::readMatch(std::function<void()> complete)
{
    for (const QByteArray& command : {QByteArray("^TP;"),QByteArray("^MD;"),QByteArray("^AN;"),QByteArray("^BY;"),
         QByteArray("^SI;"),QByteArray("^CR;"),QByteArray("^SW;"),QByteArray("^SB;")}) { query(command); }
    query("^LR;",std::move(complete));
}
bool Kxpa100Model::readMatchStatus()
{
    if (!begin(QStringLiteral("Read tuner match"),false,true)) { return false; }
    readMatch([this] { finish(QStringLiteral("Tuner match readback complete.")); }); return true;
}
void Kxpa100Model::requestMemory(int frequencyKhz, const QByteArray& prefix, std::function<void()> complete)
{
    enqueue({prefix + buildFrequencyCommand("^DM",frequencyKhz),{},[](const QByteArray& frame) { return decodeMemoryBin(frame).has_value(); },
        [this,frequencyKhz,complete=std::move(complete)](const QByteArray& frame) {
            const MemoryBin bin = *decodeMemoryBin(frame);
            if (bin.frequencyKhz != frequencyKhz) { fail(QStringLiteral("Memory reply is for a different frequency.")); return; }
            m_memories = bin; emit memoriesRead(bin);
            if (operationBusy() && complete) { complete(); }
        },4000,!prefix.isEmpty()});
}
bool Kxpa100Model::inspectMemories(int frequencyKhz)
{
    if (!isSupportedFrequency(frequencyKhz) || !begin(QStringLiteral("Inspect tuner memories"),false,true)) { return false; }
    requestMemory(frequencyKhz,{},[this] { finish(QStringLiteral("Memory readback complete.")); }); return true;
}
bool Kxpa100Model::memorize(int frequencyKhz, std::function<bool()> permit)
{
    if (!isSupportedFrequency(frequencyKhz) || !begin(QStringLiteral("Save current match"),true,true)) { return false; }
    m_tunePermit = std::move(permit);
    preflight([this,frequencyKhz] {
        readMatch([this,frequencyKhz] {
            const Status expected = m_status;
            if (!expected.antenna || !expected.bypassed || !expected.side || !expected.capacitorMask || !expected.inductorMask) {
                fail(QStringLiteral("Current match is incomplete.")); return;
            }
            requestMemory(frequencyKhz,buildFrequencyCommand("^SM",frequencyKhz),[this,expected] {
                const bool found = std::any_of(m_memories.matches.cbegin(),m_memories.matches.cend(),[&expected](const MemoryMatch& match) {
                    return !match.empty && match.antenna == expected.antenna && match.bypassed == expected.bypassed &&
                        (match.bypassed || (match.side == expected.side && match.capacitorMask == expected.capacitorMask && match.inductorMask == expected.inductorMask));
                });
                if (!found) { fail(QStringLiteral("Saved match was not confirmed by memory readback.")); return; }
                finish(QStringLiteral("Saved match confirmed by memory readback."));
            });
        });
    }); return true;
}
bool Kxpa100Model::recallFrequency(int frequencyKhz)
{
    if (!isSupportedFrequency(frequencyKhz) || !begin(QStringLiteral("Recall frequency match"),true,true)) { return false; }
    preflight([this,frequencyKhz] {
        enqueue({buildFrequencyCommand("^MT",frequencyKhz) + "^BN;","^BN",[](const QByteArray& frame) { return matchesStatusQuery("^BN;",frame); },
            [this](const QByteArray&) { readMatch([this] { finish(QStringLiteral("Recall processed; current tuner settings read back.")); }); },2000,true});
    }); return true;
}
} // namespace AetherSDR
