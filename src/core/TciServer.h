#pragma once
#ifdef HAVE_WEBSOCKETS

#include <QObject>
#include <QPointer>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>

class QWebSocketServer;
class QWebSocket;
class QTimer;

namespace AetherSDR {

class RadioModel;
class AudioEngine;
class SliceModel;
class TciProtocol;
class Resampler;

// Read-only snapshot of one connected TCI client, surfaced to the Radio
// Setup → TCI tab. TCI has no client-identity handshake, so a client is
// only ever known by its network endpoint plus the stream subscriptions
// it has requested.
struct TciClientInfo {
    QString peerAddress;
    quint16 peerPort{0};
    bool    audio{false};
    int     audioReceiver{-1};   // -1 = all receivers
    bool    iq{false};
    bool    rxSensors{false};
    bool    txSensors{false};
};

// TCI WebSocket server — exposes radio state and audio over the TCI protocol.
// Phase 1: text commands (VFO, mode, filter, TX, RIT/XIT, CW, spots)
// Phase 2: binary RX/TX audio streaming
class TciServer : public QObject {
    Q_OBJECT

public:
    explicit TciServer(RadioModel* model, QObject* parent = nullptr);
    ~TciServer() override;

    bool start(quint16 port = 50001);
    void stop();

    bool isRunning() const;
    quint16 port() const;
    int clientCount() const { return m_clients.size(); }

    // Snapshot of all currently connected clients (endpoint + subscriptions).
    // Cheap to call; intended for the Radio Setup → TCI tab on demand and
    // whenever clientsChanged() fires.
    QVector<TciClientInfo> connectedClients() const;

    void setAudioEngine(AudioEngine* audio) { m_audio = audio; }

    // Broadcast a master-volume change to all connected TCI clients. Called
    // by MainWindow whenever the GUI master volume slider moves so remote
    // controllers (e.g. aether_pad) stay in sync. Idempotent — clients
    // re-applying the value they just sent is harmless.
    void broadcastMasterVolume(int pct);

    // TCI TX gain (0.0–1.0). Applied to outbound TX audio from WSJT-X/JTDX
    // before the radio.  Decoupled from DaxTxGain (#1627) — the DAX bridge
    // and TCI maintain independent gain settings.  Persists to TciTxGain.
    void setTxGain(float gain);
    float txGain() const { return m_txGain; }

    // Per-channel TCI RX gain (0.0–1.0), applied to outbound DAX audio before
    // resampling and sending to TCI clients.  Decoupled from DaxRxGain<n> so
    // DAX bridge and TCI maintain independent per-channel gains.
    // Channel is 1-based (1–4).  Persists to TciRxGain<channel>.
    void setRxChannelGain(int channel, float gain);
    float rxChannelGain(int channel) const;

    // Wire slice signals for state change broadcasts
    void wireSlice(int trx, SliceModel* slice);
    void wireSpotModel();

public slots:
    // RX audio from main audio pipeline (float32 stereo, 24 kHz)
    void onRxAudioReady(const QByteArray& pcm);
    // RX audio from DAX pipeline (float32 stereo, 24 kHz)
    void onDaxAudioReady(int channel, const QByteArray& pcm);
    // IQ data from DAX IQ stream (big-endian float32 I/Q pairs)
    void onIqDataReady(int channel, const QByteArray& rawPayload, int sampleRate);
    // Waterfall row from PanadapterStream — forwarded to spectrum_event subscribers
    void onWaterfallRowReady(quint32 streamId, const QVector<float>& binsDbm,
                             double lowMhz, double highMhz,
                             quint32 timecode, qint64 emittedNs);

signals:
    void clientCountChanged(int count);
    // Fired whenever the client list or any client's subscriptions change
    // (connect, disconnect, audio start/stop). The TCI tab repopulates on
    // this signal.
    void clientsChanged();
    // Raw TCI text traffic for the embedded monitor. direction is
    // "rx" (received from a client) or "tx" (broadcast to clients).
    // One emission per logical message. Binary audio/IQ frames are
    // never emitted; high-rate text broadcasts like rx_smeter ARE
    // emitted but can be muted per-command via the monitor's
    // suppression list to keep the stream readable.
    void tciMessage(const QString& direction, const QString& text);
    void rxLevel(int channel, float rms);  // 1-based channel, RMS of TCI-gained RX audio
    void txLevel(float rms);                // RMS of post-gain TCI TX audio
    // Emitted when a TCI client sends `volume:N;` (master volume SET).
    // MainWindow handles it by calling the same path as the title bar
    // master volume slider — m_audio->setRxVolume() (or lineout when PC
    // audio is off) plus persistence to AppSettings.
    void masterVolumeRequested(int pct);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onTextMessage(const QString& msg);
    void onBinaryMessage(const QByteArray& data);
    void broadcastStatus();

private:
    void sendInitBurst(QWebSocket* client);
    void broadcast(const QString& msg);
    void broadcastBinary(const QByteArray& data);
    void startTxChrono(QWebSocket* client, int trx);
    void stopTxChrono();
    void sendTxChronoFrame(QWebSocket* client);
    void logTxAudioSummary(const char* reason);

