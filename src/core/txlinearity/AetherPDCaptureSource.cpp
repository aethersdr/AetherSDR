#include "AetherPDCaptureSource.h"

#include "core/AetherPDTransportPolicy.h"
#include "core/TailnetShimClient.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLoggingCategory>
#include <QThread>
#include <QUrl>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace AetherSDR::txlin {
Q_LOGGING_CATEGORY(lcAetherPDCapture, "aether.waveform.capture", QtWarningMsg)
namespace {
constexpr qint64 kMaxReplyBytes = 8192;
constexpr int kCaptureTimeoutMs = 6000;
// Includes cold alignment and WDSP's first fit within the native eight-second
// measurement source. The independent TX lease and source expiry still bound RF.
constexpr int kMeasurementCaptureTimeoutMs = 8000;
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
    m_deadline.setTimerType(Qt::PreciseTimer);
    connect(&m_deadline, &QTimer::timeout, this, &AetherPDCaptureSource::captureTimeout);
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
    m_waitingForTones = false;
    m_readyCallback = {};
    const QPointer<QNetworkReply> controlReply = std::exchange(m_reply, {});
    const auto chunks = std::exchange(m_chunkReplies, {});
    const auto retire = [this](QPointer<QNetworkReply> reply) {
        if (reply) {
            disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            if (reply) { reply->deleteLater(); }
        }
    };
    retire(controlReply);
    for (const QPointer<QNetworkReply>& reply : chunks) { retire(reply); }
}

void AetherPDCaptureSource::stop()
{
    m_running = m_gate = false;
    m_verifiedReceiveSource.reset();
    abandon();
}

void AetherPDCaptureSource::setSession(const QHostAddress& address, const QString& token,
                                      std::function<bool()> current)
{
    stop();
    m_binding.reset();
    m_measurementSourceId = 0;
    m_measurementTones.reset();
    m_legacyCapture = m_sourceIdentitySupported = false;
    m_address = address;
    m_token = token;
    m_current = std::move(current);
}

void AetherPDCaptureSource::clearSession()
{
    setSession({}, {}, {});
}

bool AetherPDCaptureSource::setFeedbackBinding(const aetherpd::FeedbackBinding& binding)
{
    if (QThread::currentThread() != thread() || m_running || !binding.valid || binding.failed
        || binding.channel < 1 || binding.channel > 4 || !binding.pan || !binding.guiHandle) { return false; }
    m_binding = binding;
    return true;
}

bool AetherPDCaptureSource::setMeasurementSource(quint64 sourceId,
                                                std::optional<aetherpd::MeasurementToneConfig> tones)
{
    if (QThread::currentThread() != thread() || m_running || !sourceId
        || (tones && (!tones->valid() || tones->sourceId != sourceId))) { return false; }
    m_measurementSourceId = sourceId;
    m_measurementTones = tones;
    return true;
}

aetherpd::Request AetherPDCaptureSource::beginRequest() const
{
    return m_measurementTones || m_binding ? aetherpd::captureBeginWithTonesRequest()
                              : aetherpd::captureBeginRequest(!m_legacyCapture);
}

bool AetherPDCaptureSource::tonesMatch(const std::optional<aetherpd::MeasurementToneConfig>& observed) const
{
    // Missing configuration is the legacy fixed pair. Once confirmed, a profile
    // must survive status, receive readiness and every transmitted snapshot.
    return m_measurementTones ? observed == m_measurementTones
                             : !observed || observed->spacingHz == aetherpd::kDefaultToneSpacingHz;
}

bool AetherPDCaptureSource::sessionCurrent() const
{
    return m_network && m_network->thread() == thread() && m_current && m_current()
        && aetherpd::lanAddress(m_address) && aetherpd::validAdminToken(m_token);
}

