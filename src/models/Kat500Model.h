#pragma once
#include "Kat500Types.h"
#include <QObject>
#include <QElapsedTimer>
#include <QQueue>
#include <QTimer>
#include <QStringList>
#include <QPair>
#include <functional>
#include <memory>

class QTcpSocket;
class QSerialPort;
namespace AetherSDR::Kat500 { class FrameParser; }
namespace AetherSDR {

// GUI-facing peripheral facade. Device values are readbacks, never a reconnect cache.
class Kat500Model : public QObject {
    Q_OBJECT
public:
    explicit Kat500Model(QObject* parent = nullptr);
    ~Kat500Model() override;
    void connectToHost(const QString& host, quint16 port);
    void connectSerial(const QString& portName, int baudRate = 38400);
    void disconnectDevice();
    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }
    bool isConnected() const { return m_connected; }
    bool isConnecting() const { return m_connecting; }
    bool isDetected() const { return m_detected; }
    bool isResponding() const;
    bool ampKeyBlockConfirmed() const;
    bool isNetworkConnection() const { return m_network; }
    QString description() const;
    const Kat500::Status& status() const { return m_status; }
    qint64 fieldAgeMs(const QString& command) const;
    QString unavailableReason(bool writing = false) const;
    bool operationBusy() const { return !m_operation.isEmpty(); }
    QString operationName() const { return m_operation; }

    bool setPower(bool on);
    bool setMode(Kat500::Mode mode);
    bool setAntenna(int antenna);
    bool setCapacitorMask(int mask);
    bool setInductorMask(int mask);
    bool setCapacitorSide(Kat500::CapacitorSide side);
    bool setBypassed(bool bypassed);
    bool setAttenuator(bool enabled);
    bool setAmpKeyInterrupted(bool interrupted);
    bool clearFault();
    // Explicit operator intent only. No command here keys a radio/carrier.
    bool startTune(bool saveMatch = true, std::function<bool()> permit = {});
    bool cancelTune();
    // Retain stop intent if its controller goes away while a serial request drains.
    void cancelTuneAfterPendingOperation();
    bool readMatchStatus();
    bool memorize(int frequencyKhz = 0);
    bool recallFrequency(int frequencyKhz);
    void setFollowTxFrequency(bool enabled);
    bool followTxFrequency() const { return m_followTx; }
    void setEffectiveTxFrequencyHz(qint64 frequencyHz);

    const Kat500::Configuration& configuration() const { return m_configuration; }
    bool readConfiguration();
    bool applyConfiguration(const Kat500::Configuration& changedFields);
    bool optimizeForKpa500();
    bool readBackup();
    bool restoreBackup(const Kat500::Backup& backup, bool confirmed);
    const Kat500::Backup& backup() const { return m_backup; }
    static QByteArray encodeBackupFile(const Kat500::Backup& backup);
    static std::optional<Kat500::Backup> decodeBackupFile(const QByteArray& data, QString* error = nullptr);
    static bool saveBackupFile(const QString& path, const Kat500::Backup& backup, QString* error = nullptr);
    bool readFaultHistory();
    const QList<Kat500::FaultRecord>& faults() const { return m_faults; }
    static QByteArray faultsCsv(const QList<Kat500::FaultRecord>& faults);
    bool inspectMemories(int frequencyKhz = 0);
    const Kat500::MemoryBin& memories() const { return m_memories; }
    // band -1 expands all eleven documented bands; antenna 0 means all antennas.
    bool eraseMemories(int band, int antenna, bool confirmed);
    bool factoryReset(bool confirmed);
    void cancelOperation();
    void setVerbose(bool verbose) { m_verbose = verbose; }
    bool verbose() const { return m_verbose; }
    QString debugText() const { return m_debug.join(QLatin1Char('\n')); }
    void clearDebug() { m_debug.clear(); emit debugChanged(); }

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& reason);
    void detectedChanged(bool detected);
    void statusUpdated(const AetherSDR::Kat500::Status& status);
    void availabilityChanged();
    void diagnostic(const QString& line);
    void debugChanged();
    void telemetryReceived(const QString& command, const AetherSDR::Kat500::Status& status);
    void operationBusyChanged(bool busy);
    void operationProgress(int completed, int total);
    void operationFinished(const QString& message);
    void operationFailed(const QString& reason);
    void configurationRead(const AetherSDR::Kat500::Configuration& configuration);
    void backupRead(const AetherSDR::Kat500::Backup& backup);
    void faultHistoryRead(const QList<AetherSDR::Kat500::FaultRecord>& records);
    void memoriesRead(const AetherSDR::Kat500::MemoryBin& bin);

