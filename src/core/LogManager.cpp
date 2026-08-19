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

namespace AetherSDR {

// Define all logging categories (disabled by default)
Q_LOGGING_CATEGORY(lcDiscovery,  "aether.discovery",  QtDebugMsg)
Q_LOGGING_CATEGORY(lcConnection, "aether.connection",  QtDebugMsg)
Q_LOGGING_CATEGORY(lcProtocol,   "aether.protocol",    QtDebugMsg)
Q_LOGGING_CATEGORY(lcAudio,      "aether.audio",       QtWarningMsg)
Q_LOGGING_CATEGORY(lcAudioSummary, "aether.audio.summary", QtInfoMsg)
Q_LOGGING_CATEGORY(lcVita49,     "aether.vita49",      QtWarningMsg)
Q_LOGGING_CATEGORY(lcDsp,        "aether.dsp",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcRade,       "aether.rade",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcSmartLink,  "aether.smartlink",   QtWarningMsg)
Q_LOGGING_CATEGORY(lcCat,        "aether.cat",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDax,        "aether.dax",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcMeters,     "aether.meters",      QtWarningMsg)
Q_LOGGING_CATEGORY(lcTransmit,   "aether.transmit",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcFirmware,   "aether.firmware",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcTuner,      "aether.tuner",       QtWarningMsg)
Q_LOGGING_CATEGORY(lcGui,        "aether.gui",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDxCluster,  "aether.dxcluster",   QtWarningMsg)
Q_LOGGING_CATEGORY(lcMqtt,       "aether.mqtt",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcRbn,        "aether.rbn",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcDevices,    "aether.devices",     QtWarningMsg)
Q_LOGGING_CATEGORY(lcPerf,       "aether.perf",        QtWarningMsg)
Q_LOGGING_CATEGORY(lcCw,         "aether.cw",          QtWarningMsg)
Q_LOGGING_CATEGORY(lcSHistory,  "aether.shistory",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcAx25,       "aether.ax25",        QtWarningMsg)
// Info by default, like aether.hl2 and for the same reason: this is the only
// record of what a connected-mode link actually did — measured round-trip times
// against the configured T1, retransmit ratios, why a session ended. An HF link
// failure is not reproducible on demand, so the evidence has to already be in a
// support log that was captured without foreknowledge. Rates are low (a handful
// of lines per session), so Info costs nothing.
Q_LOGGING_CATEGORY(lcAx25Link,   "aether.ax25.link",   QtInfoMsg)
Q_LOGGING_CATEGORY(lcWaveform,   "aether.waveform",    QtWarningMsg)
Q_LOGGING_CATEGORY(lcKiwiSdr,    "aether.kiwisdr",     QtDebugMsg)
Q_LOGGING_CATEGORY(lcKiwiSdrAudio, "aether.kiwisdr.audio", QtWarningMsg)
Q_LOGGING_CATEGORY(lcAutomation, "aether.automation",  QtInfoMsg)
Q_LOGGING_CATEGORY(lcQrz,        "aether.qrz",         QtWarningMsg)
Q_LOGGING_CATEGORY(lcClock,      "aether.clock",       QtWarningMsg)
// Info by default: the band-filter transitions this carries are low-rate and
// are the only record of what the companion filter board was told to do, so
// they have to be in a support log that was captured without foreknowledge.
Q_LOGGING_CATEGORY(lcHl2,        "aether.hl2",         QtInfoMsg)

LogManager::LogManager()
{
    // Register categories with human-readable labels and descriptions
    m_categories = {
        {"aether.discovery",  "Discovery",    "UDP radio discovery broadcasts"},
        {"aether.connection", "Connection / Commands", "Raw TCP command channel lines: TX commands, RX responses, and socket state"},
        {"aether.protocol",   "Protocol / Status",     "Parsed SmartSDR protocol handling and model status updates"},
        {"aether.audio",      "Audio",        "RX/TX audio, device negotiation, volume"},
        {"aether.audio.summary", "Audio Summary", "Default support log summaries for audio routing and sink/source negotiation"},
        {"aether.vita49",     "VITA-49",      "UDP packet routing: FFT, waterfall, meters, DAX"},
        {"aether.dsp",        "DSP",          "NR2, RN2, CW decoder processing"},
        {"aether.rade",       "RADE",         "FreeDV Radio Autoencoder digital voice"},
        {"aether.smartlink",  "SmartLink",    "Auth0 login, TLS tunnel, WAN streaming"},
        // Label leads with TCI because TciServer.cpp owns the large majority of
        // this category's call sites — 53 of 76 as of #4750, with CatPort.cpp
        // next at 15 and the rest scattered. The old "CAT/rigctld" label named
        // only the minority user, so someone chasing a TCI problem scanned the
        // checkbox list, saw nothing about TCI, and concluded AetherSDR had no
        // TCI logging to turn on. It has more than any other rig-control
        // surface; it was just filed under another name. Deliberately NOT split
        // into a new aether.tci category: the id is what filter rules and saved
        // settings key off, so renaming it would silently drop every user's
        // existing preference for this category, and the two surfaces genuinely
        // share the rig-control plumbing that the qCInfo lines report on.
        // The description names the TX-summary fields (blocks/peak/rms/clips)
        // because those are the tokens an operator greps a support log for.
        {"aether.cat",        "TCI / CAT / rigctld",  "TCI server (slice and DAX arming, TX audio summary: blocks, peak, rms, clips), rigctld TCP servers, PTY virtual serial ports"},
        {"aether.dax",        "DAX",          "Virtual audio bridge (PipeWire/CoreAudio)"},
        {"aether.meters",     "Meters",       "Meter definitions and value conversion"},
        {"aether.transmit",   "Transmit",     "TX state, ATU, profiles, power control"},
        {"aether.firmware",   "Firmware",     "Firmware download, staging, upload"},
        {"aether.tuner",      "Tuner/AGM",    "TGXL tuner, Antenna Genius state"},
        {"aether.gui",        "GUI",          "Window, applets, dialogs"},
        {"aether.dxcluster",  "DX Cluster",   "DX cluster telnet connection and spot parsing"},
        {"aether.mqtt",       "MQTT",         "MQTT telemetry client connection and messages"},
        {"aether.rbn",        "RBN",          "Reverse Beacon Network connection and spots"},
        {"aether.devices",    "Ext Devices",  "Serial port, FlexControl, MIDI, HID encoder"},
        {"aether.perf",       "Performance",  "Render timing and CPU profiling data"},
        {"aether.propforecast", "Propagation",  "Solar and propagation forecast updates"},
        {"aether.cw",         "CW / netCW",    "CW keying, MIDI paddle, iambic, and netCW timing"},
        {"aether.shistory",   "S History",     "Past-Signals voice detection: noise floor, region width, band-plan filter"},
        {"aether.ax25",       "AetherModem", "AX.25 modem lifecycle, RX/TX audio, demod, framing, and packet diagnostics"},
        {"aether.ax25.link",  "AX.25 Link",  "Connected-mode data link: session open/close, measured round-trip vs configured T1, retransmits, idle-link polls"},
        {"aether.waveform",   "Waveform",    "Docker waveform image install upload and local waveform helper lifecycle"},
        {"aether.kiwisdr",    "KiwiSDR",     "KiwiSDR remote RX antennas: connect, handshake, audio/waterfall negotiation, reconnect, profile lifecycle"},
        {"aether.kiwisdr.audio", "KiwiSDR Audio/DSP", "Verbose KiwiSDR receive audio: frame decode, resampler, jitter/FIFO under/overrun, mixing (high-rate; off by default)"},
        {"aether.automation", "Automation Bridge", "Agent-drivable test bridge (#3646): QLocalServer verbs, widget snapshots, captures (AETHER_AUTOMATION only)"},
        {"aether.qrz",        "QRZ Lookup",   "QRZ.com callsign lookups: session, cache, CW callsign spotting, photos"},
        {"aether.clock",      "AetherClock",  "WWV/WWVB time-signal decoder: state transitions, per-second alignment, frame decodes, voter verdicts"},
        {"aether.hl2",        "Hermes-Lite 2", "HL2 backend: band changes, J16 companion-filter selection, LNA gain, and radio health telemetry"},
        // Registered so the TRANSMIT telemetry is reachable at all. The category
        // is declared locally in Hl2Backend.cpp and was never listed here, so
        // applyFilterRules()'s blanket "aether.*.debug=false" switched it off and
        // no UI toggle could switch it back on. What that hid is the TX IQ FIFO
        // depth plus its underflow/overflow flags — which the backend's own
        // comment calls "the most important number in the protocol" and "what
        // distinguishes 'the audio is wrong' from 'the audio never arrived'".
        // An underflow is exactly what a mid-transmission click sounds like,
        // and it was undiagnosable from a log file.
        // SEPARATE TOGGLE from "Hermes-Lite 2", deliberately: the FIFO depth is
        // sampled per telemetry frame, so this is high-rate next to the rest of
        // the HL2 category and is not something to leave on by default. Said
        // explicitly in the description because the two labels otherwise read as
        // one control, and someone chasing transmit telemetry will tick the
        // wrong box and conclude the logging is still broken.
        {"aether.hl2.tx",     "Hermes-Lite 2 TX", "HL2 transmit telemetry: TX IQ FIFO depth with underflow/overflow flags, and forward/reflected power counts. Separate toggle — ticking \"Hermes-Lite 2\" does NOT enable this (high-rate)"},
        // ICOM — the same "declared locally, never registered" hole the HL2
        // categories above had. All five were unreachable: applyFilterRules()'s
        // blanket `aether.*.debug=false` switched them off and no UI toggle
        // could switch them back on, so an Icom session logged nothing beyond
        // its INF lines. Chasing a mode-reporting bug on a live IC-9700 that
        // way means guessing from published state instead of reading frames.
        {"aether.icom.session", "Icom Session",  "Icom RS-BA1 session: handshake, token/auth, capabilities, keepalive"},
        {"aether.icom.stream",  "Icom Streams",  "Icom UDP stream lifecycle: control/serial/audio handshakes and ports"},
        {"aether.icom.civ",     "Icom CI-V",     "Every CI-V frame in and out, decoded — command, subcommand and payload bytes. The only way to tell 'the radio never sent it' from 'we sent it and dropped the reply' (high-rate)"},
        {"aether.icom.pan",     "Icom Scope",    "Icom spectrum scope: sweep frames, division reassembly, bounds"},
        {"aether.icom.link",    "Icom Link",     "Icom backend link state: connect/disconnect, model resolution, capability publication"},
        {"aether.icom.cred",    "Icom Credentials", "Icom credential storage and retrieval (no secret values are logged)"},
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

    // NOT IN THE CURATED LIST — register it and honour the request anyway.
    //
    // The list above is the set worth OFFERING in the UI, and it was silently
    // doubling as the set that could be TOGGLED AT ALL: a category it did not
    // name accepted `setEnabled` and did nothing, while the verb that called it
    // reported ok. `log set aether.icom.stream on` answered
    // `{ok: true, enabled: false}`, so the only way to see wire traffic was to
    // relaunch with QT_LOGGING_RULES — and on a single-client radio every
    // relaunch costs a session timeout.
    //
    // Refusing loudly would be defensible; accepting is better. Qt categories
    // are created by any translation unit that declares one, so no central list
    // can be complete, and a diagnostic category nobody thought to curate is
    // exactly the one you need at 2am.
    if (!on)
        return;   // nothing to turn off, and no reason to accrue an entry

    // VALIDATE BEFORE IT REACHES THE FILTER RULES (Principle VII).
    // applyFilterRules() splices this id into a rule string and hands the whole
    // thing to QLoggingCategory::setFilterRules(), so an id carrying '=' or a
    // newline injects rules of its own — and the bridge's `log set <id> on` is a
    // reachable caller. The cap stops a scripted caller growing the PERSISTED
    // list without bound, since every distinct id appends an entry and
    // saveSettings() writes it.
    static const QRegularExpression kCategoryName(
        QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$"));
    if (!kCategoryName.match(id).hasMatch() || m_categories.size() >= kMaxCategories) {
        qWarning() << "LogManager: refusing to register category" << id;
        return;
    }
    m_categories.append({id, id, QStringLiteral("Registered on demand")});
    m_categories.last().enabled = true;
    applyFilterRules();
    saveSettings();
    emit categoryChanged(id, true);
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
    // Default: all aether.* debug messages off, then enable selected ones
    QStringList rules;
    rules << "aether.*.debug=false";
    rules << "aether.audio.summary.info=true";
    for (const auto& c : m_categories) {
        if (c.enabled) {
            rules << QString("%1.debug=true").arg(c.id);
            // …and info, which is NOT implied by enabling debug.
            //
            // Qt filter rules are per-LEVEL, not a threshold: "x.debug=true"
            // turns on debug and leaves every other level at the category's
            // declared default. Most categories here are declared
            // Q_LOGGING_CATEGORY(…, QtWarningMsg), so their info tier was off
            // and no UI toggle could reach it — ticking the category produced
            // its DEBUG messages while silently withholding its INFO ones,
            // which are the more important half.
            //
            // Found while trying to diagnose transmit clicking from a user log:
            // TciServer::logTxAudioSummary() is a qCInfo(lcCat) carrying
            // blocks / requested48k / effective48k / peak / rms / clips — the
            // numbers that answer "is the TX feed underrunning?" — and it had
            // never once appeared in a log file. Neither had the 16 qCInfo
            // calls on lcDax. Two dozen categories are declared QtWarningMsg,
            // so this was most of the codebase's INFO logging.
            //
            // Note the asymmetry: no blanket "aether.*.info=false" is emitted
            // above — it would newly silence the QtDebugMsg/QtInfoMsg-declared
            // categories (aether.kiwisdr, aether.connection, aether.hl2, …)
            // whose Info is visible today. For those, this toggle governs
            // Debug only; their Info stays on regardless.
            rules << QString("%1.info=true").arg(c.id);
        }
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
    // re-points the aethersdr.log symlink so the Support dialog and
    // support-bundle scan continue to find the live file. Writer hands us
    // the closed file via currentPath; we never touch the writer's file
    // handle here. (#2498)
    m_writer.setRotationConfig(maxBytes,
        [this](const QString& currentPath) -> QString {
            const QString dir = QFileInfo(currentPath).absolutePath();
            const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
            QString candidate = dir + "/aethersdr-" + ts + ".log";
            int suffix = 1;
            while (QFile::exists(candidate) && suffix <= 100) {
                candidate = dir + QString("/aethersdr-%1-%2.log").arg(ts).arg(suffix++);
            }
            if (QFile::exists(candidate))
                return {};

            setActiveLogFilePath(candidate);

            const QString symlink = dir + "/aethersdr.log";
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

    // Fan out to diagnostic taps (automation event channel). Held under the
    // tap mutex; taps are documented as cheap and non-logging, so there is no
    // re-entrancy. The hash is empty in normal runs, so this is near-free.
    {
        QMutexLocker lk(&m_tapMutex);
        for (const auto& tap : m_taps)
            tap(type, category, msg);
    }

    if (type == QtFatalMsg)
        m_writer.flush();
}

int LogManager::addTap(LogTap tap)
{
    QMutexLocker lk(&m_tapMutex);
    const int id = m_nextTapId++;
    m_taps.insert(id, std::move(tap));
    return id;
}

void LogManager::removeTap(int id)
{
    QMutexLocker lk(&m_tapMutex);
    m_taps.remove(id);
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
           + "/AetherSDR/logs/aethersdr.log";
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
        "aether.discovery", "aether.connection", "aether.protocol",
        "aether.audio.summary", "aether.kiwisdr"
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
        {"aethersdr-*.log"}, QDir::Files, QDir::Time);

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

} // namespace AetherSDR
