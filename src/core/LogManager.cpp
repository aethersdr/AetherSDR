#include "LogManager.h"
#include "AppSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTime>

namespace MasterSDR {

// Define all logging categories (disabled by default)
Q_LOGGING_CATEGORY(lcDiscovery,  "mastersdr.discovery",  QtDebugMsg)
Q_LOGGING_CATEGORY(lcConnection, "mastersdr.connection",  QtDebugMsg)
Q_LOGGING_CATEGORY(lcProtocol,   "mastersdr.protocol",    QtDebugMsg)
Q_LOGGING_CATEGORY(lcAudio,      "mastersdr.audio",       QtWarningMsg)
Q_LOGGING_CATEGORY(lcVita49,     "mastersdr.vita49",      QtWarningMsg)
Q_LOGGING_CATEGORY(lcDsp,        "mastersdr.dsp",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcRade,       "mastersdr.rade",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcSmartLink,  "mastersdr.smartlink",   QtWarningMsg)
Q_LOGGING_CATEGORY(lcCat,        "mastersdr.cat",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDax,        "mastersdr.dax",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcMeters,     "mastersdr.meters",      QtWarningMsg)
Q_LOGGING_CATEGORY(lcTransmit,   "mastersdr.transmit",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcFirmware,   "mastersdr.firmware",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcTuner,      "mastersdr.tuner",       QtWarningMsg)
Q_LOGGING_CATEGORY(lcGui,        "mastersdr.gui",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDxCluster,  "mastersdr.dxcluster",   QtWarningMsg)
Q_LOGGING_CATEGORY(lcMqtt,       "mastersdr.mqtt",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcRbn,        "mastersdr.rbn",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDevices,    "mastersdr.devices",     QtWarningMsg)
Q_LOGGING_CATEGORY(lcPerf,       "mastersdr.perf",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcCw,         "mastersdr.cw",          QtWarningMsg)
Q_LOGGING_CATEGORY(lcSHistory,  "mastersdr.shistory",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcAx25,       "mastersdr.ax25",        QtWarningMsg)

LogManager::LogManager()
{
    // Register categories with human-readable labels and descriptions
    m_categories = {
        {"mastersdr.discovery",  "Discovery",    "UDP radio discovery broadcasts"},
        {"mastersdr.connection", "Connection / Commands", "Raw TCP command channel lines: TX commands, RX responses, and socket state"},
        {"mastersdr.protocol",   "Protocol / Status",     "Parsed SmartSDR protocol handling and model status updates"},
        {"mastersdr.audio",      "Audio",        "RX/TX audio, device negotiation, volume"},
        {"mastersdr.vita49",     "VITA-49",      "UDP packet routing: FFT, waterfall, meters, DAX"},
        {"mastersdr.dsp",        "DSP",          "NR2, RN2, CW decoder processing"},
        {"mastersdr.rade",       "RADE",         "FreeDV Radio Autoencoder digital voice"},
        {"mastersdr.smartlink",  "SmartLink",    "Auth0 login, TLS tunnel, WAN streaming"},
        {"mastersdr.cat",        "CAT/rigctld",  "rigctld TCP servers, PTY virtual serial ports"},
        {"mastersdr.dax",        "DAX",          "Virtual audio bridge (PipeWire/CoreAudio)"},
        {"mastersdr.meters",     "Meters",       "Meter definitions and value conversion"},
        {"mastersdr.transmit",   "Transmit",     "TX state, ATU, profiles, power control"},
        {"mastersdr.firmware",   "Firmware",     "Firmware download, staging, upload"},
        {"mastersdr.tuner",      "Tuner/AGM",    "TGXL tuner, Antenna Genius state"},
        {"mastersdr.gui",        "GUI",          "Window, applets, dialogs"},
        {"mastersdr.dxcluster",  "DX Cluster",   "DX cluster telnet connection and spot parsing"},
        {"mastersdr.mqtt",       "MQTT",         "MQTT telemetry client connection and messages"},
        {"mastersdr.rbn",        "RBN",          "Reverse Beacon Network connection and spots"},
        {"mastersdr.devices",    "Ext Devices",  "Serial port, FlexControl, MIDI, HID encoder"},
        {"mastersdr.perf",       "Performance",  "Render timing and CPU profiling data"},
        {"mastersdr.propforecast", "Propagation",  "Solar and propagation forecast updates"},
        {"mastersdr.cw",         "CW / netCW",    "CW keying, MIDI paddle, iambic, and netCW timing"},
        {"mastersdr.shistory",   "S History",     "Past-Signals voice detection: noise floor, region width, band-plan filter"},
        {"mastersdr.ax25",       "MasterModem", "AX.25 modem lifecycle, RX/TX audio, demod, framing, and packet diagnostics"},
    };

    // QLoggingCategory objects are defined above via Q_LOGGING_CATEGORY macros.
    // setFilterRules() controls them by name string — no need to hold pointers.
}

LogManager& LogManager::instance()
{
    // The Qt message handler can still be invoked during late teardown on
    // some platforms. Keep the manager alive for process lifetime and shut
    // down the writer explicitly from main().
    static LogManager* mgr = new LogManager;
    return *mgr;
}

bool LogManager::isEnabled(const QString& id) const
{
    for (const auto& c : m_categories)
        if (c.id == id) return c.enabled;
    return false;
}

void LogManager::setEnabled(const QString& id, bool on)
{
    for (auto& c : m_categories) {
        if (c.id == id) {
            if (c.enabled == on) return;
            c.enabled = on;
            applyFilterRules();
            saveSettings();
            emit categoryChanged(id, on);
            return;
        }
    }
}

void LogManager::setAllEnabled(bool on)
{
    for (auto& c : m_categories)
        c.enabled = on;
    applyFilterRules();
    saveSettings();
}

void LogManager::applyFilterRules()
{
    // Build a filter rule string for QLoggingCategory
    // Default: all mastersdr.* debug messages off, then enable selected ones
    QStringList rules;
    rules << "mastersdr.*.debug=false";
    for (const auto& c : m_categories) {
        if (c.enabled)
            rules << QString("%1.debug=true").arg(c.id);
    }
    QLoggingCategory::setFilterRules(rules.join('\n'));
}

bool LogManager::startLogging(const QString& path, bool mirrorToStderr)
{
    setActiveLogFilePath(path);

    const RetentionConfig cfg = retentionConfig();
    const qint64 maxBytes = static_cast<qint64>(cfg.activeLogMaxMb) * 1024 * 1024;

    // Rotation callback runs on the writer thread. It picks a fresh
    // timestamped path under the same dir, updates the active path, and
    // re-points the MasterSDR.log symlink so the Support dialog and
    // support-bundle scan continue to find the live file. Writer hands us
    // the closed file via currentPath; we never touch the writer's file
    // handle here. (#2498)
    m_writer.setRotationConfig(maxBytes,
        [this](const QString& currentPath) -> QString {
            const QString dir = QFileInfo(currentPath).absolutePath();
            const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
            QString candidate = dir + "/MasterSDR-" + ts + ".log";
            int suffix = 1;
            while (QFile::exists(candidate) && suffix <= 100) {
                candidate = dir + QString("/MasterSDR-%1-%2.log").arg(ts).arg(suffix++);
            }
            if (QFile::exists(candidate))
                return {};

            setActiveLogFilePath(candidate);

            const QString symlink = dir + "/MasterSDR.log";
            QFile::remove(symlink);
            QFile::link(candidate, symlink);
            return candidate;
        });

    if (!m_writer.start(path, mirrorToStderr))
        return false;

    return true;
}

void LogManager::shutdownLogging()
{
    m_writer.shutdown();
}

void LogManager::enqueueMessage(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const QString category = (ctx.category && *ctx.category)
        ? QString::fromUtf8(ctx.category)
        : QStringLiteral("default");

    m_writer.enqueue(type, QTime::currentTime(), category, msg);
    if (type == QtFatalMsg)
        m_writer.flush();
}

void LogManager::flushLog() const
{
    m_writer.flush();
}

AsyncLogWriter::Counters LogManager::logCounters() const
{
    return m_writer.counters();
}

QString LogManager::logFilePath() const
{
    QMutexLocker locker(&m_pathMutex);
    if (!m_activeLogFilePath.isEmpty()) {
        return m_activeLogFilePath;
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/MasterSDR.log";
}

void LogManager::setActiveLogFilePath(const QString& path)
{
    QMutexLocker locker(&m_pathMutex);
    m_activeLogFilePath = path;
}

qint64 LogManager::logFileSize() const
{
    flushLog();
    QFileInfo fi(logFilePath());
    return fi.exists() ? fi.size() : 0;
}

void LogManager::clearLog()
{
    if (m_writer.isRunning()) {
        m_writer.clearLog();
        return;
    }

    QFile f(logFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.close();
}

// AppSettings XML keys cannot contain dots — replace '.' with '_'
static QString settingsKey(const QString& id)
{
    QString key = "LogCategory_" + id;
    key.replace('.', '_');
    return key;
}

void LogManager::saveSettings()
{
    auto& s = AppSettings::instance();
    for (const auto& c : m_categories)
        s.setValue(settingsKey(c.id), c.enabled ? "True" : "False");
    s.save();
}

void LogManager::loadSettings()
{
    auto& s = AppSettings::instance();
    // Default Discovery, Commands, and Status to on
    static const QStringList defaultOn = {
        "mastersdr.discovery", "mastersdr.connection", "mastersdr.protocol"
    };
    for (auto& c : m_categories) {
        QString def = defaultOn.contains(c.id) ? "True" : "False";
        c.enabled = s.value(settingsKey(c.id), def).toString() == "True";
    }
    applyFilterRules();
}

LogManager::RetentionConfig LogManager::retentionConfig() const
{
    // Nested JSON per Principle V (constitution): one root key
    // "LogRetention" instead of three flat AppSettings keys.
    RetentionConfig cfg;
    const QString json = AppSettings::instance()
        .value("LogRetention", "").toString();
    if (json.isEmpty())
        return cfg;

    const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
    if (obj.contains("ActiveLogMaxMb"))
        cfg.activeLogMaxMb = obj.value("ActiveLogMaxMb").toInt(cfg.activeLogMaxMb);
    if (obj.contains("RetentionDays"))
        cfg.retentionDays = obj.value("RetentionDays").toInt(cfg.retentionDays);
    if (obj.contains("RetentionMaxTotalMb"))
        cfg.retentionMaxTotalMb = obj.value("RetentionMaxTotalMb").toInt(cfg.retentionMaxTotalMb);
    return cfg;
}

void LogManager::pruneOldLogs(const QString& dir)
{
    const RetentionConfig cfg = retentionConfig();
    QDir d(dir);
    if (!d.exists())
        return;

    // Newest-first scan; keep at least the two most-recent so "yesterday's
    // log" remains available for support cases even under aggressive caps.
    const QFileInfoList entries = d.entryInfoList(
        {"MasterSDR-*.log"}, QDir::Files, QDir::Time);

    const QDateTime cutoff = (cfg.retentionDays > 0)
        ? QDateTime::currentDateTime().addDays(-cfg.retentionDays)
        : QDateTime();
    const qint64 totalCap = static_cast<qint64>(cfg.retentionMaxTotalMb) * 1024 * 1024;

    qint64 cumulative = 0;
    int kept = 0;
    constexpr int kAlwaysKeep = 2;
    for (const QFileInfo& fi : entries) {
        const qint64 sz = fi.size();
        const bool tooOld = cutoff.isValid()
            && fi.lastModified().isValid()
            && fi.lastModified() < cutoff;
        const bool overSize = totalCap > 0 && (cumulative + sz) > totalCap;

        if (kept < kAlwaysKeep || (!tooOld && !overSize)) {
            cumulative += sz;
            ++kept;
            continue;
        }
        QFile::remove(fi.absoluteFilePath());
    }
}

} // namespace MasterSDR
