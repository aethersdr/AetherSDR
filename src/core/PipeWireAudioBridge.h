#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <atomic>
#include <array>
#include <memory>

class QTimer;
class QFile;

namespace AetherSDR {

#ifdef HAVE_PIPEWIRE_NATIVE
class PipeWireNativeRxSource;
#endif

// DAX virtual audio bridge for Linux using PulseAudio pipe modules.
// Creates named pipes in /tmp and loads module-pipe-source (RX, up to
// NUM_CHANNELS — open() opens only maxSlices() of them, following the radio)
// and module-pipe-sink (TX, 1 channel) so that apps like WSJT-X, VARA, fldigi
// see "AetherSDR DAX 1-8" as audio input and "AetherSDR TX" as audio output.
// Works with both PulseAudio and PipeWire (via pipewire-pulse).
class PipeWireAudioBridge : public QObject {
    Q_OBJECT

public:
    static constexpr int NUM_CHANNELS    = 8;
    static constexpr int RADIO_RATE      = 24000;  // radio-native DAX rate
    static constexpr int PIPE_RATE       = 48000;  // matches PipeWire graph rate — avoids in-graph resampler
    static constexpr int PIPE_CHANNELS   = 1;      // mono — ham radio DAX is single-channel
    static constexpr int PIPE_KERNEL_BUF = 4096;   // ~21 ms at 48 kHz mono float32 — bounds kernel FIFO buffering
    static constexpr int TX_RATE         = 24000;  // TX sink stays at radio-native rate (s16le mono)
    static constexpr int TX_CHANNELS     = 1;

    explicit PipeWireAudioBridge(QObject* parent = nullptr);
    ~PipeWireAudioBridge() override;

    // activeChannels = how many RX sources to actually open (the radio's
    // slice capacity, RadioModel::maxSlices()). Clamped to [1, NUM_CHANNELS].
    // NUM_CHANNELS stays the compile-time array bound; this only bounds the
    // open loop so the device list follows the radio (#4854). Dynamic resize
    // after connect is a follow-up (issue #4935).
    bool open(int activeChannels = NUM_CHANNELS);
    void close();
    bool isOpen() const { return m_open; }

    void setGain(float g);                     // global gain (all RX channels)
    void setChannelGain(int channel, float g);  // per-channel RX gain (1-4)
    void setTxGain(float g);                    // TX gain
    float gain() const { return m_gain; }

public slots:
    void feedDaxAudio(int channel, const QByteArray& pcm);
    void setTransmitting(bool tx);

signals:
    void txAudioReady(const QByteArray& pcm);
    void daxRxLevel(int channel, float rms);  // 0.0–1.0 RMS for meter display
    void daxTxLevel(float rms);

private:
    bool loadPipeSource(int index);
    bool loadPipeSink();
    void unloadModules();

    // RX: we write int16 mono PCM to these pipes → PulseAudio reads them
    struct RxPipe {
        int fd{-1};
        uint32_t moduleIndex{0};
        QString pipePath;
    };
    std::array<RxPipe, NUM_CHANNELS> m_rx;

    // TX: PulseAudio writes to this pipe → we read from it
    struct TxPipe {
        int fd{-1};
        uint32_t moduleIndex{0};
        QString pipePath;
    };
    TxPipe m_tx;

    QTimer* m_txReadTimer{nullptr};
    void readTxPipe();

    // Silence fill during TX — keeps pipe-source clock advancing so
    // WSJT-X / VARA don't see a stalled audio source.
    QTimer*       m_silenceTimer{nullptr};
    QElapsedTimer m_silenceElapsed;
    void feedSilenceToAllPipes();

    std::atomic_bool m_open{false};
    // How many RX sources open() actually opened (bounded by maxSlices()).
    // Teardown loops still walk the full NUM_CHANNELS bound — closing an
    // unopened slot is a no-op. (#4854)
    int m_activeChannels{NUM_CHANNELS};
    float m_gain{0.5f};
    // m_channelGain is read on PanadapterStream's network thread (DirectConnection
    // fast path) and written from the main thread (DaxApplet slider).  Float
    // load/store is naturally atomic on aligned x86_64 / aarch64, but use
    // std::atomic for the formal happens-before guarantee.
    std::atomic<float> m_channelGain[NUM_CHANNELS]{0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    float m_txGain{0.5f};
    std::atomic_bool m_transmitting{false};

    // Per-channel state for 24 kHz → 48 kHz linear-interp upsampler.
    // Holds the last input sample so the first interpolated output of the
    // next packet has a correct neighbor instead of starting from zero.
    // Only ever touched in feedDaxAudio (single thread, per-channel) — no atomic needed.
    float m_rxLastSample[NUM_CHANNELS]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // Wall-clock timestamp (ms) of the most recent real audio packet for any
    // channel.  Updated lock-free from the audio fast path; read by the
    // main-thread silence timer to decide when to stop itself.  Replaces the
    // old in-feedDaxAudio QTimer::stop() which was unsafe to call cross-thread.
    std::atomic<qint64> m_lastAudioMs{0};

#ifdef HAVE_PIPEWIRE_NATIVE
    // Native pw_stream-based RX sources, one per channel.  When any of these
    // are non-null, feedDaxAudio() routes that channel's audio through the
    // native source instead of the legacy module-pipe-source FIFO.  Falls
    // back to the FIFO path on the per-channel granularity if open() fails.
    std::array<std::unique_ptr<PipeWireNativeRxSource>, NUM_CHANNELS> m_nativeRx;
#endif
};

} // namespace AetherSDR
