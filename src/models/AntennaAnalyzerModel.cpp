#include "AntennaAnalyzerModel.h"

#include <QDateTime>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>
#include <complex>
#include <utility>

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcAetherLab, "aether.lab")
constexpr qint64 kMinHz = 100000;
constexpr qint64 kMaxHz = 600000000;
constexpr int kMaxIntervals = 1000; // host PoC cap, not a firmware limit
constexpr int kAckTimeoutMs = 3000;
constexpr int kSampleTimeoutMs = 15000;
constexpr int kSweepTimeoutMs = 60000;
constexpr int kMaxLine = 512;
constexpr int kTraceReports = 128;

QJsonObject errorResult(const QString& message)
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}
QJsonValue finiteValue(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}
} // namespace

AntennaAnalyzerModel::AntennaAnalyzerModel(QObject* parent) : QObject(parent)
{
    if (RigExpertHidTransport::available()) {
        m_source = &RigExpertHidTransport::enumerate;
        m_factory = &RigExpertHidTransport::create;
    }
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, &AntennaAnalyzerModel::deadlineExpired);
    m_identityQuiet.setSingleShot(true);
    m_identityQuiet.setInterval(500);
    connect(&m_identityQuiet, &QTimer::timeout, this, [this] {
        if (m_state == State::Identifying && !m_identity.isEmpty()) {
            m_deadline.stop();
            m_state = State::Ready;
            emit changed();
        }
    });
}

AntennaAnalyzerModel::~AntennaAnalyzerModel()
{
    if (m_port) { m_port->finish(commandReport("off\r")); }
}

void AntennaAnalyzerModel::setUsbBackend(DeviceSource source, TransportFactory factory)
{
    if (m_port) { return; }
    m_source = std::move(source);
    m_factory = std::move(factory);
}

bool AntennaAnalyzerModel::reject(const QString& error)
{
    m_error = error;
    emit changed();
    return false;
}

bool AntennaAnalyzerModel::refreshDevices()
{
    if (m_port) { return reject(QStringLiteral("Disconnect the instrument before rescanning")); }
    if (!m_source) { return reject(QStringLiteral("USB HID support is unavailable in this build")); }
    m_devices = m_source();
    m_error.clear();
    emit changed();
    return true;
}

bool AntennaAnalyzerModel::connectDevice(int index)
{
    if (m_port) { return reject(QStringLiteral("Instrument session is already open or closing")); }
    if (!m_factory || index < 0 || index >= m_devices.size()) {
        return reject(QStringLiteral("Select a discovered Stick Pro USB interface"));
    }
    m_port = m_factory(this);
    if (!m_port) { return reject(QStringLiteral("Cannot create USB transport")); }
    m_selected = m_devices.at(index);
    m_samples = {};
    m_startHz = 0;
    m_stopHz = 0;
    m_intervals = 0;
    ++m_generation;
    m_identity.clear();
    m_identityLengthQuirkReports = 0;
    m_info = {};
    m_trace = {};
    m_pending.clear();
    m_error.clear();
    m_state = State::Identifying;
    RigExpertHidTransport* port = m_port;
    connect(port, &RigExpertHidTransport::opened, this, [this, port] {
        if (m_port != port || m_state != State::Identifying) { return; }
        sendCommand("VER\r\n");
    });
    connect(port, &RigExpertHidTransport::reportReceived, this, [this, port](const QByteArray& report) {
        if (m_port == port) { receive(report); }
    });
    connect(port, &RigExpertHidTransport::reportWritten, this, [this, port](const QByteArray& report) {
        if (m_port == port) {
            record(QStringLiteral("tx"), report);
            if (report == commandReport(m_expectedCommand)) { m_commandWritten = true; }
            emit changed();
        }
    });
    connect(port, &RigExpertHidTransport::failed, this, [this, port](const QString& error) {
        if (m_port == port) { finish(State::Error, error); }
    });
    connect(port, &RigExpertHidTransport::closed, this, [this, port] {
        if (m_port != port) { return; }
        if (m_state != State::Closing) {
            m_terminal = State::Error;
            m_error = QStringLiteral("USB transport closed unexpectedly");
        }
        m_port = nullptr;
        m_deadline.stop();
        m_identityQuiet.stop();
        m_pending.clear();
        m_state = m_terminal;
        port->deleteLater();
        emit changed();
    });
    m_deadline.start(kAckTimeoutMs);
    port->start(m_selected.path);
    emit changed();
    return true;
}

