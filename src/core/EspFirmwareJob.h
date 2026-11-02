#pragma once

#include "EspFirmwareManifest.h"
#include "EspFlashTarget.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

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
        // The device the operator confirmed (6-byte MAC). The job checks the
        // device it opened is that one before anything else; empty skips.
        QByteArray expectedMac;
        Action action{Action::Install};
        EspFirmwareManifest manifest;
        QHash<QString, QByteArray> files;
        QString backupPath;    // Backup, Install
        QString restoreFrom;   // Restore: a backup file, with its .json beside it
        // Install: only on a CTR2-Max its stock firmware has registered
        // (Ctr2MaxRegistration), checked before anything is written.
        bool requireCtr2MaxRegistration{false};
        // Install, Start: firmware laid out by the device's own partition
        // table (Ctr2MaxStockFirmware). The table is read first and plan()
        // supplies the manifest and files; nothing is written if it refuses.
        using Plan = std::function<bool(const QByteArray& table, quint32 flashSize,
                                        EspFirmwareManifest* manifest,
                                        QHash<QString, QByteArray>* files, QString* problem)>;
        Plan plan;
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

    // Opens the device on a port; by default an EspFlasher (tests: a flash
    // in memory). Called on the job's thread.
    // accessDenied: set when the OS refused to open the port (Linux: no
    // access to the serial device), as opposed to any other failure.
    using TargetOpener = std::function<std::unique_ptr<EspFlashTarget>(
        const QString& portName, QString* problem, bool* accessDenied)>;
    void setTargetOpener(TargetOpener opener) { m_opener = std::move(opener); }
    // The device is left as the current step leaves it; a restore or install
    // stopped part way leaves its region incomplete.
    void cancel();
    bool isRunning() const { return m_thread != nullptr; }
    // The last job failed because the OS refused to open the port.
    bool accessDenied() const { return m_accessDenied; }

signals:
    void connected(const QString& chip, const QByteArray& mac, quint32 flashSize);
    void progress(const QString& step, qint64 done, qint64 total);
    void finished(bool ok, const QString& message);

private:
    void run(const Request& request);

    TargetOpener m_opener;
    QThread* m_thread{nullptr};
    std::atomic<bool> m_cancel{false};
    bool m_ok{false};        // written by the job's thread before it finishes
    bool m_accessDenied{false};
    QString m_message;
};

} // namespace AetherSDR
