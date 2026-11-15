#include "AetherPDCaptureSource.h"

#include "core/AetherPDTransportPolicy.h"
#include "core/TailnetShimClient.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <utility>

namespace AetherSDR::txlin {
namespace {
constexpr qint64 kMaxReplyBytes = 8192;
constexpr int kCaptureTimeoutMs = 6000;
}

AetherPDCaptureSource::AetherPDCaptureSource(QObject* parent)
    : QObject(parent), m_network(&m_ownedNetwork)
{
    m_ownedNetwork.setProxy(QNetworkProxy::NoProxy);
    initialize();
}

AetherPDCaptureSource::AetherPDCaptureSource(QNetworkAccessManager& network, QObject* parent)
    : QObject(parent), m_network(&network)
{
    initialize();
}

void AetherPDCaptureSource::initialize()
{
    m_ownedNetwork.setParent(this);
    m_deadline.setParent(this);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail("IQ capture timed out"); });
    connect(m_network, &QObject::destroyed, this, [this] {
        m_network = nullptr;
        if (m_running) { fail("IQ capture transport was destroyed"); }
    });
}

AetherPDCaptureSource::~AetherPDCaptureSource()
{
    if (m_network) { disconnect(m_network, nullptr, this, nullptr); }
    stop();
}

void AetherPDCaptureSource::abandon()
{
    ++m_generation;
    m_deadline.stop();
    m_age.invalidate();
    m_manifest.reset();
    m_bytes.clear();
    m_chunk = 0;
    m_checkingReady = false;
    m_readyCallback = {};
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    if (reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        if (reply) { reply->deleteLater(); }
    }
}

void AetherPDCaptureSource::stop()
{
    m_running = m_gate = false;
    abandon();
}

void AetherPDCaptureSource::setSession(const QHostAddress& address, const QString& token,
                                      std::function<bool()> current)
{
    stop();
    m_address = address;
    m_token = token;
    m_current = std::move(current);
}

void AetherPDCaptureSource::clearSession()
{
    setSession({}, {}, {});
}

bool AetherPDCaptureSource::sessionCurrent() const
{
    return m_network && m_network->thread() == thread() && m_current && m_current()
        && aetherpd::lanAddress(m_address) && aetherpd::validAdminToken(m_token);
}

bool AetherPDCaptureSource::start(const CaptureConfig& config)
{
    if (QThread::currentThread() != thread() || m_running || !sessionCurrent()
        || !m_frameCallback || !m_errorCallback || config.frameSamples != aetherpd::kCaptureSamples
        || !std::isfinite(config.centerFreqHz) || config.centerFreqHz <= 0 || config.centerFreqHz > 1e9
        || (config.sampleRateHz != 24000 && config.sampleRateHz != 48000
            && config.sampleRateHz != 96000 && config.sampleRateHz != 192000)) {
        return false;
    }
    abandon();
    m_config = config;
    m_running = true;
    m_gate = false;
    return true;
}

void AetherPDCaptureSource::setFrameCallback(FrameCallback callback)
{
    stop();
    m_frameCallback = std::move(callback);
}

void AetherPDCaptureSource::setErrorCallback(ErrorCallback callback)
{
    stop();
    m_errorCallback = std::move(callback);
}

void AetherPDCaptureSource::setGateOpen(bool open)
{
    if (m_checkingReady) { abandon(); }
    if (!m_running || m_gate == open) { return; }
    m_gate = open;
    abandon();
    if (open) { beginCapture(); }
}

bool AetherPDCaptureSource::checkReceiveReady(ReadyCallback ready)
{
    if (QThread::currentThread() != thread() || !m_running || m_gate || m_reply
        || m_checkingReady || !ready || !sessionCurrent()) {
        return false;
    }
    abandon();
    m_readyCallback = std::move(ready);
    m_checkingReady = true;
    beginCapture();
    return true;
}

void AetherPDCaptureSource::beginCapture()
{
    if (!m_running || (!m_gate && !m_checkingReady) || m_reply) { return; }
    if (!sessionCurrent()) { fail("IQ capture session is no longer current"); return; }
    m_manifest.reset();
    m_bytes.clear();
    m_chunk = 0;
    m_age.start();
    m_deadline.start(kCaptureTimeoutMs);
    send(aetherpd::captureBeginRequest());
}