QByteArray AntennaAnalyzerModel::commandReport(const QByteArray& command)
{
    if (command.isEmpty() || command.size() > 62) { return {}; }
    QByteArray report(64, '\0');
    report[0] = 0x07;
    report[1] = static_cast<char>(command.size());
    report.replace(2, command.size(), command);
    return report;
}

void AntennaAnalyzerModel::sendCommand(const QByteArray& command)
{
    m_expectedCommand = command;
    m_commandWritten = false;
    if (m_port) { m_port->send(commandReport(command)); }
}

void AntennaAnalyzerModel::record(const QString& direction, const QByteArray& report)
{
    m_trace.append(QJsonObject{{QStringLiteral("direction"), direction},
        {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("bytes"), report.size()},
        {QStringLiteral("hex"), QString::fromLatin1(report.left(64).toHex())}});
    while (m_trace.size() > kTraceReports) { m_trace.removeAt(0); }
}

void AntennaAnalyzerModel::receive(const QByteArray& report)
{
    record(QStringLiteral("rx"), report);
    if (m_state == State::Closing) { return; }
    if (report.size() < 2 || report.size() > 64 || report.at(0) != 0x07) {
        finish(State::Error, QStringLiteral("Malformed HID report; inspect the protocol trace")); return;
    }
    const int declared = static_cast<unsigned char>(report.at(1));
    // Stick Pro VER 108 declares 63 on full 64-byte FULLINFO reports. Its HID
    // descriptor and a 256-byte read confirm only 62 text bytes are available.
    // Bound optional identity metadata as AntScope does; never repair CSV data.
    const bool identityLengthQuirk = report.size() == 64 && declared == 63
        && (m_state == State::Identifying || m_state == State::Ready);
    if (declared > report.size() - 2 && !identityLengthQuirk) {
        finish(State::Error, QStringLiteral("Malformed HID report; inspect the protocol trace")); return;
    }
    if (identityLengthQuirk) {
        ++m_identityLengthQuirkReports;
        QJsonObject entry = m_trace.last().toObject();
        entry.insert(QStringLiteral("identityLengthQuirk"), true);
        entry.insert(QStringLiteral("payloadBytesUsed"), 62);
        m_trace[m_trace.size() - 1] = entry;
        if (m_identityLengthQuirkReports == 1) {
            qCWarning(lcAetherLab) << "Stick Pro FULLINFO length 63 bounded to 62 received bytes";
        }
    }
    m_pending.append(report.mid(2, qMin(declared, static_cast<int>(report.size()) - 2)));
    while (m_pending.contains('\n')) {
        const qsizetype end = m_pending.indexOf('\n');
        QByteArray line = m_pending.left(end);
        m_pending.remove(0, end + 1);
        if (line.endsWith('\r')) { line.chop(1); }
        if (line.size() > kMaxLine || line.contains('\r') || line.contains('\0')) {
            finish(State::Error, QStringLiteral("Malformed or oversized protocol line")); return;
        }
        if (!line.isEmpty()) { lineReceived(line); }
        if (m_state == State::Closing) { m_pending.clear(); break; }
    }
    if (m_pending.size() > kMaxLine) {
        finish(State::Error, QStringLiteral("Protocol line exceeds the 512-byte host limit"));
    }
    emit changed();
}

