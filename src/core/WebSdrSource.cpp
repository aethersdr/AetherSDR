#include "WebSdrSource.h"

#include "Resampler.h"

#include <QWebSocket>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QRegularExpression>
#include <QLoggingCategory>

#include <vector>

Q_LOGGING_CATEGORY(lcWebSdr, "aether.websdr")

namespace AetherSDR {

namespace {

struct ModePreset { int modeInt; double lo; double hi; };

// mode integer: 0 = SSB/CW, 1 = AM, 4 = FM. USB/LSB/CW differ only via lo/hi.
ModePreset presetFor(const QString& modeLower)
{
    const QString m = modeLower.toLower();
    if (m == "usb") return {0,  0.0,  2.7};
    if (m == "lsb") return {0, -2.7,  0.0};
    if (m == "cw")  return {0, -0.2,  0.2};
    if (m == "cwu") return {0,  0.2,  0.7};
    if (m == "cwl") return {0, -0.7, -0.2};
    if (m == "am")  return {1, -4.5,  4.5};
    if (m == "fm")  return {4, -6.0,  6.0};
    return {0, 0.0, 2.7}; // default USB
}

} // namespace

WebSdrSource::WebSdrSource(QObject* parent) : QObject(parent) {}

WebSdrSource::~WebSdrSource()
{
    if (m_audioWs) {
        m_audioWs->abort();
    }
    if (m_wfWs) {
        m_wfWs->abort();
    }
}

void WebSdrSource::init()
{
    // Construct sockets/timers on the worker thread (#1929 pattern).
    m_net = new QNetworkAccessManager(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &WebSdrSource::onReconnect);
    m_wfDec.setWidth(m_wfWidth);
}

void WebSdrSource::setState(State s, const QString& detail)
{
    m_state = s;
    m_connected.store(s == State::Connected || s == State::Streaming);
    emit stateChanged(static_cast<int>(s), detail);
}

void WebSdrSource::connectToServer(const QString& host)
{
    m_host = host;
    m_intentionalDisconnect = false;
    m_reconnectDelayMs = kInitialReconnectDelayMs;
    setState(State::Connecting);
    fetchBandInfo();
    openAudioSocket();
}

void WebSdrSource::disconnectFromServer()
{
    m_intentionalDisconnect = true;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_audioWs) {
        m_audioWs->abort();
        m_audioWs->deleteLater();
        m_audioWs = nullptr;
    }
    if (m_wfWs) {
        m_wfWs->abort();
        m_wfWs->deleteLater();
        m_wfWs = nullptr;
    }
    m_wfOpenBand = -1;
    setState(State::Disconnected);
}

void WebSdrSource::openAudioSocket()
{
    if (m_audioWs) {
        m_audioWs->abort();
        m_audioWs->deleteLater();
        m_audioWs = nullptr;
    }

    m_audioWs = new QWebSocket(QStringLiteral("http://%1").arg(m_host),
                               QWebSocketProtocol::VersionLatest, this);
    connect(m_audioWs, &QWebSocket::connected,             this, &WebSdrSource::onConnected);
    connect(m_audioWs, &QWebSocket::disconnected,          this, &WebSdrSource::onDisconnected);
    connect(m_audioWs, &QWebSocket::binaryMessageReceived, this, &WebSdrSource::onBinaryMessage);
    auto onSocketError = [this](QAbstractSocket::SocketError) {
        qCWarning(lcWebSdr) << "audio socket error:" << m_audioWs->errorString();
        setState(State::Error, m_audioWs->errorString());
        if (!m_intentionalDisconnect && m_reconnectTimer) {
            m_reconnectTimer->start(m_reconnectDelayMs);
            m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
        }
    };
    // QWebSocket::errorOccurred is Qt 6.5+; the Linux CI floor is 6.4.2.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_audioWs, &QWebSocket::errorOccurred, this, onSocketError);
#else
    connect(m_audioWs, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, onSocketError);
#endif

    const QUrl url(QStringLiteral("ws://%1/~~stream?v=11").arg(m_host));
    m_audioWs->open(url);
}

void WebSdrSource::onReconnect()
{
    if (m_intentionalDisconnect) {
        return;
    }
    setState(State::Connecting);
    openAudioSocket();
}

void WebSdrSource::onConnected()
{
    m_reconnectDelayMs = kInitialReconnectDelayMs;
    setState(State::Connected);
    if (m_haveTune && m_bandValid) { sendTuneCommand(); ensureWaterfall(); }
}

void WebSdrSource::openWaterfallSocket()
{
    if (m_wfWs) {
        m_wfWs->abort();
        m_wfWs->deleteLater();
        m_wfWs = nullptr;
    }

    m_wfWs = new QWebSocket(QStringLiteral("http://%1").arg(m_host),
                            QWebSocketProtocol::VersionLatest, this);
    connect(m_wfWs, &QWebSocket::binaryMessageReceived,
            this, &WebSdrSource::onWaterfallMessage);
    // The band index is part of the path; format 1, full-band zoom.
    const QUrl url(QStringLiteral("ws://%1/~~waterstream%2?format=1&width=%3&zoom=0&start=0")
                       .arg(m_host).arg(m_band).arg(m_wfWidth));
    m_wfWs->open(url);
    m_wfOpenBand = m_band;
}