void AetherPDCaptureSource::send(const aetherpd::Request& descriptor)
{
    if (!sessionCurrent()) { fail("IQ capture session is no longer current"); return; }
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort);
    url.setPath(descriptor.path);
    QNetworkRequest request(url);
    request.setTransferTimeout(descriptor.timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    m_reply = descriptor.method == "GET" ? m_network->get(request) : m_network->post(request, QByteArray{});
    QNetworkReply* reply = m_reply;
    const quint64 generation = m_generation;
    reply->setReadBufferSize(kMaxReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply, generation] {
        if (generation == m_generation && m_reply == reply && reply->bytesAvailable() > kMaxReplyBytes) {
            fail("IQ capture response exceeded its size limit");
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] { finish(reply, generation); });
}

void AetherPDCaptureSource::fail(const std::string& reason)
{
    const ErrorCallback callback = m_errorCallback;
    const bool running = m_running;
    stop();
    if (running && callback) { callback(reason); }
}

void AetherPDCaptureSource::finish(QNetworkReply* reply, quint64 generation)
{
    reply->deleteLater();
    if (m_reply != reply || generation != m_generation || !m_running
        || (!m_gate && !m_checkingReady)) { return; }
    m_reply = nullptr;
    if (!sessionCurrent()) { fail("IQ capture session is no longer current"); return; }
    if (!m_age.isValid() || m_age.elapsed() > kCaptureTimeoutMs) { fail("IQ capture timed out"); return; }
    const QByteArray body = reply->read(kMaxReplyBytes + 1);
    if (body.size() > kMaxReplyBytes || reply->error() != QNetworkReply::NoError
        || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        fail("IQ capture request failed"); return;
    }
    if (!m_manifest) {
        m_manifest = aetherpd::parseCaptureManifest(body);
        if (!m_manifest || m_manifest->keyed != !m_checkingReady || m_manifest->sampleRateHz != m_config.sampleRateHz
            || std::abs(double(m_manifest->centerHz) - m_config.centerFreqHz) > 0.5) {
            fail("IQ capture does not match the expected key state, rate or center"); return;
        }
        m_bytes.reserve(8 * aetherpd::kCaptureSamples);
    } else {
        const std::optional<QByteArray> bytes = aetherpd::parseCaptureChunk(body, *m_manifest, m_chunk);
        if (!bytes) { fail("IQ capture chunk was invalid or belonged to another snapshot"); return; }
        m_bytes.append(*bytes);
        ++m_chunk;
    }
    if (m_chunk < aetherpd::kCaptureSamples / aetherpd::kCaptureChunkSamples) {
        send(*aetherpd::captureChunkRequest(*m_manifest, m_chunk));
        return;
    }
    m_deadline.stop();
    if (m_bytes.size() != 8 * aetherpd::kCaptureSamples) {
        fail("IQ capture was incomplete"); return;
    }
    if (m_checkingReady) {
        const ReadyCallback callback = std::move(m_readyCallback);
        abandon();
        callback();
        return;
    }
    CaptureFrame frame;
    frame.sampleRateHz = m_manifest->sampleRateHz;
    frame.centerFreqHz = double(m_manifest->centerHz);
    frame.timestampNs = m_manifest->firstReceiveNs;
    frame.sourceCaptureId = m_manifest->captureId.toStdString();
    frame.correctionActive = m_manifest->correcting;
    frame.clippingKnown = false;
    frame.feedback.reserve(aetherpd::kCaptureSamples);
    const char* samples = m_bytes.data();
    for (qsizetype i = 0; i < 8 * aetherpd::kCaptureSamples; i += 8) {
        frame.feedback.emplace_back(std::bit_cast<float>(qFromLittleEndian<quint32>(samples + i)),
                                    std::bit_cast<float>(qFromLittleEndian<quint32>(samples + i + 4)));
    }
    m_bytes.clear();
    const FrameCallback callback = m_frameCallback;
    const QPointer<AetherPDCaptureSource> guard(this);
    callback(frame);
    if (!guard || generation != m_generation || !m_running || !m_gate) { return; }
    QTimer::singleShot(0, this, [this, generation] {
        if (generation == m_generation) { beginCapture(); }
    });
}
} // namespace AetherSDR::txlin
