#pragma once

#include "EspFirmwareManifest.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>

class QThread;

namespace AetherSDR {

// One session with an ESP32 device in download mode, on a thread of its
// own (a whole-flash backup takes about a minute): connect, do one action,
// restart the device. Signals arrive on the thread that owns the job.
class EspFirmwareJob : public QObject {
    Q_OBJECT

public:
    enum class Action {
        Backup,    // the whole flash to backupPath
        Install,   // backup to backupPath first (if given), then the manifest's images
        Start,     // select the manifest's firmware to start
        Restore,   // a whole-flash backup (restoreFrom) back onto the device
    };
    struct Request {
        QString portName;
        Action action{Action::Install};
        EspFirmwareManifest manifest;
        QHash<QString, QByteArray> files;
        QString backupPath;    // Backup, Install
        QString restoreFrom;   // Restore: a backup file, with its .json beside it
        // Install: only on a CTR2-Max its stock firmware has registered
        // (Ctr2MaxRegistration), checked before anything is written.
        bool requireCtr2MaxRegistration{false};
    };

    // A backup's description, kept beside it as <backup>.json, so a restore
    // goes back onto the device it came from.
    struct BackupInfo {
        QString chip;
        QByteArray mac;        // 6 bytes
        quint32 flashSize{0};
        QByteArray sha256;
    };
    static bool readBackupInfo(const QString& backupPath, BackupInfo* info, QString* problem);

    explicit EspFirmwareJob(QObject* parent = nullptr);
    ~EspFirmwareJob() override;

    // False if a job is already running.
    bool start(const Request& request);
    // The device is left as the current step leaves it; a restore or install
    // stopped part way leaves its region incomplete.
    void cancel();
    bool isRunning() const { return m_thread != nullptr; }

signals:
    void connected(const QString& chip, const QByteArray& mac, quint32 flashSize);
    void progress(const QString& step, qint64 done, qint64 total);
    void finished(bool ok, const QString& message);

private:
    void run(const Request& request);

    QThread* m_thread{nullptr};
    std::atomic<bool> m_cancel{false};
    bool m_ok{false};        // written by the job's thread before it finishes
    QString m_message;
};

} // namespace AetherSDR