void WebSdrSource::ensureWaterfall()
{
    if (!m_haveTune || !m_bandValid) {
        return;
    }
    if (m_wfWs && m_wfOpenBand == m_band &&
        m_wfWs->state() != QAbstractSocket::UnconnectedState) {
        return;   // already open for this band
    }
    openWaterfallSocket();
}

void WebSdrSource::onWaterfallMessage(const QByteArray& message)
{
    QImage row;
    if (m_wfDec.feed(message, row)) {
        emit rowReady(row);
    }
}

void WebSdrSource::emitBandSpan()
{
    if (m_band >= 0 && m_band < m_bands.size()) {
        emit bandSpan(m_bands[m_band].centerKHz, m_bands[m_band].srKHz, m_bands[m_band].name);
    }
}

void WebSdrSource::onDisconnected()
{
    if (m_intentionalDisconnect) { setState(State::Disconnected); return; }
    setState(State::Error, QStringLiteral("disconnected"));
    if (m_reconnectTimer) {
        m_reconnectTimer->start(m_reconnectDelayMs);
        m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
    }
}

void WebSdrSource::onBinaryMessage(const QByteArray& message)
{
    m_dec.feed(message);
    const QVector<qint16> mono = m_dec.takeSamples();
    if (!mono.isEmpty()) {
        if (m_state != State::Streaming) {
            setState(State::Streaming);
        }
        emitResampled(mono, m_dec.nativeRate());
    }
    emit sMeter(m_dec.sMeter());
}

void WebSdrSource::emitResampled(const QVector<qint16>& mono, int srcRate)
{
    if (srcRate <= 0) {
        return;
    }
    if (!m_resampler || m_resamplerSrcRate != srcRate) {
        m_resampler = std::make_unique<Resampler>(static_cast<double>(srcRate),
                                                  static_cast<double>(kOutputRate),
                                                  8192);
        m_resamplerSrcRate = srcRate;
    }
    std::vector<float> f(mono.size());
    for (int i = 0; i < mono.size(); ++i) {
        f[i] = mono[i] / 32768.0f;
    }
    QByteArray stereo = m_resampler->processMonoToStereo(f.data(), static_cast<int>(f.size()));
    if (!stereo.isEmpty()) {
        emit audioReady(stereo);
    }
}

void WebSdrSource::tune(double freqKHz, const QString& mode, double loKHz, double hiKHz)
{
    m_freqKHz = freqKHz;
    m_mode = mode;
    m_loKHz = loKHz;
    m_hiKHz = hiKHz;
    m_haveTune = true;

    if (m_bands.isEmpty()) {
        return;   // band chosen once bandinfo arrives
    }

    const int nb = pickBand(freqKHz);
    if (nb < 0) {                    // not covered by this WebSDR — don't break the stream
        m_bandValid = false;
        setState(m_state, QStringLiteral("%1 kHz outside WebSDR bands").arg(freqKHz, 0, 'f', 1));
        return;
    }
    m_band = nb;
    m_bandValid = true;
    emitBandSpan();
    if (m_connected.load()) {
        sendTuneCommand();
        ensureWaterfall();
    }
}

void WebSdrSource::sendTuneCommand()
{
    if (!m_audioWs || m_audioWs->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const ModePreset p = presetFor(m_mode);   // for the demodulator integer only
    const QString cmd = QStringLiteral("GET /~~param?f=%1&band=%2&lo=%3&hi=%4&mode=%5&name=aethersdr")
                            .arg(m_freqKHz, 0, 'f', 3)
                            .arg(m_band)
                            .arg(m_loKHz, 0, 'f', 3)
                            .arg(m_hiKHz, 0, 'f', 3)
                            .arg(p.modeInt);
    m_audioWs->sendTextMessage(cmd);
}

int WebSdrSource::pickBand(double freqKHz) const
{
    for (int i = 0; i < m_bands.size(); ++i) {
        const double half = m_bands[i].srKHz / 2.0;
        if (freqKHz >= m_bands[i].centerKHz - half &&
            freqKHz <= m_bands[i].centerKHz + half) {
            return i;
        }
    }
    return -1;   // frequency not covered by any band
}

void WebSdrSource::fetchBandInfo()
{
    if (!m_net) {
        return;
    }
    const QUrl url(QStringLiteral("http://%1/tmp/bandinfo.js").arg(m_host));
    QNetworkReply* reply = m_net->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcWebSdr) << "bandinfo fetch failed:" << reply->errorString();
            return;
        }
        const QString js = QString::fromUtf8(reply->readAll());
        m_bands.clear();
        QStringList names;
        // centerfreq: N ... samplerate: N ... name: '...'
        QRegularExpression re(
            QStringLiteral("centerfreq:\\s*([0-9.]+).*?samplerate:\\s*([0-9.]+).*?name:\\s*'([^']*)'"),
            QRegularExpression::DotMatchesEverythingOption);
        auto it = re.globalMatch(js);
        while (it.hasNext()) {
            const auto m = it.next();
            m_bands.push_back({m.captured(1).toDouble(), m.captured(2).toDouble(), m.captured(3)});
            names << m.captured(3);
        }
        if (m_haveTune) {
            const int nb = pickBand(m_freqKHz);
            if (nb >= 0) {
                m_band = nb;
                m_bandValid = true;
                emitBandSpan();
                if (m_connected.load()) {
                    sendTuneCommand();
                    ensureWaterfall();
                }
            }
        }
        emit bandsResolved(names, m_band);
    });
}

} // namespace AetherSDR