    // Build a TCI binary audio frame (64-byte header + float32 samples)
    static QByteArray buildAudioFrame(int receiver, int type,
                                      int sampleRate, int channels,
                                      const float* samples, int sampleCount);

    struct ClientState {
        QWebSocket*  socket{nullptr};
        TciProtocol* protocol{nullptr};
        bool         audioEnabled{false};   // client sent AUDIO_START
        int          audioReceiver{-1};     // -1 = all receivers, otherwise TCI TRX
        int          audioSampleRate{48000}; // requested output rate (48kHz for WSJT-X compat)
        int          audioChannels{2};       // 1=mono, 2=stereo
        int          audioFormat{3};         // 0=int16, 3=float32
        // Per-DAX-channel resamplers.  A single shared r8brain instance would
        // carry filter state from slice A into slice B, causing audible
        // crosstalk (#1806).  Each channel gets its own stateful instance,
        // lazily created in onDaxAudioReady() and deleted/recreated whenever
        // the client changes its audio_samplerate.  No entry (or nullptr) for
        // a channel means 24 kHz pass-through (no resampling needed).
        QHash<int, Resampler*> resamplers;
        // Per-DAX-channel accumulation buffers. Concatenating multi-channel
        // packets into a shared buffer would interleave audio from different
        // slices and destroy the resampler output, so each channel maintains
        // its own staging area. QHash over QMap: channel count is tiny (1-4)
        // and we never iterate in key order.
        QHash<int, QByteArray> rxAccumBuf;
        bool         rxSensorsEnabled{false};
        bool         txSensorsEnabled{false};
        bool         iqEnabled{false};       // client sent IQ_START
        int          iqChannel{0};           // TCI TRX → DAX IQ channel (0-based)
        bool         spectrumEnabled{false}; // client sent spectrum_event:on;
    };

    // Minimum frames to accumulate before flushing to r8brain.
    // ~21ms at 24kHz — large enough for clean resampling, small enough
    // for acceptable latency in digital modes.
    static constexpr int kAccumMinFrames = 512;

    void ensureDaxForTci();
    void releaseDaxForTci();

    QPointer<RadioModel> m_model;  // QPointer auto-clears when RadioModel is destroyed (#2385)
    AudioEngine*      m_audio{nullptr};
    QWebSocketServer* m_server{nullptr};
    QList<ClientState> m_clients;
    QSet<int>         m_tciDaxSlices;   // slice IDs where we auto-assigned DAX (#1331)
    QMap<int, quint32> m_tciDaxStreamIds;      // DAX channel → stream ID created or borrowed by TCI
    QSet<int>          m_tciDaxBorrowedChannels; // channels where TCI reused an existing stream
    QTimer*           m_meterTimer{nullptr};  // 200ms status broadcast
    QTimer*           m_txChronoTimer{nullptr}; // TX_CHRONO frame cadence
    QWebSocket*       m_txChronoClient{nullptr};
    int               m_txChronoTrx{0};
    std::unique_ptr<Resampler> m_txResampler; // 48kHz→24kHz TX downsampler
    QElapsedTimer     m_txChronoClock;
    QElapsedTimer     m_txChronoSessionClock;
    qint64            m_txChronoAccumNs{0};
    qint64            m_txChronoRequestedFrames{0};
    bool              m_txUseRadioRoute{true};
    float             m_txGain{1.0f};
    float             m_rxChannelGain[4]{1.0f, 1.0f, 1.0f, 1.0f};
    qint64            m_txAudioBlocks{0};
    qint64            m_txInputFrames{0};
    qint64            m_txOutputFrames{0};
    qint64            m_txClipSamples{0};
    qint64            m_txAudioSampleCount{0};
    double            m_txAudioSumSq{0.0};
    float             m_txAudioPeak{0.0f};
    bool              m_txSawDuplicatedStereo{false};
    QElapsedTimer     m_rxAudioLogTimer;
    qint64            m_rxAudioPackets{0};
    qint64            m_rxAudioFramesSent{0};
    bool              m_lastTx{false};
    float             m_cachedSLevel[8]{-130,-130,-130,-130,-130,-130,-130,-130};
    float             m_cachedFwdPower{0};
    float             m_cachedSwr{1.0f};
    float             m_cachedMicLevel{-50.0f};
    float             m_cachedAlc{0.0f};       // SW-ALC peak, dBFS
};

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