void AntennaAnalyzerModel::lineReceived(const QByteArray& line)
{
    if (line == "ERROR") { finish(State::Error, QStringLiteral("Stick Pro returned ERROR")); return; }
    if (m_state == State::Identifying) {
        if (m_info.size() < 32) { m_info.append(QString::fromLatin1(line)); }
        static const QRegularExpression identity(QStringLiteral("^Stick Pro\\s+[^\\s]+(?:\\s+.*)?$"),
                                                  QRegularExpression::CaseInsensitiveOption);
        if (identity.match(QString::fromLatin1(line)).hasMatch() && m_identity.isEmpty()) {
            m_identity = QString::fromLatin1(line);
            sendCommand("FULLINFO\r\n");
        }
        if (!m_identity.isEmpty()) { m_identityQuiet.start(); }
        return;
    }
    if (m_state == State::Ready) {
        // Late FULLINFO is retained, never interpreted as a sweep acknowledgement.
        if (m_info.size() < 32) { m_info.append(QString::fromLatin1(line)); }
        return;
    }
    if (m_state == State::SettingFrequency || m_state == State::SettingWidth) {
        if (!m_commandWritten) { finish(State::Error, QStringLiteral("Reply arrived before the corresponding HID write receipt")); return; }
        if (line != "OK") { finish(State::Error, QStringLiteral("Expected exact OK during sweep setup")); return; }
        if (m_state == State::SettingFrequency) {
            m_state = State::SettingWidth;
            sendCommand("SW" + QByteArray::number(m_stopHz - m_startHz) + "\r");
            m_deadline.start(kAckTimeoutMs);
        } else {
            m_state = State::Sweeping;
            sendCommand("FRX" + QByteArray::number(m_intervals) + "\r");
            m_deadline.start(kSampleTimeoutMs);
        }
        return;
    }
    if (m_state != State::Sweeping) { return; }
    if (!m_commandWritten) { finish(State::Error, QStringLiteral("Data arrived before the FRX HID write receipt")); return; }
    if (m_sweepElapsed.elapsed() >= kSweepTimeoutMs) { deadlineExpired(); return; }
    if (line == "OK") {
        // Some firmware acknowledges FRX before its first CSV row. It is not
        // completion evidence and cannot extend either acquisition deadline.
        if (m_samples.isEmpty()) { return; }
        finish(State::Error, QStringLiteral("Unexpected OK inside measurement data")); return;
    }
    const QJsonObject sample = impedanceSample(line);
    if (sample.isEmpty()) { finish(State::Error, QStringLiteral("Invalid frequency,R,X row")); return; }
    const qint64 hz = static_cast<qint64>(sample.value(QStringLiteral("frequencyHz")).toDouble());
    const qint64 previous = m_samples.isEmpty() ? m_startHz - 1
        : static_cast<qint64>(m_samples.last().toObject().value(QStringLiteral("frequencyHz")).toDouble());
    if (hz < m_startHz || hz > m_stopHz || hz <= previous
        || (m_samples.isEmpty() && hz != m_startHz)) {
        finish(State::Error, QStringLiteral("Unexpected sweep frequency or ordering; retain trace for qualification")); return;
    }
    m_samples.append(sample);
    m_deadline.start(qMin(kSampleTimeoutMs, kSweepTimeoutMs - static_cast<int>(m_sweepElapsed.elapsed())));
    // AntScope2 normal HID acquisition delivers FRX N + 1 rows. This remains
    // a hardware qualification hypothesis, with exact endpoints also required.
    if (m_samples.size() == m_intervals + 1) {
        if (hz != m_stopHz) { finish(State::Error, QStringLiteral("Sweep count reached without the requested stop frequency")); }
        else { finish(State::Complete); }
    }
}