std::optional<aetherpd::CaptureSourceIdentity> AetherPDCaptureSource::receiveSourceIdentity() const
{
    return QThread::currentThread() == thread() && m_running && sessionCurrent()
        ? m_verifiedReceiveSource : std::nullopt;
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
    m_waitingForTones = m_measurementSourceId && !m_checkingReady;
    m_captureBudgetMs = m_waitingForTones ? kMeasurementCaptureTimeoutMs : kCaptureTimeoutMs;
    m_deadline.start(m_captureBudgetMs);
    send(m_waitingForTones ? *aetherpd::request(aetherpd::Operation::Status) : beginRequest());
}

void AetherPDCaptureSource::send(const aetherpd::Request& descriptor, int chunk)
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
    QElapsedTimer requestAge;
    requestAge.start();
    QNetworkReply* reply = descriptor.method == "GET" ? m_network->get(request) : m_network->post(request, QByteArray{});
    QPointer<QNetworkReply>& pending = chunk < 0 ? m_reply : m_chunkReplies[chunk];
    pending = reply;
    const quint64 generation = m_generation;
    reply->setReadBufferSize(kMaxReplyBytes + 1);
    connect(reply, &QIODevice::readyRead, this, [this, reply, generation, chunk] {
        const QPointer<QNetworkReply>& pending = chunk < 0 ? m_reply : m_chunkReplies[chunk];
        if (generation == m_generation && pending == reply && reply->bytesAvailable() > kMaxReplyBytes) {
            fail("IQ capture response exceeded its size limit");
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, requestAge, chunk] {
        qCDebug(lcAetherPDCapture) << "IQ request completed" << reply->url().path()
            << "request_ms" << requestAge.elapsed() << "capture_ms" << (m_age.isValid() ? m_age.elapsed() : -1)
            << "chunks" << m_chunk << "waiting_tones" << m_waitingForTones
            << "http" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
            << "error" << reply->error() << "bytes" << reply->bytesAvailable();
        finish(reply, generation, chunk);
    });
}

void AetherPDCaptureSource::captureTimeout()
{
    qCWarning(lcAetherPDCapture) << "IQ capture deadline expired"
        << "capture_ms" << (m_age.isValid() ? m_age.elapsed() : -1) << "waiting_tones" << m_waitingForTones
        << "manifest" << m_manifest.has_value() << "chunks" << m_chunk;
    fail("IQ capture timed out");
}

void AetherPDCaptureSource::fail(const std::string& reason)
{
    const ErrorCallback callback = m_errorCallback;
    const bool running = m_running;
    stop();
    if (running && callback) { callback(reason); }
}

