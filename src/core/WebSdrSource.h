#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QVector>
#include <QImage>
#include <memory>
#include <atomic>

#include "WebSdrAudioDecoder.h"
#include "WebSdrWaterfallDecoder.h"

class QWebSocket;
class QTimer;
class QNetworkAccessManager;

namespace AetherSDR {

class Resampler;

// Listen-only WebSDR receive source (PA3FWM software). Owns a QWebSocket to the
// audio stream, tunes via GET /~~param, decodes the audio and emits 24 kHz
// stereo float32 frames ready for AudioEngine::feedWebSdrAudio().
//
// Passive bolt-on: never touches the Flex radio model. Lives on a dedicated
// worker thread (moveToThread + init(), like FreeDvClient). The waterfall
// stream is added in M2.
class WebSdrSource : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected, Streaming, Error };

    explicit WebSdrSource(QObject* parent = nullptr);
    ~WebSdrSource() override;

    bool isConnected() const { return m_connected.load(); }

public slots:
    void init();                                  // construct socket/timers on worker thread
    void connectToServer(const QString& host);    // "host:port"
    void disconnectFromServer();
    void tune(double freqKHz, const QString& mode, double loKHz, double hiKHz);

signals:
    void stateChanged(int state, const QString& detail);   // State as int
    void bandsResolved(const QStringList& bandNames, int selectedBand);
    void bandSpan(double centerKHz, double srKHz, const QString& name);  // active band
    void audioReady(const QByteArray& pcm24kStereoF32);
    void rowReady(const QImage& waterfallRow);             // width×1 RGB32
    void sMeter(int level);

private slots:
    void onConnected();
    void onDisconnected();
    void onBinaryMessage(const QByteArray& message);
    void onWaterfallMessage(const QByteArray& message);
    void onReconnect();

private:
    void setState(State s, const QString& detail = QString());
    void openAudioSocket();
    void openWaterfallSocket();
    void ensureWaterfall();
    void emitBandSpan();
    void fetchBandInfo();
    int  pickBand(double freqKHz) const;
    void sendTuneCommand();
    void emitResampled(const QVector<qint16>& mono, int srcRate);

    struct Band { double centerKHz; double srKHz; QString name; };

    QString  m_host;
    QWebSocket* m_audioWs{nullptr};
    QWebSocket* m_wfWs{nullptr};
    QTimer*  m_reconnectTimer{nullptr};
    QNetworkAccessManager* m_net{nullptr};

    WebSdrAudioDecoder m_dec;
    WebSdrWaterfallDecoder m_wfDec;
    // At zoom 0 the full band is exactly 1024 px wide (units = 1024<<maxzoom).
    // Requesting 1024 makes the row span the whole band, so center±sr/2 maps
    // correctly to [0,width] and the listening marker lines up.
    int  m_wfWidth{1024};
    int  m_wfOpenBand{-1};   // band the waterfall socket is currently open for
    std::unique_ptr<Resampler> m_resampler;
    int m_resamplerSrcRate{0};

    QVector<Band> m_bands;
    int     m_band{0};
    double  m_freqKHz{0.0};
    QString m_mode{"usb"};
    double  m_loKHz{0.0};
    double  m_hiKHz{2.7};
    bool    m_haveTune{false};
    bool    m_bandValid{false};   // a real band was selected for the current freq

    State m_state{State::Disconnected};
    std::atomic<bool> m_connected{false};
    bool  m_intentionalDisconnect{false};
    int   m_reconnectDelayMs{kInitialReconnectDelayMs};

    static constexpr int kInitialReconnectDelayMs = 2000;
    static constexpr int kMaxReconnectDelayMs     = 30000;
    static constexpr int kOutputRate              = 24000;  // AudioEngine native
};

} // namespace AetherSDR
