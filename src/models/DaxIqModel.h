#pragma once

#include <QObject>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QThread>

namespace MasterSDR {

// Manages up to 4 DAX IQ channels for raw I/Q streaming to SDR apps.
// Each channel provides complex float32 data (I=left, Q=right) from the
// radio's DDC at 24/48/96/192 kHz. The radio is authoritative for all
// stream state — we send commands and reflect status.
//
// IQ data processing (byte-swap, pipe write, metering) runs on a dedicated
// worker thread to avoid blocking the main thread at high sample rates.

class DaxIqWorker;

class DaxIqModel : public QObject {
    Q_OBJECT

public:
    static constexpr int NUM_CHANNELS = 4;

    struct IqStream {
        quint32 streamId{0};
        int     channel{0};        // 1-4
        int     sampleRate{48000}; // 24000, 48000, 96000, 192000
        QString panId;
        bool    active{false};
        bool    exists{false};
    };

    explicit DaxIqModel(QObject* parent = nullptr);
    ~DaxIqModel() override;

    // Commands (send to radio via commandReady signal)
    void createStream(int channel);               // stream create type=dax_iq daxiq_channel=N
    void removeStream(int channel);               // stream remove 0x<id>
    void setSampleRate(int channel, int rate);     // stream set 0x<id> daxiq_rate=N

    // State
    const IqStream& stream(int channel) const;    // channel 1-4
    int capacity() const  { return m_capacity; }
    int available() const { return m_available; }
    void setCapacity(int cap)   { m_capacity = cap; }
    void setAvailable(int avail){ m_available = avail; }

    // Called by RadioModel when stream status arrives
    void applyStreamStatus(quint32 streamId, const QMap<QString, QString>& kvs);
    void handleStreamRemoved(quint32 streamId);

    // Feed raw IQ packet from PanadapterStream (main thread → worker thread)
    void feedRawIqPacket(int channel, const QByteArray& rawPayload, int sampleRate);

signals:
    void streamChanged(int channel);
    void commandReady(const QString& cmd);
    void iqLevelReady(int channel, float rms);

private:
    IqStream m_streams[NUM_CHANNELS];  // index 0-3 for channels 1-4
    int m_capacity{0};
    int m_available{0};

    // Worker thread for IQ processing (byte-swap, pipe write, metering)
    QThread m_workerThread;
    DaxIqWorker* m_worker{nullptr};

    int channelIndex(int channel) const { return channel - 1; }
};

// Worker object — lives on m_workerThread, handles IQ byte-swap and pipe I/O.
class DaxIqWorker : public QObject {
    Q_OBJECT

public:
    explicit DaxIqWorker(QObject* parent = nullptr);
    ~DaxIqWorker() override;

    // Pipe management (called from main thread before/after worker processes data)
    void createPipe(int channel, int sampleRate);
    void destroyPipe(int channel);

public slots:
    void processIqPacket(int channel, const QByteArray& rawPayload, int sampleRate);

signals:
    void levelReady(int channel, float rms);

private:
    int m_pipeFds[DaxIqModel::NUM_CHANNELS]{-1, -1, -1, -1};
    int m_sampleCount[DaxIqModel::NUM_CHANNELS]{};
    double m_sumSq[DaxIqModel::NUM_CHANNELS]{};
};

} // namespace MasterSDR