void AetherPDCaptureSource::finish(QNetworkReply* reply, quint64 generation, int chunk)
{
    reply->deleteLater();
    QPointer<QNetworkReply>& pending = chunk < 0 ? m_reply : m_chunkReplies[chunk];
    if (pending != reply || generation != m_generation || !m_running
        || (!m_gate && !m_checkingReady)) { return; }
    pending = nullptr;
    if (!sessionCurrent()) { fail("IQ capture session is no longer current"); return; }
    if (!m_age.isValid() || m_age.elapsed() >= m_captureBudgetMs) { captureTimeout(); return; }
    const QByteArray body = reply->read(kMaxReplyBytes + 1);
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (body.size() <= kMaxReplyBytes && !m_manifest && !m_waitingForTones
        && !m_binding && !m_measurementTones && !m_legacyCapture && !m_sourceIdentitySupported && chunk < 0
        && reply->url().path() == QStringLiteral("/v2/aetherpd/capture") && (code == 404 || code == 501)) {
        // Only explicit version absence permits legacy capture, within the
        // original deadline. A previously observed identity cannot disappear.
        m_legacyCapture = true;
        send(aetherpd::captureBeginRequest());
        return;
    }
    if (body.size() > kMaxReplyBytes || reply->error() != QNetworkReply::NoError
        || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        fail("IQ capture request failed"); return;
    }
    if (m_waitingForTones) {
        const std::optional<aetherpd::Status> observed = aetherpd::parseStatus(body);
        if (!observed || !observed->available || !observed->registered
            || !observed->measurement || observed->measurement->sourceId != m_measurementSourceId
            || (m_binding && observed->feedbackBinding != m_binding)
            || !tonesMatch(observed->measurementToneConfig)
            || (observed->calibrationState != aetherpd::CalibrationState::Running
                && observed->calibrationState != aetherpd::CalibrationState::Armed)) {
            fail("Measurement source changed or ended before capture"); return;
        }
        if (!observed->measurement->tones) {
            QTimer::singleShot(20, this, [this, generation] {
                if (generation == m_generation && m_running && m_gate && m_waitingForTones) {
                    send(*aetherpd::request(aetherpd::Operation::Status));
                }
            });
            return;
        }
        m_waitingForTones = false;
        send(beginRequest());
        return;
    }
    if (!m_manifest) {
        m_manifest = aetherpd::parseCaptureManifest(body);
        if (!m_manifest || m_manifest->keyed != !m_checkingReady || m_manifest->sampleRateHz != m_config.sampleRateHz
            || (m_binding && m_manifest->feedbackBinding != m_binding)
            || !tonesMatch(m_manifest->measurementToneConfig)
            || m_manifest->source.has_value() == m_legacyCapture
            || (m_verifiedReceiveSource && (!m_manifest->source
                || m_verifiedReceiveSource->streamId != m_manifest->source->streamId))
            || std::abs(double(m_manifest->centerHz) - m_config.centerFreqHz) > 0.5
            || (m_measurementSourceId && (!m_manifest->measurement
                || m_manifest->measurement->sourceId != m_measurementSourceId
                || (!m_checkingReady && !m_manifest->measurement->tones)))) {
            fail("IQ capture does not match the expected key state, rate, center or source"); return;
        }
        m_sourceIdentitySupported = m_manifest->source.has_value();
        m_bytes.resize(8 * aetherpd::kCaptureSamples);
        // Queue one fixed snapshot, allowing the network worker to make progress
        // without a GUI event-loop round trip between every 512-sample chunk.
        const QPointer<AetherPDCaptureSource> guard(this);
        for (int index = 0; index < kChunkCount; ++index) {
            send(*aetherpd::captureChunkRequest(*m_manifest, index), index);
            if (!guard || generation != m_generation || !m_running) { return; }
        }
        return;
    }
    const std::optional<QByteArray> bytes = aetherpd::parseCaptureChunk(body, *m_manifest, chunk);
    if (!bytes) { fail("IQ capture chunk was invalid or belonged to another snapshot"); return; }
    std::memcpy(m_bytes.data() + chunk * 8 * aetherpd::kCaptureChunkSamples,
                bytes->constData(), static_cast<std::size_t>(bytes->size()));
    ++m_chunk;
    if (m_chunk < kChunkCount) { return; }
    m_deadline.stop();
    if (m_bytes.size() != 8 * aetherpd::kCaptureSamples) {
        fail("IQ capture was incomplete"); return;
    }
    if (m_checkingReady) {
        m_verifiedReceiveSource = m_manifest->source;
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
    if (m_manifest->source) {
        frame.sourceStream = CaptureStreamIdentity{m_manifest->source->streamId, m_manifest->source->generation};
    }
    if (m_manifest->feedbackBinding) {
        const aetherpd::FeedbackBinding& binding = *m_manifest->feedbackBinding;
        frame.sourceBinding = CaptureBindingIdentity{binding.channel, binding.pan, binding.guiHandle};
    }
    frame.correctionActive = m_manifest->correcting;
    if (m_manifest->measurementToneConfig) {
        const aetherpd::MeasurementToneConfig& tones = *m_manifest->measurementToneConfig;
        frame.sourceTones = CaptureToneProfile{tones.sourceId, tones.lowerOffsetHz(), tones.upperOffsetHz()};
    }
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