private:
    friend class Kat500ModelTest;
    struct Request {
        QByteArray wire;
        std::function<bool(const QByteArray&)> matches;
        std::function<void(const QByteArray&)> complete;
        int timeoutMs = 2000;
        bool mutation = false;
        bool requiresIdle = true;
    };
    void transportOpened();
    void transportClosed(const QString& reason = {});
    void receiveBytes(const QByteArray& bytes);
    void receiveFrame(const QByteArray& frame);
    void tick();
    bool expireActiveRequest();
    bool write(const QByteArray& bytes);
    void query(const QByteArray& wire, std::function<void()> complete = {});
    void enqueue(Request request);
    bool begin(const QString& name, bool writing);
    void finish(const QString& message);
    void fail(const QString& reason);
    void report(const QString& text, bool polling = false);
    bool requestValue(const QByteArray& setter, const QByteArray& getter, std::function<bool(const Kat500::Status&)> verify,
                      const QString& name, bool allowOff = false);
    void preflight(std::function<void()> ready);
    void readRegion(bool external, int address, int end, std::function<void(QByteArray)> complete);
    void readRegionBlock();
    void restoreBlock();
    void restoreNextRegion();
    void applyNextSetting();
    void eraseNextBand();
    void queueIdentity();
    void pollTuneCancellation();
    void readTuneIdle();
    void confirmRequestedValue();
    void pollRequestedValue();
    bool validateMemoryFrequency(int frequencyKhz, bool allowCurrent = false);
    void requestMemory(int frequencyKhz, const QByteArray& prefix = {});
    void reconnect();

    std::unique_ptr<Kat500::FrameParser> m_parser;
    std::unique_ptr<QTcpSocket> m_socket;
#ifdef HAVE_SERIALPORT
    std::unique_ptr<QSerialPort> m_serial;
#endif
    QElapsedTimer m_clock;
    QTimer m_timer;
    QTimer m_reconnectTimer;
    std::function<qint64()> m_now;
    std::function<bool(const QByteArray&)> m_write;
    QQueue<Request> m_queue;
    std::optional<Request> m_active;
    quint64 m_generation = 0;
    qint64 m_resetAt = 0;
    quint64 m_resetGeneration = 0;
    qint64 m_sentAt = 0;
    qint64 m_lastIo = -10000;
    qint64 m_lastReply = -10000;
    qint64 m_nextPoll = 0;
    qint64 m_quarantineUntil = 0;
    int m_pollIndex = 0;
    int m_wakeCount = 0;
    bool m_waking = false;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_detected = false;
    bool m_network = true;
    bool m_autoReconnect = false;
    bool m_deliberateDisconnect = false;
    bool m_verbose = false;
    bool m_followTx = false;
    std::optional<int> m_followIntentKhz;
    bool m_followIntentAdmitted = false;
    bool m_dropMemory = false;
    QString m_host;
    QString m_portName;
    quint16 m_port = 0;
    int m_baud = 38400;
    qint64 m_connectStarted = 0;
    QStringList m_debug;
    Kat500::Status m_status;
    QMap<QString,qint64> m_received;
    QMap<QString,QByteArray> m_lastStateFrames;
    QString m_operation;
    QString m_operationSerial;
    QString m_operationFirmware;
    bool m_operationWriting = false;
    int m_progress = 0;
    int m_total = 0;
    Kat500::Configuration m_configuration;
    QQueue<QPair<Kat500::SettingKey,double>> m_edits;
    Kat500::Backup m_backup;
    Kat500::Backup m_pendingBackup;
    Kat500::Backup m_restore;
    QList<Kat500::FaultRecord> m_faults;
    Kat500::MemoryBin m_memories;
    QList<QByteArray> m_memoryFrames;
    bool m_memoryReading = false;
    std::optional<Kat500::MemoryMatch> m_memoryExpected;
    QByteArray m_settleGetter;
    std::function<bool(const Kat500::Status&)> m_settleVerify;
    qint64 m_settleDeadline = 0;
    qint64 m_settleNext = 0;
    quint64 m_settleGeneration = 0;
    bool m_settleAllowOff = false;
    qint64 m_cancelTuneDeadline = 0;
    qint64 m_tuneResultDeadline = 0;
    bool m_deferredTuneCancel = false;
    bool m_tuneCompleted = false;
    std::function<bool()> m_tunePermit;
    bool m_regionExternal = false;
    int m_regionAddress = 0;
    int m_regionEnd = 0;
    QByteArray m_regionData;
    std::function<void(QByteArray)> m_regionComplete;
    bool m_restoreExternal = false;
    int m_restoreAddress = 0;
    QQueue<int> m_eraseBands;
    int m_eraseAntenna = 0;
};
} // namespace AetherSDR