bool AntennaAnalyzerModel::sweep(qint64 startHz, qint64 stopHz, int intervals, bool rfIsolated)
{
    if (!rfIsolated) { return reject(QStringLiteral("Confirm that the analyzer RF port is isolated from transmitters")); }
    if (m_state != State::Ready || !m_port) { return reject(QStringLiteral("Connect and identify the Stick Pro before sweeping")); }
    if (startHz < kMinHz || stopHz > kMaxHz || startHz >= stopHz
        || ((stopHz - startHz) % 2) != 0 || intervals < 1 || intervals > kMaxIntervals
        || stopHz - startHz < intervals) {
        return reject(QStringLiteral("Use 100 kHz–600 MHz, an even positive span, and 1–1000 FRX intervals"));
    }
    m_error.clear();
    m_samples = {};
    m_startHz = startHz;
    m_stopHz = stopHz;
    m_intervals = intervals;
    m_pending.clear();
    m_sweepElapsed.start();
    m_state = State::SettingFrequency;
    sendCommand("FQ" + QByteArray::number(startHz + (stopHz - startHz) / 2) + "\r");
    m_deadline.start(kAckTimeoutMs);
    emit changed();
    return true;
}

void AntennaAnalyzerModel::finish(State result, const QString& error)
{
    m_deadline.stop();
    m_identityQuiet.stop();
    if (!error.isEmpty()) { m_error = error; qCWarning(lcAetherLab).noquote() << error; }
    m_terminal = result;
    if (m_port) {
        if (m_state != State::Closing) {
            m_state = State::Closing;
            m_port->finish(commandReport("off\r"));
        }
    } else { m_state = result; }
    emit changed();
}

void AntennaAnalyzerModel::stop()
{
    if (m_port) { finish(State::Cancelled); }
}
void AntennaAnalyzerModel::disconnectDevice() { finish(State::Disconnected); }
void AntennaAnalyzerModel::deadlineExpired()
{
    finish(State::Error, QStringLiteral("Protocol deadline expired; partial data retained, RF-off unconfirmed"));
}

QString AntennaAnalyzerModel::stateText() const
{
    switch (m_state) {
    case State::Disconnected: return QStringLiteral("disconnected");
    case State::Identifying: return QStringLiteral("identifying");
    case State::Ready: return QStringLiteral("ready");
    case State::SettingFrequency: return QStringLiteral("setting-frequency");
    case State::SettingWidth: return QStringLiteral("setting-width");
    case State::Sweeping: return QStringLiteral("sweeping");
    case State::Closing: return QStringLiteral("closing");
    case State::Complete: return QStringLiteral("complete");
    case State::Cancelled: return QStringLiteral("cancelled");
    case State::Error: return QStringLiteral("error");
    }
    return {};
}

