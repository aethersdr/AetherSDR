#pragma once

#include "AsyncLogWriter.h"

#include <QObject>
#include <QList>
#include <QHash>
#include <QString>
#include <QLoggingCategory>
#include <QMutex>
#include <QtCore/qlogging.h>

#include <functional>

namespace AetherSDR {

// Declares all logging categories used throughout AetherSDR.
// Include this header and use qCDebug(lcXxx) / qCWarning(lcXxx) / qCInfo(lcXxx).

Q_DECLARE_LOGGING_CATEGORY(lcDiscovery)
Q_DECLARE_LOGGING_CATEGORY(lcConnection)
Q_DECLARE_LOGGING_CATEGORY(lcProtocol)
Q_DECLARE_LOGGING_CATEGORY(lcAudio)
Q_DECLARE_LOGGING_CATEGORY(lcAudioSummary)
Q_DECLARE_LOGGING_CATEGORY(lcVita49)
Q_DECLARE_LOGGING_CATEGORY(lcDsp)
Q_DECLARE_LOGGING_CATEGORY(lcRade)
Q_DECLARE_LOGGING_CATEGORY(lcSmartLink)
Q_DECLARE_LOGGING_CATEGORY(lcCat)
Q_DECLARE_LOGGING_CATEGORY(lcDax)
Q_DECLARE_LOGGING_CATEGORY(lcMeters)
Q_DECLARE_LOGGING_CATEGORY(lcTransmit)
Q_DECLARE_LOGGING_CATEGORY(lcFirmware)
Q_DECLARE_LOGGING_CATEGORY(lcTuner)
Q_DECLARE_LOGGING_CATEGORY(lcGui)
Q_DECLARE_LOGGING_CATEGORY(lcDxCluster)
Q_DECLARE_LOGGING_CATEGORY(lcMqtt)
Q_DECLARE_LOGGING_CATEGORY(lcRbn)
Q_DECLARE_LOGGING_CATEGORY(lcDevices)
Q_DECLARE_LOGGING_CATEGORY(lcPerf)
Q_DECLARE_LOGGING_CATEGORY(lcCw)
Q_DECLARE_LOGGING_CATEGORY(lcSHistory)
Q_DECLARE_LOGGING_CATEGORY(lcAx25)
Q_DECLARE_LOGGING_CATEGORY(lcWaveform)
Q_DECLARE_LOGGING_CATEGORY(lcKiwiSdr)
Q_DECLARE_LOGGING_CATEGORY(lcKiwiSdrAudio)
Q_DECLARE_LOGGING_CATEGORY(lcAutomation)
Q_DECLARE_LOGGING_CATEGORY(lcQrz)
Q_DECLARE_LOGGING_CATEGORY(lcClock)

// Central registry for toggling per-module diagnostic logging at runtime.
// The Support dialog (Help → Support) uses this to let users enable/disable
// logging for individual modules without recompiling.
//
// Default state: all debug logging DISABLED. Warnings/criticals always pass.
// When a category is enabled, qCDebug messages for that module flow to the
// log file and stderr.

class LogManager : public QObject {
    Q_OBJECT

public:
    struct Category {
        QString id;           // e.g. "aether.connection"
        QString label;        // e.g. "Connection"
        QString description;  // e.g. "TCP command channel, protocol parsing"
        bool enabled{false};
    };

    static LogManager& instance();

    QList<Category> categories() const { return m_categories; }
    bool isEnabled(const QString& id) const;
    void setEnabled(const QString& id, bool on);
    void setAllEnabled(bool on);

    // Log file management
    bool startLogging(const QString& path, bool mirrorToStderr);
    void shutdownLogging();
    void enqueueMessage(QtMsgType type, const QMessageLogContext& ctx, const QString& msg);
    void flushLog() const;

    // Diagnostic tap (#3646 observability). Invoked synchronously, on the
    // logging thread, for every enqueued message — after it is handed to the
    // writer. The automation bridge registers one to expose a high-resolution,
    // monotonic-timestamped event stream that mirrors the log file. Taps must
    // be cheap and MUST NOT log (re-entrancy would deadlock the tap mutex).
    using LogTap = std::function<void(QtMsgType type, const QString& category,
                                      const QString& message)>;
    int  addTap(LogTap tap);
    void removeTap(int id);
    AsyncLogWriter::Counters logCounters() const;

    QString logFilePath() const;
    void setActiveLogFilePath(const QString& path);
    qint64 logFileSize() const;
    void clearLog();

    // Bounded historical footprint: delete timestamped logs in dir that
    // are older than retention.days, then drop oldest-first until total
    // size <= retention.maxTotalMb. Reads AppSettings["LogRetention"]
    // (see RetentionConfig). Call once at startup before startLogging.
    void pruneOldLogs(const QString& dir);

    struct RetentionConfig {
        int activeLogMaxMb{100};
        int retentionDays{7};
        int retentionMaxTotalMb{500};
    };
    RetentionConfig retentionConfig() const;

    // Persist toggle state to AppSettings
    void saveSettings();
    void loadSettings();

signals:
    void categoryChanged(const QString& id, bool enabled);

private:
    LogManager();
    void applyFilterRules();

    QList<Category> m_categories;
    mutable QMutex m_pathMutex;
    QString m_activeLogFilePath;
    mutable AsyncLogWriter m_writer;

    mutable QMutex m_tapMutex;
    QHash<int, LogTap> m_taps;
    int m_nextTapId{1};
};

} // namespace AetherSDR
