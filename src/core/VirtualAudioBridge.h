#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <atomic>
#include <algorithm>
#include <cstdint>

class QTimer;

namespace AetherSDR {

// Shared memory layout for one DAX audio channel.
// Used by both AetherSDR (writer) and the HAL plugin (reader).
// Sample rate matches FLEX-8600 fw v1.4.0.0 native DAX rate: 24 kHz stereo.
struct DaxShmBlock {
    std::atomic<uint32_t> writePos{0};
    std::atomic<uint32_t> readPos{0};
    uint32_t sampleRate{24000};
    uint32_t channels{2};           // stereo
    uint32_t active{0};             // 1 = AetherSDR is feeding data
    uint32_t reserved[3]{};
    // Ring buffer: 2 seconds @ 24kHz stereo = 96000 float samples
    static constexpr uint32_t RING_SIZE = 24000 * 2 * 2;
    float ringBuffer[RING_SIZE]{};
};

// Bridge between AetherSDR and the HAL plugin via POSIX shared memory.
// Creates 8 RX shared memory segments (/aethersdr-dax-1 through /aethersdr-dax-8)
// for DAX audio from radio to apps, plus 1 TX segment (/aethersdr-dax-tx)
// for audio from apps to radio.
class VirtualAudioBridge : public QObject {
    Q_OBJECT

public:
    static constexpr int NUM_CHANNELS = 8;

    explicit VirtualAudioBridge(QObject* parent = nullptr);
    ~VirtualAudioBridge() override;

    // activeChannels = how many RX shm segments to actually open (the radio's
    // slice capacity, RadioModel::maxSlices()). Clamped to [1, NUM_CHANNELS].
    // NUM_CHANNELS stays the compile-time array bound; this only bounds the
    // open loop so the device list follows the radio (#4854). Dynamic resize
    // after connect is a follow-up (issue #4935).
    bool open(int activeChannels = NUM_CHANNELS);
    void close();
    bool isOpen() const { return m_open.load(std::memory_order_acquire); }

    // DAX output gain (0.0–1.0). Default 0.25 (≈ -12 dB) to avoid
    // overloading digital-mode software like WSJT-X.
    void setGain(float g) { m_gain = std::clamp(g, 0.0f, 1.0f); }
    void setChannelGain(int channel, float g) {
        if (channel >= 1 && channel <= NUM_CHANNELS)
            m_channelGain[channel - 1].store(std::clamp(g, 0.0f, 1.0f),
                                             std::memory_order_relaxed);
    }
    void setTxGain(float g) { m_txGain = std::clamp(g, 0.0f, 1.0f); }
    float gain() const { return m_gain; }

    // Read TX audio from shared memory (apps → radio).
    // Returns float32 stereo PCM, or empty if no data available.
    QByteArray readTxAudio(int maxFrames = 480);

public slots:
    // Feed decoded DAX audio for a channel (1-8).
    // pcm format: int16 stereo, 24 kHz, little-endian.
    void feedDaxAudio(int channel, const QByteArray& pcm);

    // When transmitting, feed silence to all RX channels so apps don't starve.
    void setTransmitting(bool tx);

signals:
    void txAudioReady(const QByteArray& pcm);
    void daxRxLevel(int channel, float rms);
    void daxTxLevel(float rms);

private:
    struct RxTimingStats {
        QElapsedTimer windowElapsed;
        uint64_t writtenSamples{0};
        uint64_t trimmedSamples{0};
        uint32_t trimEvents{0};
        uint32_t overrunEvents{0};
        uint32_t peakBacklogSamples{0};
    };

    // m_open and m_channelGain are read on PanadapterStream's network thread
    // when feedDaxAudio runs via Qt::DirectConnection (the cross-platform DAX
    // RX fast path) and written from the GUI thread (open()/close(),
    // DaxApplet slider).  std::atomic gives the formal happens-before guarantee
    // that plain bool/float load/store would lack on weakly-ordered archs.
    std::atomic_bool m_open{false};
    // How many RX shm segments open() actually opened (bounded by maxSlices()).
    // Teardown loops still walk the full NUM_CHANNELS bound — closing an
    // unopened slot is a no-op. (#4854)
    int m_activeChannels{NUM_CHANNELS};
    float m_gain{0.5f};  // -6 dB default — GUI-only
    std::atomic<float> m_channelGain[NUM_CHANNELS]{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    float m_txGain{0.5f};

    // RX channels (radio → apps)
    int  m_shmFds[NUM_CHANNELS]{-1, -1, -1, -1, -1, -1, -1, -1};
    DaxShmBlock* m_blocks[NUM_CHANNELS]{};

    // TX channel (apps → radio)
    int m_txShmFd{-1};
    DaxShmBlock* m_txBlock{nullptr};
    ::QTimer* m_txPollTimer{nullptr};

    void feedSilenceToAllChannels();
    static QString shmName(int channel);
    void logRxTimingSummary(int channel, DaxShmBlock* block, RxTimingStats& stats);

    std::atomic_bool m_transmitting{false};
    QTimer*  m_silenceTimer{nullptr};
    QElapsedTimer m_silenceElapsed;   // tracks wall-clock time for accurate silence fill

    // Wall-clock timestamp (ms) of the most recent real DAX RX packet on any
    // channel.  Written lock-free from the audio fast path (feedDaxAudio,
    // potentially on PanadapterStream's network thread); read by the
    // GUI-thread silence timer to decide when to self-stop.  Replaces the
    // old in-feedDaxAudio QTimer::stop() which was unsafe to call cross-thread.
    std::atomic<qint64> m_lastAudioMs{0};

    // Best-effort diagnostics — currently only touched by feedDaxAudio /
    // logRxTimingSummary on the same thread.  If a future GUI-thread
    // inspector reads these, expect torn 64-bit values; atomic-ify then.
    RxTimingStats m_rxTiming[NUM_CHANNELS];
};

} // namespace AetherSDR