QJsonObject AntennaAnalyzerModel::impedanceSample(const QByteArray& line)
{
    const QList<QByteArray> fields = line.split(',');
    if (fields.size() != 3) { return {}; }
    static const QRegularExpression number(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"));
    double values[3]{};
    for (int i = 0; i < 3; ++i) {
        const QString text = QString::fromLatin1(fields.at(i).trimmed());
        if (!number.match(text).hasMatch()) { return {}; }
        bool ok = false;
        values[i] = text.toDouble(&ok);
        if (!ok || !std::isfinite(values[i])) { return {}; }
    }
    const double hz = values[0] * 1e6;
    if (!std::isfinite(hz) || hz < kMinHz || hz > kMaxHz) { return {}; }
    const std::complex<double> z(values[1], values[2]);
    const std::complex<double> gamma = (z - 50.0) / (z + 50.0);
    const double rho = std::abs(gamma);
    const bool passive = values[1] >= 0 && std::isfinite(rho) && rho <= 1;
    const double swr = passive ? (1 + rho) / (1 - rho) : NAN;
    const double rl = passive ? -20 * std::log10(rho) : NAN;
    return {{QStringLiteral("frequencyHz"), std::round(hz)},
        {QStringLiteral("raw"), QString::fromLatin1(line)},
        {QStringLiteral("rOhm"), values[1]}, {QStringLiteral("xOhm"), values[2]},
        {QStringLiteral("swr"), finiteValue(swr)}, {QStringLiteral("returnLossDb"), finiteValue(rl)},
        {QStringLiteral("gammaReal"), finiteValue(gamma.real())},
        {QStringLiteral("gammaImag"), finiteValue(gamma.imag())},
        {QStringLiteral("quality"), !passive ? QStringLiteral("non-passive-or-singular")
            : rho == 0 ? QStringLiteral("perfect-match")
            : rho == 1 ? QStringLiteral("total-reflection") : QStringLiteral("valid")}};
}

QJsonObject AntennaAnalyzerModel::snapshot() const
{
    QJsonArray devices;
    for (qsizetype i = 0; i < m_devices.size(); ++i) {
        const RigExpertUsbDevice& device = m_devices.at(i);
        devices.append(QJsonObject{{QStringLiteral("index"), i}, {QStringLiteral("serial"), device.serial},
            {QStringLiteral("product"), device.product}, {QStringLiteral("path"), device.path}});
    }
    return {{QStringLiteral("ok"), true}, {QStringLiteral("state"), stateText()},
        {QStringLiteral("usbAvailable"), static_cast<bool>(m_factory)},
        {QStringLiteral("generation"), static_cast<qint64>(m_generation)},
        {QStringLiteral("identity"), m_identity}, {QStringLiteral("serial"), m_selected.serial},
        {QStringLiteral("identityLengthQuirkReports"), m_identityLengthQuirkReports},
        {QStringLiteral("fullInfoLines"), m_info}, {QStringLiteral("error"), m_error},
        {QStringLiteral("devices"), devices}, {QStringLiteral("samples"), m_samples},
        {QStringLiteral("trace"), m_trace}, {QStringLiteral("startHz"), m_startHz},
        {QStringLiteral("stopHz"), m_stopHz}, {QStringLiteral("frxIntervals"), m_intervals},
        {QStringLiteral("receivedRows"), m_samples.size()},
        {QStringLiteral("expectedRowsHypothesis"), m_intervals > 0 ? m_intervals + 1 : 0},
        {QStringLiteral("completionEvidence"), QStringLiteral("host-count-and-endpoints")},
        {QStringLiteral("countQualifiedOnHardware"), false},
        {QStringLiteral("rfOffConfirmed"), false}, {QStringLiteral("stationTxInterlock"), false}};
}

QJsonObject AntennaAnalyzerModel::automation(const QString& action, const QString& value)
{
    const QString normalized = action.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QLatin1String("status")) { return snapshot(); }
    bool accepted = false;
    if (normalized == QLatin1String("devices")) { accepted = refreshDevices(); }
    else if (normalized == QLatin1String("connect")) {
        bool ok = false;
        const int index = value.toInt(&ok);
        if (!ok) { return errorResult(QStringLiteral("lab connect requires a discovered device index")); }
        accepted = connectDevice(index);
    } else if (normalized == QLatin1String("sweep")) {
        const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 4 || parts.at(3) != QLatin1String("isolated")) {
            return errorResult(QStringLiteral("lab sweep requires startHz stopHz intervals isolated"));
        }
        bool a = false, b = false, c = false;
        const qint64 start = parts.at(0).toLongLong(&a);
        const qint64 stop = parts.at(1).toLongLong(&b);
        const int count = parts.at(2).toInt(&c);
        if (!a || !b || !c) { return errorResult(QStringLiteral("Invalid integer sweep parameters")); }
        accepted = sweep(start, stop, count, true);
    } else if (normalized == QLatin1String("stop")) { stop(); accepted = true; }
    else if (normalized == QLatin1String("disconnect")) { disconnectDevice(); accepted = true; }
    else { return errorResult(QStringLiteral("lab requires status|devices|connect|sweep|stop|disconnect")); }
    if (!accepted) { return errorResult(m_error); }
    QJsonObject result = snapshot();
    result.insert(QStringLiteral("accepted"), true);
    return result;
}

} // namespace AetherSDR
