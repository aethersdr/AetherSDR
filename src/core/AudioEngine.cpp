#include "AudioEngine.h"
#include "AppSettings.h"
#include "ClientEq.h"
#include "ClientComp.h"
#include "ClientGate.h"
#include "ClientDeEss.h"
#include "ClientTube.h"
#include "ClientPudu.h"
#include "ClientPuduMonitor.h"
#include "ClientReverb.h"
#include "ClientFinalLimiter.h"
#include "ClientTxTestTone.h"
#include "ClientQuindarTone.h"
#include "QuindarLocalSink.h"
#include "CwSidetoneGenerator.h"
#include "CwSidetoneQAudioSink.h"
#include "CwSidetoneSinkBackend.h"
#ifdef HAVE_PORTAUDIO
#include "CwSidetonePortAudioSink.h"
#endif
#include "LogManager.h"
#include "OpusCodec.h"
#include "SpectralNR.h"
#ifdef HAVE_SPECBLEACH
#include "SpecbleachFilter.h"
#endif
#include "RNNoiseFilter.h"
#include "NvidiaBnrFilter.h"
#ifdef HAVE_DFNR
#include "DeepFilterFilter.h"
#endif
#ifdef __APPLE__
#include "MacNRFilter.h"
#endif
#include "Resampler.h"

#include <cmath>
#include <limits>
#include <QIODevice>
#include <QFile>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QDir>
#include <QtEndian>
#include <QThread>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

namespace {
constexpr qint64 kTxAutoRestartMinRuntimeMs = 60000;
constexpr qint64 kScopeEmitMinIntervalMs = 25;  // ~40 fps, per RX/TX source
// The strip's "Waveform CE-SSB" panel renders at higher refresh than
// the floating Waveform applet, so its dedicated post-chain emit
// path uses a shorter throttle so the widget actually has fresh data
// to draw on every frame.  ~120 Hz max — emissions over the strip
// widget's repaint rate are simply ignored by the panel.
constexpr qint64 kTxPostChainEmitMinIntervalMs = 8;
// RX strip-panel mirror — same 8 ms throttle so the strip's "Aetherial
// Waveform — RX" panel sees one emission per audio callback (no dropped
// blocks).  The shared scopeSamplesReady throttle stays at 25 ms for
// the floating WaveApplet which doesn't need this fidelity.
constexpr qint64 kRxPostChainEmitMinIntervalMs = 8;

bool devicePresent(const QList<QAudioDevice>& devices, const QAudioDevice& target)
{
    if (target.isNull()) {
        return false;
    }

    return std::any_of(devices.begin(), devices.end(), [&target](const QAudioDevice& device) {
        return device.id() == target.id();
    });
}
}

void AudioEngine::emitScopeFromFloat32Stereo(const QByteArray& pcm,
                                             int sampleRate,
                                             bool tx)
{
    const int floatSamples = pcm.size() / static_cast<int>(sizeof(float));
    if (floatSamples <= 0)
        return;

    QElapsedTimer& throttle = tx ? m_lastTxScopeEmit : m_lastRxScopeEmit;
    if (throttle.isValid() && throttle.elapsed() < kScopeEmitMinIntervalMs)
        return;

    const bool stereo = (floatSamples % 2) == 0;
    const int monoSamples = stereo ? floatSamples / 2 : floatSamples;
    QByteArray& mono = tx ? m_scopeTxScratch : m_scopeRxScratch;
    mono.resize(monoSamples * static_cast<int>(sizeof(float)));

    const auto* src = reinterpret_cast<const float*>(pcm.constData());
    auto* dst = reinterpret_cast<float*>(mono.data());
    if (stereo) {
        for (int i = 0; i < monoSamples; ++i) {
            const float avg = (src[i * 2] + src[i * 2 + 1]) * 0.5f;
            dst[i] = std::clamp(std::isfinite(avg) ? avg : 0.0f, -1.0f, 1.0f);
        }
    } else {
        for (int i = 0; i < monoSamples; ++i) {
            const float s = src[i];
            dst[i] = std::clamp(std::isfinite(s) ? s : 0.0f, -1.0f, 1.0f);
        }
    }

    if (throttle.isValid())
        throttle.restart();
    else
        throttle.start();
    emit scopeSamplesReady(mono, sampleRate > 0 ? sampleRate : DEFAULT_SAMPLE_RATE, tx);
}

void AudioEngine::emitScopeFromInt16Stereo(const QByteArray& pcm,
                                           int sampleRate,
                                           bool tx)
{
    const int intSamples = pcm.size() / static_cast<int>(sizeof(int16_t));
    if (intSamples <= 0)
        return;

    QElapsedTimer& throttle = tx ? m_lastTxScopeEmit : m_lastRxScopeEmit;
    if (throttle.isValid() && throttle.elapsed() < kScopeEmitMinIntervalMs)
        return;

    const bool stereo = (intSamples % 2) == 0;
    const int monoSamples = stereo ? intSamples / 2 : intSamples;
    QByteArray& mono = tx ? m_scopeTxScratch : m_scopeRxScratch;
    mono.resize(monoSamples * static_cast<int>(sizeof(float)));

    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    auto* dst = reinterpret_cast<float*>(mono.data());
    if (stereo) {
        for (int i = 0; i < monoSamples; ++i) {
            const float l = src[i * 2] / 32768.0f;
            const float r = src[i * 2 + 1] / 32768.0f;
            dst[i] = std::clamp((l + r) * 0.5f, -1.0f, 1.0f);
        }
    } else {
        for (int i = 0; i < monoSamples; ++i)
            dst[i] = std::clamp(src[i] / 32768.0f, -1.0f, 1.0f);
    }

    if (throttle.isValid())
        throttle.restart();
    else
        throttle.start();
    emit scopeSamplesReady(mono, sampleRate > 0 ? sampleRate : DEFAULT_SAMPLE_RATE, tx);
}

void AudioEngine::emitTxPostChainScopeFromInt16Stereo(const QByteArray& pcm,
                                                      int sampleRate)
{
    // Same int16-stereo → mono-float collapse as emitScopeFromInt16Stereo,
    // but reuses the TX scratch and emits on the dedicated
    // txPostChainScopeReady signal so consumers (channel-strip waveform
    // panel) see exactly the post-DSP-chain audio without competing
    // with the floating Waveform applet's main TX/RX feed.
    const int intSamples = pcm.size() / static_cast<int>(sizeof(int16_t));
    if (intSamples <= 0)
        return;

    if (m_lastTxPostChainScopeEmit.isValid()
        && m_lastTxPostChainScopeEmit.elapsed() < kTxPostChainEmitMinIntervalMs)
        return;

    const bool stereo = (intSamples % 2) == 0;
    const int monoSamples = stereo ? intSamples / 2 : intSamples;
    m_scopeTxPostChainScratch.resize(monoSamples * static_cast<int>(sizeof(float)));

    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    auto* dst = reinterpret_cast<float*>(m_scopeTxPostChainScratch.data());
    if (stereo) {
        for (int i = 0; i < monoSamples; ++i) {
            const float l = src[i * 2] / 32768.0f;
            const float r = src[i * 2 + 1] / 32768.0f;
            dst[i] = std::clamp((l + r) * 0.5f, -1.0f, 1.0f);
        }
    } else {
        for (int i = 0; i < monoSamples; ++i)
            dst[i] = std::clamp(src[i] / 32768.0f, -1.0f, 1.0f);
    }

    if (m_lastTxPostChainScopeEmit.isValid())
        m_lastTxPostChainScopeEmit.restart();
    else
        m_lastTxPostChainScopeEmit.start();
    emit txPostChainScopeReady(m_scopeTxPostChainScratch,
                               sampleRate > 0 ? sampleRate : DEFAULT_SAMPLE_RATE);
}

void AudioEngine::emitRxPostChainScopeFromFloat32Stereo(const QByteArray& pcm,
                                                         int sampleRate)
{
    // RX-side mirror of emitTxPostChainScopeFromInt16Stereo.  Same
    // float32-stereo → mono-float collapse as emitScopeFromFloat32Stereo,
    // but uses a dedicated 8 ms throttle and emits on rxPostChainScopeReady
    // so the channel strip's RX scope tracks wall clock at short windows.
    const int floatSamples = pcm.size() / static_cast<int>(sizeof(float));
    if (floatSamples <= 0)
        return;

    if (m_lastRxPostChainScopeEmit.isValid()
        && m_lastRxPostChainScopeEmit.elapsed() < kRxPostChainEmitMinIntervalMs)
        return;

    const bool stereo = (floatSamples % 2) == 0;
    const int monoSamples = stereo ? floatSamples / 2 : floatSamples;
    m_scopeRxPostChainScratch.resize(monoSamples * static_cast<int>(sizeof(float)));

    const auto* src = reinterpret_cast<const float*>(pcm.constData());
    auto* dst = reinterpret_cast<float*>(m_scopeRxPostChainScratch.data());
    if (stereo) {
        for (int i = 0; i < monoSamples; ++i) {
            const float avg = (src[i * 2] + src[i * 2 + 1]) * 0.5f;
            dst[i] = std::clamp(std::isfinite(avg) ? avg : 0.0f, -1.0f, 1.0f);
        }
    } else {
        for (int i = 0; i < monoSamples; ++i) {
            const float s = src[i];
            dst[i] = std::clamp(std::isfinite(s) ? s : 0.0f, -1.0f, 1.0f);
        }
    }

    if (m_lastRxPostChainScopeEmit.isValid())
        m_lastRxPostChainScopeEmit.restart();
    else
        m_lastRxPostChainScopeEmit.start();
    emit rxPostChainScopeReady(m_scopeRxPostChainScratch,
                               sampleRate > 0 ? sampleRate : DEFAULT_SAMPLE_RATE);
}

void AudioEngine::updateRxBufferStats()
{
    const qsizetype total = m_rxBuffer.size() + m_radeRxBuffer.size();
    m_rxBufferBytes.store(total);
    m_rxBufferPeakBytes.store(std::max(m_rxBufferPeakBytes.load(), total));
}

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , m_clientEqRx(std::make_unique<ClientEq>())
    , m_clientEqTx(std::make_unique<ClientEq>())
    , m_clientCompTx(std::make_unique<ClientComp>())
    , m_clientCompRx(std::make_unique<ClientComp>())
    , m_clientGateTx(std::make_unique<ClientGate>())
    , m_clientGateRx(std::make_unique<ClientGate>())
    , m_clientDeEssTx(std::make_unique<ClientDeEss>())
    , m_clientDeEssRx(std::make_unique<ClientDeEss>())
    , m_clientTubeTx(std::make_unique<ClientTube>())
    , m_clientTubeRx(std::make_unique<ClientTube>())
    , m_clientPuduTx(std::make_unique<ClientPudu>())
    , m_clientPuduRx(std::make_unique<ClientPudu>())
    , m_clientReverbTx(std::make_unique<ClientReverb>())
    , m_clientFinalLimiterTx(std::make_unique<ClientFinalLimiter>())
    , m_clientTxTestTone(std::make_unique<ClientTxTestTone>())
    , m_cwSidetone(std::make_unique<CwSidetoneGenerator>(48000))
    , m_clientQuindarTone(std::make_unique<ClientQuindarTone>())
{
    // Prepare client DSP at the native 24 kHz rate. Sink resampling is
    // handled separately after EQ — EQ always runs at radio-native rate.
    m_clientEqRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientEqTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientCompTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientGateTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientGateRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientCompRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientTubeRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientPuduRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientDeEssTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientDeEssRx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientTubeTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientPuduTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientReverbTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientFinalLimiterTx->prepare(DEFAULT_SAMPLE_RATE);
    m_clientTxTestTone->prepare(DEFAULT_SAMPLE_RATE);
    m_clientQuindarTone->prepare(DEFAULT_SAMPLE_RATE);
    loadClientEqSettings();      // restore persisted bands before first audio
    loadClientCompSettings();    // restore persisted comp params + chain order
    loadClientGateSettings();    // restore persisted gate params
    loadClientGateRxSettings();  // restore persisted RX gate params
    loadClientCompRxSettings();  // restore persisted RX comp params
    loadClientTubeRxSettings();  // restore persisted RX tube params
    loadClientPuduRxSettings();  // restore persisted RX PUDU params
    loadClientDeEssSettings();   // restore persisted de-esser params
    loadClientDeEssRxSettings(); // restore persisted RX de-esser params
    loadClientTubeSettings();    // restore persisted tube params
    loadClientPuduSettings();    // restore persisted PUDU params
    loadClientReverbSettings();  // restore persisted reverb params
    loadClientFinalLimiterSettings();  // restore persisted final-limiter params
    loadClientQuindarSettings();       // restore persisted Quindar tone params
    loadClientRxChainOrder();    // restore persisted RX chain order (Phase 0+)

    // Restore saved audio device selections
    auto& s = AppSettings::instance();
    QByteArray savedOutId = s.value("AudioOutputDeviceId", "").toByteArray();
    QByteArray savedInId  = s.value("AudioInputDeviceId",  "").toByteArray();

    if (!savedOutId.isEmpty()) {
        for (const auto& dev : QMediaDevices::audioOutputs()) {
            if (dev.id() == savedOutId) { m_outputDevice = dev; break; }
        }
    }
    if (!savedInId.isEmpty()) {
        for (const auto& dev : QMediaDevices::audioInputs()) {
            if (dev.id() == savedInId) { m_inputDevice = dev; break; }
        }
    }

    // Monitor audio output device list for changes — when a USB audio device
    // (like Connect6) power-cycles or WASAPI sessions reset after idle/screensaver,
    // restart the RX stream to re-acquire a fresh handle. (#1361)
    // Windows/macOS only: PipeWire on Linux crashes in pw_stream_connect when
    // audioOutputsChanged fires during device enumeration. The zombie sink
    // watchdog and stateChanged handlers cover Linux recovery instead.
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this]() {
        if (!m_outputDevice.isNull()) {
            const auto outputs = QMediaDevices::audioOutputs();
            if (!devicePresent(outputs, m_outputDevice)) {
                qCWarning(lcAudio) << "AudioEngine: selected output device is no longer available, falling back to the system default";
                m_outputDevice = QAudioDevice{};
            }
        }
        if (!m_audioSink) return;
        qCWarning(lcAudio) << "AudioEngine: audio output device list changed, restarting RX (#1361)";
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_audioSink) return;
            stopRxStream();
            startRxStream();
        }, Qt::QueuedConnection);
    });
#endif

    // Opus TX pacing timer — sends one queued packet every 10ms for even
    // delivery timing. Without this, QAudioSource delivers bursts of samples
    // that get Opus-encoded and sent back-to-back, causing jitter-induced
    // crackling on SmartLink/WAN connections.
    m_opusTxPaceTimer = new QTimer(this);
    m_opusTxPaceTimer->setTimerType(Qt::PreciseTimer);
    m_opusTxPaceTimer->setInterval(10);
    connect(m_opusTxPaceTimer, &QTimer::timeout, this, [this]() {
        if (m_opusTxQueue.isEmpty()) return;
        emit txPacketReady(m_opusTxQueue.takeFirst());
    });
    m_opusTxPaceTimer->start();

    // RX pacing timer -- drains m_rxBuffer into QAudioSink at regular intervals.
    // Includes latency management: caps buffer at ~200ms to prevent unbounded
    // growth when network packets arrive in bursts (common on Windows WASAPI
    // with virtual audio routers like Voicemeeter).
    m_rxTimer = new QTimer(this);
    m_rxTimer->setTimerType(Qt::PreciseTimer);
    m_rxTimer->setInterval(10);
    connect(m_rxTimer, &QTimer::timeout, this, [this]() {
        if (!m_audioSink || !m_audioDevice || !m_audioDevice->isOpen() || m_audioSink->state() == QAudio::StoppedState) return;

        // Cap buffer to bound latency. Default 200ms, user-adjustable for
        // high-jitter connections (VPN, SmartLink) where drops cause choppy audio.
        const int sampleRate = m_resampleTo48k ? 48000 : DEFAULT_SAMPLE_RATE;
        const int bufMs = m_rxBufferCapMs.load();
        const qsizetype maxBufBytes = sampleRate * 2 * static_cast<qsizetype>(sizeof(float)) * bufMs / 1000;
        if (m_rxBuffer.size() > maxBufBytes) {
            // Drop oldest samples to keep latency bounded
            m_rxBuffer.remove(0, m_rxBuffer.size() - maxBufBytes);
        }
        if (m_radeRxBuffer.size() > maxBufBytes) {
            m_radeRxBuffer.remove(0, m_radeRxBuffer.size() - maxBufBytes);
        }

        const qsizetype freeBytes = m_audioSink->bytesFree();
        if (freeBytes > 0 && m_rxBuffer.isEmpty() && m_radeRxBuffer.isEmpty()) {
            m_rxBufferUnderrunCount.fetch_add(1);
        }

        // Zombie sink watchdog: if we have data waiting but the sink reports
        // zero bytes free for ~2 seconds, the WASAPI handle is likely stale
        // (e.g. after screensaver/idle on Windows with USB audio). (#1361)
        if (freeBytes == 0 && !m_rxBuffer.isEmpty()) {
            if (++m_rxZombieTickCount >= kZombieTickThreshold) {
                m_rxZombieTickCount = 0;
                qCWarning(lcAudio) << "AudioEngine: sink appears zombie (bytesFree stuck at 0 for"
                                   << kZombieTickThreshold * 10 << "ms), restarting RX (#1361)";
                QMetaObject::invokeMethod(this, [this]() {
                    if (!m_audioSink) return;
                    stopRxStream();
                    startRxStream();
                }, Qt::QueuedConnection);
                return;
            }
        } else {
            m_rxZombieTickCount = 0;
        }

        // Audio liveness watchdog: if no audio data has arrived via
        // feedAudioData() for ~15 seconds while the sink is still running,
        // the audio backend may have silently stopped (CoreAudio after
        // extended idle, or the radio stopped sending VITA-49 packets).
        // Restart the sink to re-acquire a fresh handle. (#1411)
        if (m_lastAudioFeedTime.isValid()
            && m_lastAudioFeedTime.elapsed() > kAudioLivenessTimeoutMs
            && m_rxBuffer.isEmpty()) {
            qCWarning(lcAudio) << "AudioEngine: no audio data received for"
                               << m_lastAudioFeedTime.elapsed() << "ms, restarting RX (#1411)";
            m_lastAudioFeedTime.start();  // prevent repeated rapid restarts
            QMetaObject::invokeMethod(this, [this]() {
                if (!m_audioSink) return;
                stopRxStream();
                startRxStream();
            }, Qt::QueuedConnection);
            return;
        }

        // Align to float32 frame boundary before any arithmetic.
        const qsizetype floatBytes = static_cast<qsizetype>(sizeof(float));
        qsizetype len = (freeBytes / floatBytes) * floatBytes;
        len = std::min(len, std::max(m_rxBuffer.size(), m_radeRxBuffer.size()));
        if (len > 0)
        {
            QByteArray chunk;
            if (m_radeRxBuffer.isEmpty()) {
                // Fast path: no RADE speech active — write m_rxBuffer directly.
                chunk = m_rxBuffer.left(len);
                m_rxBuffer.remove(0, chunk.size());
            } else {
                // Mix path: add m_rxBuffer (SSB/CW audio or zero-filled muted-RADE
                // frames) and m_radeRxBuffer (decoded RADE speech) sample-wise.
                // Both are float32 stereo at the same rate. Zero-init the output
                // so that whichever buffer is shorter contributes silence for its
                // missing tail samples without special-casing.
                chunk = QByteArray(len, '\0');
                auto* out = reinterpret_cast<float*>(chunk.data());

                const qsizetype rxTake = (std::min(len, m_rxBuffer.size()) / floatBytes) * floatBytes;
                if (rxTake > 0) {
                    const auto* rx = reinterpret_cast<const float*>(m_rxBuffer.constData());
                    const qsizetype rxSamples = rxTake / floatBytes;
                    for (qsizetype i = 0; i < rxSamples; ++i)
                        out[i] += rx[i];
                    m_rxBuffer.remove(0, rxTake);
                }

                const qsizetype radeTake = (std::min(len, m_radeRxBuffer.size()) / floatBytes) * floatBytes;
                if (radeTake > 0) {
                    const auto* rade = reinterpret_cast<const float*>(m_radeRxBuffer.constData());
                    const qsizetype radeSamples = radeTake / floatBytes;
                    for (qsizetype i = 0; i < radeSamples; ++i)
                        out[i] += rade[i];
                    m_radeRxBuffer.remove(0, radeTake);
                }
                // Single clamp pass after all sources are mixed.
                const qsizetype totalSamples = len / floatBytes;
                for (qsizetype i = 0; i < totalSamples; ++i)
                    out[i] = std::clamp(out[i], -1.0f, 1.0f);
            }

            len = m_audioDevice->write(chunk);

            // Stale session watchdog: if we're writing data but processedUSecs()
            // hasn't advanced, the WASAPI session is silently discarding audio
            // (e.g. after Teams/Zoom reconfigures the audio endpoint). (#1569)
            qint64 processed = m_audioSink->processedUSecs();
            if (processed == m_lastProcessedUSecs) {
                if (++m_rxStaleTickCount >= kStaleTickThreshold) {
                    m_rxStaleTickCount = 0;
                    qCWarning(lcAudio) << "AudioEngine: sink appears stale (processedUSecs stuck at"
                                       << processed << "for" << kStaleTickThreshold * 10
                                       << "ms), restarting RX (#1569)";
                    QMetaObject::invokeMethod(this, [this]() {
                        if (!m_audioSink) return;
                        stopRxStream();
                        startRxStream();
                    }, Qt::QueuedConnection);
                    return;
                }
            } else {
                m_rxStaleTickCount = 0;
                m_lastProcessedUSecs = processed;
            }
        }

        m_rxBufferBytes.store(m_rxBuffer.size());
    });
    m_rxTimer->start();
}

AudioEngine::~AudioEngine()
{
    stopRxStream();
    stopTxStream();
}

QAudioFormat AudioEngine::makeFormat() const
{
    QAudioFormat fmt;
    fmt.setSampleRate(DEFAULT_SAMPLE_RATE);
    fmt.setChannelCount(2);                        // stereo
    fmt.setSampleFormat(QAudioFormat::Float);
    return fmt;
}

// ─── RX stream ───────────────────────────────────────────────────────────────

bool AudioEngine::startRxStream()
{
    if (m_audioSink) return true;   // already running

    m_rxBuffer.clear();
    m_rxBufferBytes.store(0);
    m_rxBufferPeakBytes.store(0);
    m_rxBufferUnderrunCount.store(0);
    m_rxBufferSampleRate.store(DEFAULT_SAMPLE_RATE);
    m_rxZombieTickCount = 0;
    m_rxStaleTickCount = 0;
    m_lastProcessedUSecs = 0;
    m_lastAudioFeedTime.start();  // initialize liveness watchdog (#1411)

    QAudioFormat fmt = makeFormat();
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!m_outputDevice.isNull()) {
        const auto outputs = QMediaDevices::audioOutputs();
        if (devicePresent(outputs, m_outputDevice)) {
            dev = m_outputDevice;
        } else {
            qCWarning(lcAudio) << "AudioEngine: saved output device is unavailable, using the system default output instead";
            m_outputDevice = QAudioDevice{};
        }
    }

#ifdef Q_OS_MAC
    if (!m_allowBluetoothTelephonyOutput.load()) {
        // Only override devices that look like Bluetooth telephony routes.
        // Telephony-only (HFP/SCO) routes cap out at 8-16 kHz and cannot
        // handle our native 24 kHz Float stereo format.  If the device
        // supports 24 kHz it's a normal output and should not be replaced,
        // even if 48 kHz is unsupported (happens on some CoreAudio device
        // types with newer Qt versions) (#1705).
        QAudioFormat nativeFmt = makeFormat();          // 24 kHz Float stereo
        const bool looksLikeTelephony = !dev.isFormatSupported(nativeFmt);

        QAudioFormat preferredFmt = makeFormat();
        preferredFmt.setSampleRate(48000);
        if (looksLikeTelephony && !dev.isFormatSupported(preferredFmt)) {
            const auto supportsPreferredOutput = [&preferredFmt](const QAudioDevice& candidate) {
                return !candidate.isNull() && candidate.isFormatSupported(preferredFmt);
            };

            const QAudioDevice defaultDev = QMediaDevices::defaultAudioOutput();
            if (supportsPreferredOutput(defaultDev)) {
                qCWarning(lcAudio) << "AudioEngine: selected output route looks telephony-only, using default 48k-capable output instead:"
                                   << defaultDev.description();
                dev = defaultDev;
            } else {
                const QString selectedDescription = dev.description();
                for (const QAudioDevice& candidate : QMediaDevices::audioOutputs()) {
                    if (candidate.id() == dev.id()) {
                        continue;
                    }
                    if (candidate.description() == selectedDescription
                        && supportsPreferredOutput(candidate)) {
                        qCWarning(lcAudio) << "AudioEngine: selected output route looks telephony-only, using sibling 48k-capable output instead:"
                                           << candidate.description();
                        dev = candidate;
                        break;
                    }
                }
            }
        }
    }
#endif

    // Prefer 48kHz on Windows — WASAPI shared mode accepts 24kHz but its
    // internal resampler introduces artifacts at non-standard rates that
    // become audible when radio-side NR (RNN/NRL/NRS) removes the noise
    // floor. Use r8brain for clean 24k→48k conversion instead, matching
    // the macOS TX-side fix for the same class of issue. (#2120)
#ifdef Q_OS_WIN
    fmt.setSampleRate(48000);
    m_resampleTo48k = true;
    m_audioSink = new QAudioSink(dev, fmt, this);
    m_audioSink->setVolume(m_muted.load() ? 0.0f : m_rxVolume.load());
    m_audioDevice = m_audioSink->start();
    if (!m_audioDevice) {
        qCWarning(lcAudio) << "AudioEngine: 48kHz sink failed to open, trying 24kHz";
        delete m_audioSink;
        fmt.setSampleRate(DEFAULT_SAMPLE_RATE);
        m_resampleTo48k = false;
        m_audioSink = new QAudioSink(dev, fmt, this);
        m_audioSink->setVolume(m_muted.load() ? 0.0f : m_rxVolume.load());
        m_audioDevice = m_audioSink->start();
        if (!m_audioDevice) {
            qCWarning(lcAudio) << "AudioEngine: 24kHz sink also failed";
            delete m_audioSink;
            m_audioSink = nullptr;
            return false;
        }
    }
    // Guard against WASAPI silently stopping the sink after idle/sleep.
    // Detect the silent stop and restart cleanly, mirroring the TX-side
    // fix for CoreAudio (#1149). (#1303)
    // Note: IdleState restart logic removed — it caused a restart loop on
    // Windows that prevented audio playback (#1405). The zombie sink
    // watchdog already handles stale WASAPI sessions after idle/sleep.
    connect(m_audioSink, &QAudioSink::stateChanged, this,
            [this](QAudio::State state) {
        if (state != QAudio::StoppedState) {
            return;
        }
        m_audioDevice = nullptr;
        if (!m_audioSink) {
            return;   // intentional stop (stopRxStream nulls this)
        }
        const QAudio::Error error = m_audioSink->error();
        if (error != QAudio::NoError) {
            qCWarning(lcAudio) << "AudioEngine: QAudioSink stopped with error, not auto-restarting RX"
                               << error;
            return;
        }
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_audioSink) return;
            qCWarning(lcAudio) << "AudioEngine: QAudioSink stopped unexpectedly, restarting RX (#1303)";
            stopRxStream();
            startRxStream();
        }, Qt::QueuedConnection);
    });
    qCWarning(lcAudio) << "AudioEngine: RX stream started at" << fmt.sampleRate() << "Hz"
                       << "device:" << dev.description();
    m_rxStreamStarted = true;
    startSidetoneStream();
    emit rxStarted();
    return true;
#else
    auto configureOutputFormat = [this, &dev](QAudioFormat& candidateFmt) {
        candidateFmt = makeFormat();
#ifdef Q_OS_MAC
        // CoreAudio can route Bluetooth headsets onto the HFP/telephony
        // transport when opened directly at 24 kHz. Prefer 48 kHz on macOS
        // so A2DP-capable devices stay on the normal output profile.
        candidateFmt.setSampleRate(48000);
        if (dev.isFormatSupported(candidateFmt)) {
            m_resampleTo48k = true;
            return true;
        }

        qCWarning(lcAudio) << "AudioEngine: output device does not support 48kHz stereo float, trying 24kHz";
        candidateFmt.setSampleRate(DEFAULT_SAMPLE_RATE);
        if (dev.isFormatSupported(candidateFmt)) {
            m_resampleTo48k = false;
            return true;
        }

        qCWarning(lcAudio) << "AudioEngine: output device does not support 24kHz stereo float either";
        return false;
#else
        if (dev.isFormatSupported(candidateFmt)) {
            m_resampleTo48k = false;
            return true;
        }

        qCWarning(lcAudio) << "AudioEngine: output device does not support 24kHz stereo Int16, trying 48kHz";
        candidateFmt.setSampleRate(48000);
        if (dev.isFormatSupported(candidateFmt)) {
            m_resampleTo48k = true;
            return true;
        }

        qCWarning(lcAudio) << "AudioEngine: output device does not support 48kHz stereo Int16 either";
        return false;
#endif
    };

    if (!configureOutputFormat(fmt)) {
        qCWarning(lcAudio) << "No audio device detected";
        return false;
    }
#endif

    m_audioSink   = new QAudioSink(dev, fmt, this);
    m_audioSink->setVolume(m_muted.load() ? 0.0f : m_rxVolume.load());
    m_audioDevice = m_audioSink->start();   // push-mode

    if (!m_audioDevice) {
        qCWarning(lcAudio) << "AudioEngine: failed to open audio sink";
        delete m_audioSink;
        m_audioSink = nullptr;
        return false;
    }

    // Guard against the audio backend silently stopping the sink after idle/sleep.
    // Detect the silent stop and restart cleanly, mirroring the TX-side
    // fix for CoreAudio (#1149). (#1303)
    // Note: IdleState restart logic removed — it caused a restart loop on
    // Windows that prevented audio playback (#1405). The zombie sink
    // watchdog already handles stale WASAPI sessions after idle/sleep.
    connect(m_audioSink, &QAudioSink::stateChanged, this,
            [this](QAudio::State state) {
        if (state != QAudio::StoppedState) {
            return;
        }
        m_audioDevice = nullptr;
        if (!m_audioSink) {
            return;   // intentional stop (stopRxStream nulls this)
        }
        const QAudio::Error error = m_audioSink->error();
        if (error != QAudio::NoError) {
            qCWarning(lcAudio) << "AudioEngine: QAudioSink stopped with error, not auto-restarting RX"
                               << error;
            return;
        }
        QMetaObject::invokeMethod(this, [this]() {
            if (!m_audioSink) return;
            qCWarning(lcAudio) << "AudioEngine: QAudioSink stopped unexpectedly, restarting RX (#1303)";
            stopRxStream();
            startRxStream();
        }, Qt::QueuedConnection);
    });
    qCDebug(lcAudio) << "AudioEngine: RX stream started";
    m_rxBufferSampleRate.store(fmt.sampleRate());
    m_rxStreamStarted = true;
    // Open the dedicated sidetone sink alongside the RX sink.  Cheap when
    // sidetone is disabled — the timer fires but writes silence to a tiny
    // primed buffer; no audible output, no extra CPU on the operator side.
    startSidetoneStream();
    startQuindarLocalSink();
    emit rxStarted();
    return true;
}

void AudioEngine::stopRxStream()
{
    stopSidetoneStream();
    stopQuindarLocalSink();
    m_rxBuffer.clear();
    m_rxBufferBytes.store(0);
    m_rxBufferSampleRate.store(DEFAULT_SAMPLE_RATE);

    if (m_audioSink) {
        // Null out m_audioSink BEFORE stopping so that the stateChanged
        // handler's "if (!m_audioSink) return" guard prevents a cascading
        // restart loop.  Without this, stop() emits stateChanged(StoppedState)
        // synchronously while m_audioSink is still non-null, causing the
        // handler to queue another stopRx+startRx — which repeats
        // indefinitely and prevents audio from ever playing. (#1441)
        auto* sink = m_audioSink;
        m_audioSink   = nullptr;
        m_audioDevice = nullptr;
        // Guard: same stale-device-handle crash can occur on the RX side (#1059).
        if (sink->state() != QAudio::StoppedState)
            sink->stop();
        delete sink;
    }
    emit rxStopped();
}

void AudioEngine::setRxVolume(float v)
{
    m_rxVolume.store(qBound(0.0f, v, 1.0f));
    if (m_audioSink)
        m_audioSink->setVolume(m_muted.load() ? 0.0f : m_rxVolume.load());
}

void AudioEngine::setMuted(bool muted)
{
    const bool prev = m_muted.load();
    m_muted.store(muted);
    if (m_audioSink)
        m_audioSink->setVolume(muted ? 0.0f : m_rxVolume.load());
    if (prev != muted)
        emit mutedChanged(muted);
}

// Pick the sidetone backend based on build flag + AppSettings override.
// PortAudio when available (lower latency on Linux/macOS); QAudioSink
// fallback otherwise or when explicitly requested by the user.
static std::unique_ptr<CwSidetoneSinkBackend> makeSidetoneBackend(QObject* qparent)
{
    const QString pref =
        AppSettings::instance().value("CwSidetoneBackend", "PortAudio").toString();

#ifdef HAVE_PORTAUDIO
    if (pref != "QAudioSink") {
        return std::unique_ptr<CwSidetoneSinkBackend>(
            new CwSidetonePortAudioSink());
    }
#endif
    return std::unique_ptr<CwSidetoneSinkBackend>(
        new CwSidetoneQAudioSink(qparent));
}

bool AudioEngine::startSidetoneStream()
{
    if (m_sidetoneSink && m_sidetoneSink->isRunning()) return true;
    if (!m_cwSidetone) return false;

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!m_outputDevice.isNull()) {
        const auto outputs = QMediaDevices::audioOutputs();
        for (const auto& d : outputs) {
            if (d.id() == m_outputDevice.id()) { dev = m_outputDevice; break; }
        }
    }

    m_sidetoneSink = makeSidetoneBackend(this);
    if (!m_sidetoneSink->start(dev, 48000, m_cwSidetone.get())) {
        // Backend failed — try the other one before giving up.  Most likely
        // path: PortAudio init failed on a quirky device, fall back to Qt.
#ifdef HAVE_PORTAUDIO
        if (qstrcmp(m_sidetoneSink->name(), "PortAudio") == 0) {
            qCWarning(lcAudio) << "AudioEngine: PortAudio sidetone failed, falling back to QAudioSink";
            m_sidetoneSink.reset(new CwSidetoneQAudioSink(this));
            if (!m_sidetoneSink->start(dev, 48000, m_cwSidetone.get())) {
                m_sidetoneSink.reset();
                return false;
            }
        } else {
            m_sidetoneSink.reset();
            return false;
        }
#else
        m_sidetoneSink.reset();
        return false;
#endif
    }

    qCInfo(lcAudio) << "AudioEngine: sidetone running on" << m_sidetoneSink->name()
                    << "rate=" << m_sidetoneSink->actualRateHz() << "Hz";
    return true;
}

void AudioEngine::stopSidetoneStream()
{
    if (m_sidetoneSink) {
        m_sidetoneSink->stop();
        m_sidetoneSink.reset();
    }
    if (m_cwSidetone) m_cwSidetone->reset();
}

bool AudioEngine::startQuindarLocalSink()
{
    if (m_quindarLocalSink && m_quindarLocalSink->isRunning()) return true;
    if (!m_clientQuindarTone) return false;

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!m_outputDevice.isNull()) {
        const auto outputs = QMediaDevices::audioOutputs();
        for (const auto& d : outputs) {
            if (d.id() == m_outputDevice.id()) { dev = m_outputDevice; break; }
        }
    }

    if (!m_quindarLocalSink) {
        m_quindarLocalSink = std::make_unique<QuindarLocalSink>(this);
    }
    if (!m_quindarLocalSink->start(dev, m_clientQuindarTone.get())) {
        m_quindarLocalSink.reset();
        return false;
    }
    return true;
}

void AudioEngine::stopQuindarLocalSink()
{
    if (m_quindarLocalSink) {
        m_quindarLocalSink->stop();
        m_quindarLocalSink.reset();
    }
}

void AudioEngine::setRxPan(int v)
{
    m_rxPan.store(qBound(0, v, 100));
}

// Apply the stored RX pan to a stereo float32 buffer in-place.
// Only called on NR output — the radio itself handles pan when NR is off.
// Pan law: linear, symmetric around centre (50).
//   pan 0-50  → L=1.0,           R=pan/50
//   pan 50-100→ L=(100-pan)/50,  R=1.0
// At pan=50 both gains are 1.0, so it is a true no-op when centred.
// Safety: if nFrames==0 (e.g. empty or partial buffer on an error path),
// the loop body never executes — no UB.
static void applyRxPanInPlace(float* stereo, int nFrames, int pan)
{
    if (pan == 50 || nFrames <= 0) return;
    const float lGain = (pan >= 50) ? (100 - pan) / 50.0f : 1.0f;
    const float rGain = (pan <= 50) ? pan        / 50.0f : 1.0f;
    for (int i = 0; i < nFrames; ++i) {
        stereo[2 * i    ] *= lGain;
        stereo[2 * i + 1] *= rGain;
    }
}

// Resample 24kHz stereo float32 → 48kHz stereo float32 via r8brain.
// L and R are processed through separate Resampler instances so that any
// per-channel difference (radio-applied audio_pan) is preserved.
// processStereoToStereo() collapses L+R to mono — do NOT use it here.
QByteArray AudioEngine::resampleStereo(const QByteArray& pcm)
{
    if (!m_rxResampler)
        m_rxResampler = std::make_unique<Resampler>(24000, 48000);
    if (!m_rxResamplerR)
        m_rxResamplerR = std::make_unique<Resampler>(24000, 48000);

    const int frames = pcm.size() / (2 * static_cast<int>(sizeof(float)));
    if (frames <= 0) return {};

    const auto* src = reinterpret_cast<const float*>(pcm.constData());

    std::vector<float> lBuf(frames), rBuf(frames);
    for (int i = 0; i < frames; ++i) {
        lBuf[i] = src[2 * i];
        rBuf[i] = src[2 * i + 1];
    }

    QByteArray lOut = m_rxResampler->process(lBuf.data(), frames);
    QByteArray rOut = m_rxResamplerR->process(rBuf.data(), frames);

    const int outFrames = lOut.size() / static_cast<int>(sizeof(float));
    const int rFrames   = rOut.size() / static_cast<int>(sizeof(float));
    const int commonFrames = std::min(outFrames, rFrames);
    if (commonFrames <= 0) return {};

    QByteArray result(commonFrames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto*       dst  = reinterpret_cast<float*>(result.data());
    const auto* lSrc = reinterpret_cast<const float*>(lOut.constData());
    const auto* rSrc = reinterpret_cast<const float*>(rOut.constData());
    for (int i = 0; i < commonFrames; ++i) {
        dst[2 * i]     = lSrc[i];
        dst[2 * i + 1] = rSrc[i];
    }
    return result;
}

void AudioEngine::feedAudioData(const QByteArray& pcm)
{
    if (!m_audioSink) return;  // PC audio disabled
    m_lastAudioFeedTime.start();  // reset liveness watchdog (#1411)

    // feedAudioData() handles all remote_audio_rx paths: SSB/CW/digital on any pan,
    // and the zero-filled frames the radio sends for the muted RADE slice
    // (audio_mute=1 zeroes the payload — it does NOT suppress packets).
    // All of these write to m_rxBuffer. Decoded RADE speech is separate:
    // feedDecodedSpeech() writes to m_radeRxBuffer. The drain timer mixes both
    // sample-wise before writing to the device, so zero frames from the muted
    // RADE slice add nothing to the output and SSB audio on a second pan is
    // heard alongside RADE decoded speech without fill-rate interference.

    auto writeAudio = [this](const QByteArray& data) {
        if (!m_audioDevice || !m_audioDevice->isOpen()) return;

        // Client-side parametric EQ runs at the native 24 kHz rate, after
        // any NR chain, before resample-to-48k and soft boost. Copy-then-
        // process because the caller owns `data`. Skip when disabled or
        // during TX (matches the NR-chain TX bypass policy).
        const QByteArray* eqSource = &data;
        if (m_clientEqRx && m_clientEqRx->isEnabled() && !m_radioTransmitting) {
            m_clientEqRxScratch = data;
            const int frames = m_clientEqRxScratch.size()
                             / (2 * static_cast<int>(sizeof(float)));
            m_clientEqRx->process(
                reinterpret_cast<float*>(m_clientEqRxScratch.data()),
                frames, 2);
            eqSource = &m_clientEqRxScratch;
        }

        // Tap post-EQ audio into the ring buffer for the editor's FFT
        // analyzer. Runs whether EQ is active or bypassed — the tap shows
        // the signal actually heading to the sink at native 24 kHz.
        const int tapFrames = eqSource->size() / (2 * static_cast<int>(sizeof(float)));
        if (tapFrames > 0) {
            tapClientEqRxStereo(
                reinterpret_cast<const float*>(eqSource->constData()),
                tapFrames);
        }

        // RX chain stage: GATE — runs after EQ, in place on a scratch
        // buffer so the EQ tap above sees the post-EQ / pre-gate signal
        // (matches the user's signal-flow expectation).  Skip during TX
        // for the same reason as EQ.
        const QByteArray* gateSource = eqSource;
        if (m_clientGateRx && m_clientGateRx->isEnabled() && !m_radioTransmitting) {
            m_clientGateRxScratch = *eqSource;
            applyClientGateRxFloat32(m_clientGateRxScratch);
            gateSource = &m_clientGateRxScratch;
        }

        // RX chain stage: COMP — runs after GATE.  Same scratch-copy
        // pattern.
        const QByteArray* compSource = gateSource;
        if (m_clientCompRx && m_clientCompRx->isEnabled() && !m_radioTransmitting) {
            m_clientCompRxScratch = *gateSource;
            applyClientCompRxFloat32(m_clientCompRxScratch);
            compSource = &m_clientCompRxScratch;
        }

        // RX chain stage: DESS — runs after COMP, before TUBE.  Same
        // scratch-copy pattern as the surrounding stages.
        const QByteArray* deEssSource = compSource;
        if (m_clientDeEssRx && m_clientDeEssRx->isEnabled() && !m_radioTransmitting) {
            m_clientDeEssRxScratch = *compSource;
            applyClientDeEssRxFloat32(m_clientDeEssRxScratch);
            deEssSource = &m_clientDeEssRxScratch;
        }

        // RX chain stage: TUBE — runs after DESS.
        const QByteArray* tubeSource = deEssSource;
        if (m_clientTubeRx && m_clientTubeRx->isEnabled() && !m_radioTransmitting) {
            m_clientTubeRxScratch = *deEssSource;
            applyClientTubeRxFloat32(m_clientTubeRxScratch);
            tubeSource = &m_clientTubeRxScratch;
        }

        // RX chain stage: PUDU — runs after TUBE.
        const QByteArray* puduSource = tubeSource;
        if (m_clientPuduRx && m_clientPuduRx->isEnabled() && !m_radioTransmitting) {
            m_clientPuduRxScratch = *tubeSource;
            applyClientPuduRxFloat32(m_clientPuduRxScratch);
            puduSource = &m_clientPuduRxScratch;
        }

        const int scopeSampleRate = m_resampleTo48k ? 48000 : DEFAULT_SAMPLE_RATE;
        const QByteArray& resampled = m_resampleTo48k ? resampleStereo(*puduSource) : *puduSource;
        const QByteArray* output = &resampled;
        QByteArray boosted;
        if (m_rxBoost.load()) {
            // Soft-knee boost — increases perceived loudness without hard clipping.
            // Uses tanh compression: loud signals are gently limited while quiet
            // signals get ~2x gain.  tanh(2*x) ≈ 2*x for small x, ≈ 1.0 for large x.
            boosted.resize(resampled.size());
            const auto* src = reinterpret_cast<const float*>(resampled.constData());
            auto* dst = reinterpret_cast<float*>(boosted.data());
            const int nSamples = resampled.size() / static_cast<int>(sizeof(float));
            for (int i = 0; i < nSamples; ++i) {
                dst[i] = std::tanh(src[i] * 2.0f);
            }
            output = &boosted;
        }
        QByteArray trimmed;
        const float trimDb = m_rxOutputTrimDb.load();
        if (std::fabs(trimDb) > 0.01f) {
            const float gain = std::pow(10.0f, trimDb / 20.0f);
            trimmed.resize(output->size());
            const auto* src = reinterpret_cast<const float*>(output->constData());
            auto* dst = reinterpret_cast<float*>(trimmed.data());
            const int nSamples = output->size() / static_cast<int>(sizeof(float));
            for (int i = 0; i < nSamples; ++i) dst[i] = src[i] * gain;
            output = &trimmed;
        }
        m_rxBuffer.append(*output);
        emitScopeFromFloat32Stereo(*output, scopeSampleRate, false);
        emitRxPostChainScopeFromFloat32Stereo(*output, scopeSampleRate);
        updateRxBufferStats();
    };

    // Bypass client-side DSP during TX (#367, #1505). NR2/RN2/BNR adapt
    // their internal state to silence during TX, causing distorted audio
    // after returning to RX. Use m_radioTransmitting (raw interlock state)
    // so bypass kicks in even when an external app triggers PTT.
    // DSP mutex: prevents use-after-free if enable/disable runs concurrently (#502)
    {
        std::lock_guard<std::recursive_mutex> dspLock(m_dspMutex);
        if (m_radioTransmitting) {
            writeAudio(pcm);
            emit levelChanged(computeRMS(pcm));
        } else if (m_rn2Enabled && m_rn2) {
            QByteArray processed = m_rn2->process(pcm);
            // Re-apply pan lost during NR mono-mix (#1460)
            applyRxPanInPlace(reinterpret_cast<float*>(processed.data()),
                              processed.size() / (2 * static_cast<int>(sizeof(float))),
                              m_rxPan.load());
            writeAudio(processed);
            emit levelChanged(computeRMS(processed));
        } else if (m_nr2Enabled && m_nr2) {
            processNr2(pcm);  // applyRxPanInPlace called inside processNr2
            writeAudio(m_nr2Output);
            emit levelChanged(computeRMS(m_nr2Output));

#ifdef HAVE_SPECBLEACH
        } else if (m_nr4Enabled && m_nr4) {
            QByteArray processed = m_nr4->process(pcm);
            // Re-apply pan lost during NR mono-mix (#1460)
            applyRxPanInPlace(reinterpret_cast<float*>(processed.data()),
                              processed.size() / (2 * static_cast<int>(sizeof(float))),
                              m_rxPan.load());
            writeAudio(processed);
            emit levelChanged(computeRMS(processed));
#endif
#ifdef HAVE_DFNR
        } else if (m_dfnrEnabled && m_dfnr) {
            QByteArray processed = m_dfnr->process(pcm);
            // Re-apply pan lost during NR mono-mix (#1460)
            applyRxPanInPlace(reinterpret_cast<float*>(processed.data()),
                              processed.size() / (2 * static_cast<int>(sizeof(float))),
                              m_rxPan.load());
            writeAudio(processed);
            emit levelChanged(computeRMS(processed));
#endif
#ifdef __APPLE__
        } else if (m_mnrEnabled && m_mnr) {
            QByteArray processed = m_mnr->process(pcm);
            writeAudio(processed);
            emit levelChanged(computeRMS(processed));
#endif
        } else if (m_bnrEnabled && m_bnr && m_bnr->isConnected()) {
            processBnr(pcm);
            // processBnr writes audio and emits level internally
        } else {
            writeAudio(pcm);
            emit levelChanged(computeRMS(pcm));
        }
    }
}

namespace {

// Key builders kept local — settings namespace lives inside AudioEngine.cpp
// so the applet never reaches past these functions to form keys directly.
QString ceqKey(const char* pathTag, const char* leaf)
{
    return QStringLiteral("ClientEq%1%2").arg(pathTag, leaf);
}

QString ceqBandKey(const char* pathTag, int band, const char* leaf)
{
    return QStringLiteral("ClientEq%1_Band%2_%3")
        .arg(pathTag).arg(band).arg(leaf);
}

void loadOne(ClientEq& eq, const char* tag)
{
    auto& s = AppSettings::instance();
    const bool enabled = s.value(ceqKey(tag, "Enabled"), "False").toString() == "True";
    const int savedCount = std::clamp(
        s.value(ceqKey(tag, "BandCount"), "0").toString().toInt(),
        0, ClientEq::kMaxBands);
    const float masterGain = std::clamp(
        s.value(ceqKey(tag, "MasterGain"), "1.0").toString().toFloat(),
        0.0f, 4.0f);
    const int familyIdx = std::clamp(
        s.value(ceqKey(tag, "FilterFamily"), "0").toString().toInt(), 0, 3);
    eq.setEnabled(enabled);
    eq.setMasterGain(masterGain);
    eq.setFilterFamily(static_cast<ClientEq::FilterFamily>(familyIdx));

    // Fixed 8-slot layout.  If the user's saved state has fewer bands,
    // we keep their saved ones in slots [0, savedCount) and pad the
    // remaining slots with the default Logic-Pro-style templates, all
    // disabled.  Existing users migrate in place — their configured
    // bands survive, they just gain a few untouched defaults next to them.
    const int activeCount = ClientEq::kDefaultBandCount;
    eq.setActiveBandCount(activeCount);

    for (int i = 0; i < activeCount; ++i) {
        ClientEq::BandParams p;
        if (i < savedCount) {
            p.freqHz  = s.value(ceqBandKey(tag, i, "Freq"), "1000").toString().toFloat();
            p.gainDb  = s.value(ceqBandKey(tag, i, "Gain"), "0").toString().toFloat();
            p.q       = s.value(ceqBandKey(tag, i, "Q"),    "0.707").toString().toFloat();
            p.type    = static_cast<ClientEq::FilterType>(
                s.value(ceqBandKey(tag, i, "Type"), "0").toString().toInt());
            p.enabled = s.value(ceqBandKey(tag, i, "BandEn"), "True").toString() == "True";
            p.slopeDbPerOct = std::clamp(
                s.value(ceqBandKey(tag, i, "Slope"), "12").toString().toInt(),
                12, 48);
        } else {
            p = ClientEq::defaultBand(i);  // disabled by default
        }
        eq.setBand(i, p);
    }
}

void saveOne(const ClientEq& eq, const char* tag)
{
    auto& s = AppSettings::instance();
    s.setValue(ceqKey(tag, "Enabled"),
               eq.isEnabled() ? "True" : "False");
    s.setValue(ceqKey(tag, "MasterGain"),
               QString::number(eq.masterGain(), 'f', 3));
    s.setValue(ceqKey(tag, "FilterFamily"),
               QString::number(static_cast<int>(eq.filterFamily())));
    const int count = eq.activeBandCount();
    s.setValue(ceqKey(tag, "BandCount"), QString::number(count));
    for (int i = 0; i < count; ++i) {
        const ClientEq::BandParams p = eq.band(i);
        s.setValue(ceqBandKey(tag, i, "Freq"),
                   QString::number(p.freqHz, 'f', 2));
        s.setValue(ceqBandKey(tag, i, "Gain"),
                   QString::number(p.gainDb, 'f', 2));
        s.setValue(ceqBandKey(tag, i, "Q"),
                   QString::number(p.q, 'f', 3));
        s.setValue(ceqBandKey(tag, i, "Type"),
                   QString::number(static_cast<int>(p.type)));
        s.setValue(ceqBandKey(tag, i, "BandEn"),
                   p.enabled ? "True" : "False");
        s.setValue(ceqBandKey(tag, i, "Slope"),
                   QString::number(p.slopeDbPerOct));
    }
}

} // namespace

void AudioEngine::loadClientEqSettings()
{
    if (!m_clientEqRx || !m_clientEqTx) return;
    loadOne(*m_clientEqRx, "Rx");
    loadOne(*m_clientEqTx, "Tx");
}

void AudioEngine::saveClientEqSettings() const
{
    if (!m_clientEqRx || !m_clientEqTx) return;
    saveOne(*m_clientEqRx, "Rx");
    saveOne(*m_clientEqTx, "Tx");
    AppSettings::instance().save();
}

void AudioEngine::tapClientEqRxStereo(const float* stereoInterleaved, int frames)
{
    if (frames <= 0) return;
    // Audio-thread writer: skip silently if UI thread holds the lock —
    // dropping a block of tap samples just produces a one-frame stutter
    // on the FFT display, never an audio glitch.
    std::unique_lock<std::mutex> lk(m_clientEqTapMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    int w = m_clientEqTapRxWrite;
    for (int i = 0; i < frames; ++i) {
        const float mono = 0.5f * (stereoInterleaved[i * 2]
                                 + stereoInterleaved[i * 2 + 1]);
        m_clientEqTapRx[w] = mono;
        w = (w + 1) & (kClientEqTapSize - 1);
    }
    m_clientEqTapRxWrite = w;
}

void AudioEngine::tapClientEqTxInt16(const int16_t* int16stereo, int frames)
{
    if (frames <= 0) return;
    std::unique_lock<std::mutex> lk(m_clientEqTapMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    int w = m_clientEqTapTxWrite;
    for (int i = 0; i < frames; ++i) {
        const float l = int16stereo[i * 2]     / 32768.0f;
        const float r = int16stereo[i * 2 + 1] / 32768.0f;
        m_clientEqTapTx[w] = 0.5f * (l + r);
        w = (w + 1) & (kClientEqTapSize - 1);
    }
    m_clientEqTapTxWrite = w;
}

void AudioEngine::tapClientEqTxFloat32(const float* f32, int samples, int channels)
{
    if (samples <= 0 || channels < 1 || channels > 2) return;
    std::unique_lock<std::mutex> lk(m_clientEqTapMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    int w = m_clientEqTapTxWrite;
    const int frames = samples / channels;
    for (int i = 0; i < frames; ++i) {
        float mono;
        if (channels == 2) {
            mono = 0.5f * (f32[i * 2] + f32[i * 2 + 1]);
        } else {
            mono = f32[i];
        }
        m_clientEqTapTx[w] = mono;
        w = (w + 1) & (kClientEqTapSize - 1);
    }
    m_clientEqTapTxWrite = w;
}

bool AudioEngine::copyRecentClientEqRxSamples(float* out, int count) const
{
    if (!out || count <= 0 || count > kClientEqTapSize) return false;
    std::lock_guard<std::mutex> lk(m_clientEqTapMutex);
    int w = m_clientEqTapRxWrite;
    for (int i = 0; i < count; ++i) {
        // Fill newest-last: out[count-1] is the most recent sample.
        const int idx = (w - count + i + kClientEqTapSize) & (kClientEqTapSize - 1);
        out[i] = m_clientEqTapRx[idx];
    }
    return true;
}

bool AudioEngine::copyRecentClientEqTxSamples(float* out, int count) const
{
    if (!out || count <= 0 || count > kClientEqTapSize) return false;
    std::lock_guard<std::mutex> lk(m_clientEqTapMutex);
    int w = m_clientEqTapTxWrite;
    for (int i = 0; i < count; ++i) {
        const int idx = (w - count + i + kClientEqTapSize) & (kClientEqTapSize - 1);
        out[i] = m_clientEqTapTx[idx];
    }
    return true;
}

void AudioEngine::applyClientEqTxInt16(QByteArray& int16stereo)
{
    if (int16stereo.isEmpty()) return;
    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;  // must be stereo
    const int frames = samples / 2;

    // EQ processing only when enabled.  The tap below runs regardless
    // so the editor's TX FFT analyzer always reflects live mic input,
    // even when the EQ stage is bypassed in the CHAIN widget.
    if (m_clientEqTx && m_clientEqTx->isEnabled()) {
        m_clientEqTxScratch.resize(samples * static_cast<int>(sizeof(float)));
        auto* f32 = reinterpret_cast<float*>(m_clientEqTxScratch.data());
        const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
        for (int i = 0; i < samples; ++i) {
            f32[i] = i16[i] / 32768.0f;
        }

        m_clientEqTx->process(f32, frames, 2);

        auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
        for (int i = 0; i < samples; ++i) {
            out[i] = static_cast<int16_t>(std::clamp(f32[i] * 32768.0f,
                                                     -32768.0f, 32767.0f));
        }
    }
    // Always tap — bypassed-EQ case means tap captures pre-EQ samples
    // (which equal post-EQ samples since no processing happened).
    tapClientEqTxInt16(reinterpret_cast<const int16_t*>(int16stereo.constData()),
                       frames);
}

void AudioEngine::applyClientEqTxFloat32(QByteArray& float32)
{
    if (float32.isEmpty()) return;
    const int samples = float32.size() / static_cast<int>(sizeof(float));
    // feedDaxTxAudio can deliver mono OR stereo float32 (depends on packet
    // class). Treat even sample counts as stereo, odd counts as mono.
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames = samples / channels;

    if (m_clientEqTx && m_clientEqTx->isEnabled()) {
        m_clientEqTx->process(reinterpret_cast<float*>(float32.data()),
                              frames, channels);
    }
    // Always tap so the editor's TX FFT analyzer reflects live audio
    // even when the EQ stage is bypassed in the CHAIN widget.
    tapClientEqTxFloat32(reinterpret_cast<const float*>(float32.constData()),
                         samples, channels);
}

void AudioEngine::applyClientCompTxInt16(QByteArray& int16stereo)
{
    if (!m_clientCompTx || !m_clientCompTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientCompTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientCompTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientCompTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientCompTxFloat32(QByteArray& float32)
{
    if (!m_clientCompTx || !m_clientCompTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientCompTx->process(reinterpret_cast<float*>(float32.data()),
                            frames, channels);
}

void AudioEngine::applyClientCompRxFloat32(QByteArray& float32)
{
    if (!m_clientCompRx || !m_clientCompRx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples = float32.size() / static_cast<int>(sizeof(float));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;
    m_clientCompRx->process(reinterpret_cast<float*>(float32.data()),
                            frames, 2);
}

void AudioEngine::applyClientGateTxInt16(QByteArray& int16stereo)
{
    if (!m_clientGateTx || !m_clientGateTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientGateTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientGateTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientGateTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientGateTxFloat32(QByteArray& float32)
{
    if (!m_clientGateTx || !m_clientGateTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientGateTx->process(reinterpret_cast<float*>(float32.data()),
                            frames, channels);
}

void AudioEngine::applyClientGateRxFloat32(QByteArray& float32)
{
    if (!m_clientGateRx || !m_clientGateRx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples = float32.size() / static_cast<int>(sizeof(float));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;  // RX path is always stereo
    m_clientGateRx->process(reinterpret_cast<float*>(float32.data()),
                            frames, 2);
}

void AudioEngine::applyClientDeEssTxInt16(QByteArray& int16stereo)
{
    if (!m_clientDeEssTx || !m_clientDeEssTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientDeEssTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientDeEssTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientDeEssTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientDeEssRxFloat32(QByteArray& float32)
{
    if (!m_clientDeEssRx || !m_clientDeEssRx->isEnabled()) return;
    const int frames = float32.size() / static_cast<int>(sizeof(float)) / 2;
    if (frames <= 0) return;

    m_clientDeEssRx->process(reinterpret_cast<float*>(float32.data()),
                             frames, 2);
}

void AudioEngine::applyClientDeEssTxFloat32(QByteArray& float32)
{
    if (!m_clientDeEssTx || !m_clientDeEssTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientDeEssTx->process(reinterpret_cast<float*>(float32.data()),
                             frames, channels);
}

void AudioEngine::applyClientTubeTxInt16(QByteArray& int16stereo)
{
    if (!m_clientTubeTx || !m_clientTubeTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientTubeTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientTubeTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientTubeTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientTubeTxFloat32(QByteArray& float32)
{
    if (!m_clientTubeTx || !m_clientTubeTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientTubeTx->process(reinterpret_cast<float*>(float32.data()),
                            frames, channels);
}

void AudioEngine::applyClientTubeRxFloat32(QByteArray& float32)
{
    if (!m_clientTubeRx || !m_clientTubeRx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples = float32.size() / static_cast<int>(sizeof(float));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;
    m_clientTubeRx->process(reinterpret_cast<float*>(float32.data()),
                            frames, 2);
}

void AudioEngine::applyClientPuduTxInt16(QByteArray& int16stereo)
{
    if (!m_clientPuduTx || !m_clientPuduTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientPuduTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientPuduTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientPuduTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientPuduTxFloat32(QByteArray& float32)
{
    if (!m_clientPuduTx || !m_clientPuduTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientPuduTx->process(reinterpret_cast<float*>(float32.data()),
                            frames, channels);
}

void AudioEngine::applyClientPuduRxFloat32(QByteArray& float32)
{
    if (!m_clientPuduRx || !m_clientPuduRx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples = float32.size() / static_cast<int>(sizeof(float));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;
    m_clientPuduRx->process(reinterpret_cast<float*>(float32.data()),
                            frames, 2);
}

void AudioEngine::applyClientReverbTxInt16(QByteArray& int16stereo)
{
    if (!m_clientReverbTx || !m_clientReverbTx->isEnabled()) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientReverbTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientReverbTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientReverbTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientFinalLimiterTxInt16(QByteArray& int16stereo)
{
    if (!m_clientFinalLimiterTx) return;
    if (int16stereo.isEmpty()) return;

    const int samples = int16stereo.size() / static_cast<int>(sizeof(int16_t));
    if ((samples & 1) != 0) return;
    const int frames = samples / 2;

    m_clientFinalLimiterTxScratch.resize(samples * static_cast<int>(sizeof(float)));
    auto* f32 = reinterpret_cast<float*>(m_clientFinalLimiterTxScratch.data());
    const auto* i16 = reinterpret_cast<const int16_t*>(int16stereo.constData());
    for (int i = 0; i < samples; ++i) f32[i] = i16[i] / 32768.0f;

    m_clientFinalLimiterTx->process(f32, frames, 2);

    auto* out = reinterpret_cast<int16_t*>(int16stereo.data());
    for (int i = 0; i < samples; ++i) {
        out[i] = static_cast<int16_t>(
            std::clamp(f32[i] * 32768.0f, -32768.0f, 32767.0f));
    }
}

void AudioEngine::applyClientFinalLimiterTxFloat32(QByteArray& float32)
{
    if (!m_clientFinalLimiterTx) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientFinalLimiterTx->process(reinterpret_cast<float*>(float32.data()),
                                    frames, channels);
}

void AudioEngine::applyClientReverbTxFloat32(QByteArray& float32)
{
    if (!m_clientReverbTx || !m_clientReverbTx->isEnabled()) return;
    if (float32.isEmpty()) return;
    const int samples  = float32.size() / static_cast<int>(sizeof(float));
    const int channels = (samples % 2 == 0) ? 2 : 1;
    const int frames   = samples / channels;
    m_clientReverbTx->process(reinterpret_cast<float*>(float32.data()),
                              frames, channels);
}

void AudioEngine::applyClientTxDspInt16(QByteArray& int16stereo)
{
    // Order determines whether the compressor colours the raw mic signal
    // before the EQ shapes it (default, Pro-XL "tone shaping after
    // dynamics"), or the EQ shapes first and the compressor tames the
    // resulting peaks.  EQ's tap is always fed post-EQ so the analyzer
    // shows the final signal leaving the TX DSP chain.
    // Walk the packed chain-stage list and dispatch each entry to its
    // matching per-stage apply helper.  The audio thread loads the
    // full chain in one atomic read — each byte is a TxChainStage.
    const uint64_t packed = m_txChainPacked.load(std::memory_order_acquire);
    for (int i = 0; i < kMaxTxChainStages; ++i) {
        const auto stage = static_cast<TxChainStage>((packed >> (i * 8)) & 0xFF);
        switch (stage) {
            case TxChainStage::None:   return;     // end-of-list marker
            case TxChainStage::Eq:     applyClientEqTxInt16(int16stereo);    break;
            case TxChainStage::Comp:   applyClientCompTxInt16(int16stereo);  break;
            case TxChainStage::Gate:   applyClientGateTxInt16(int16stereo);  break;
            case TxChainStage::DeEss:  applyClientDeEssTxInt16(int16stereo); break;
            case TxChainStage::Tube:   applyClientTubeTxInt16(int16stereo);  break;
            // "Enh" is the legacy enum name; the user-facing label is
            // PUDU (Phase 5 exciter, Aphex/Behringer-modelled).
            case TxChainStage::Enh:    applyClientPuduTxInt16(int16stereo);  break;
            case TxChainStage::Reverb: applyClientReverbTxInt16(int16stereo); break;
        }
    }
}

void AudioEngine::applyClientTxDspFloat32(QByteArray& float32)
{
    const uint64_t packed = m_txChainPacked.load(std::memory_order_acquire);
    for (int i = 0; i < kMaxTxChainStages; ++i) {
        const auto stage = static_cast<TxChainStage>((packed >> (i * 8)) & 0xFF);
        switch (stage) {
            case TxChainStage::None:   return;
            case TxChainStage::Eq:     applyClientEqTxFloat32(float32);    break;
            case TxChainStage::Comp:   applyClientCompTxFloat32(float32);  break;
            case TxChainStage::Gate:   applyClientGateTxFloat32(float32);  break;
            case TxChainStage::DeEss:  applyClientDeEssTxFloat32(float32); break;
            case TxChainStage::Tube:   applyClientTubeTxFloat32(float32);  break;
            case TxChainStage::Enh:    applyClientPuduTxFloat32(float32);  break;
            case TxChainStage::Reverb: applyClientReverbTxFloat32(float32); break;
        }
    }
}

void AudioEngine::applyClientRxDspFloat32(QByteArray& float32)
{
    // Walk the packed RX chain-stage list and dispatch each entry to
    // its per-stage apply helper.  Phase 0 ships with no implemented
    // stages — every entry is a no-op until Phase 1+ slot in the DSP
    // classes (RX EQ first).  Same atomic-load pattern as TX so the
    // audio thread reads the entire chain order in one access.
    const uint64_t packed = m_rxChainPacked.load(std::memory_order_acquire);
    for (int i = 0; i < kMaxRxChainStages; ++i) {
        const auto stage = static_cast<RxChainStage>((packed >> (i * 8)) & 0xFF);
        switch (stage) {
            case RxChainStage::None:  return;     // end-of-list marker
            case RxChainStage::Eq:    /* TODO Phase 1 */ break;
            case RxChainStage::Gate:  /* TODO Phase 2 */ break;
            case RxChainStage::Comp:  /* TODO Phase 3 */ break;
            case RxChainStage::Tube:  /* TODO Phase 4 */ break;
            case RxChainStage::Pudu:  /* TODO Phase 5 */ break;
            case RxChainStage::DeEss: /* TODO Phase 6 */ break;
        }
    }
    (void)float32;  // unused until first stage lands
}

namespace {

// Pack a stage list into the uint64_t atomic format used by the audio
// thread.  Unused slots are TxChainStage::None (0).
uint64_t packChain(const QVector<AudioEngine::TxChainStage>& stages)
{
    uint64_t v = 0;
    const int n = std::min(static_cast<int>(stages.size()),
                           AudioEngine::kMaxTxChainStages);
    for (int i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(stages[i])) << (i * 8);
    }
    return v;
}

QVector<AudioEngine::TxChainStage> unpackChain(uint64_t v)
{
    QVector<AudioEngine::TxChainStage> out;
    out.reserve(AudioEngine::kMaxTxChainStages);
    for (int i = 0; i < AudioEngine::kMaxTxChainStages; ++i) {
        const auto s = static_cast<AudioEngine::TxChainStage>((v >> (i * 8)) & 0xFF);
        if (s == AudioEngine::TxChainStage::None) break;
        out.append(s);
    }
    return out;
}

// Map persisted stage names (human-readable in the XML settings) to
// the enum and back.  Keeping names textual means a settings file can
// be inspected and edited without decoding byte values.
QString stageName(AudioEngine::TxChainStage s)
{
    switch (s) {
        case AudioEngine::TxChainStage::Gate:   return "Gate";
        case AudioEngine::TxChainStage::Eq:     return "Eq";
        case AudioEngine::TxChainStage::DeEss:  return "DeEss";
        case AudioEngine::TxChainStage::Comp:   return "Comp";
        case AudioEngine::TxChainStage::Tube:   return "Tube";
        case AudioEngine::TxChainStage::Enh:    return "Enh";
        case AudioEngine::TxChainStage::Reverb: return "Reverb";
        case AudioEngine::TxChainStage::None:   return "";
    }
    return "";
}

AudioEngine::TxChainStage stageFromName(const QString& name)
{
    if (name == "Gate")   return AudioEngine::TxChainStage::Gate;
    if (name == "Eq")     return AudioEngine::TxChainStage::Eq;
    if (name == "DeEss")  return AudioEngine::TxChainStage::DeEss;
    if (name == "Comp")   return AudioEngine::TxChainStage::Comp;
    if (name == "Tube")   return AudioEngine::TxChainStage::Tube;
    if (name == "Enh")    return AudioEngine::TxChainStage::Enh;
    if (name == "Reverb") return AudioEngine::TxChainStage::Reverb;
    return AudioEngine::TxChainStage::None;
}

// Canonical default order for a fresh install — stages appear in the
// order they'll typically be wanted in the signal chain.  Only Eq and
// Comp do anything today; the others are no-ops until their DSP ships.
QVector<AudioEngine::TxChainStage> defaultChain()
{
    return {
        AudioEngine::TxChainStage::Gate,
        AudioEngine::TxChainStage::Eq,
        AudioEngine::TxChainStage::DeEss,
        AudioEngine::TxChainStage::Comp,
        AudioEngine::TxChainStage::Tube,
        AudioEngine::TxChainStage::Enh,
        AudioEngine::TxChainStage::Reverb,
    };
}

// ── RX chain helpers — parallel to the TX functions above ───────────────

uint64_t packRxChain(const QVector<AudioEngine::RxChainStage>& stages)
{
    uint64_t v = 0;
    const int n = std::min(static_cast<int>(stages.size()),
                           AudioEngine::kMaxRxChainStages);
    for (int i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(static_cast<uint8_t>(stages[i])) << (i * 8);
    }
    return v;
}

QVector<AudioEngine::RxChainStage> unpackRxChain(uint64_t v)
{
    QVector<AudioEngine::RxChainStage> out;
    out.reserve(AudioEngine::kMaxRxChainStages);
    for (int i = 0; i < AudioEngine::kMaxRxChainStages; ++i) {
        const auto s = static_cast<AudioEngine::RxChainStage>((v >> (i * 8)) & 0xFF);
        if (s == AudioEngine::RxChainStage::None) break;
        out.append(s);
    }
    return out;
}

QString rxStageName(AudioEngine::RxChainStage s)
{
    switch (s) {
        case AudioEngine::RxChainStage::Eq:    return "Eq";
        case AudioEngine::RxChainStage::Gate:  return "Gate";
        case AudioEngine::RxChainStage::Comp:  return "Comp";
        case AudioEngine::RxChainStage::Tube:  return "Tube";
        case AudioEngine::RxChainStage::Pudu:  return "Pudu";
        case AudioEngine::RxChainStage::DeEss: return "DeEss";
        case AudioEngine::RxChainStage::None:  return "";
    }
    return "";
}

AudioEngine::RxChainStage rxStageFromName(const QString& name)
{
    if (name == "Eq")    return AudioEngine::RxChainStage::Eq;
    if (name == "Gate")  return AudioEngine::RxChainStage::Gate;
    if (name == "Comp")  return AudioEngine::RxChainStage::Comp;
    if (name == "Tube")  return AudioEngine::RxChainStage::Tube;
    if (name == "Pudu")  return AudioEngine::RxChainStage::Pudu;
    if (name == "DeEss") return AudioEngine::RxChainStage::DeEss;
    return AudioEngine::RxChainStage::None;
}

// Canonical RX chain order (#2425):
//   [RADIO]→[ADSP]→[AGC-T]→[EQ]→[AGC-C]→[DESS]→[TUBE]→[EVO]→[SPEAK]
// RADIO / ADSP / SPEAK are status/launcher tiles handled by the chain
// widget; the audio path only sees the six user-controllable stages
// between them, in the order: Gate, Eq, Comp, DeEss, Tube, Pudu.
QVector<AudioEngine::RxChainStage> defaultRxChain()
{
    return {
        AudioEngine::RxChainStage::Gate,
        AudioEngine::RxChainStage::Eq,
        AudioEngine::RxChainStage::Comp,
        AudioEngine::RxChainStage::DeEss,
        AudioEngine::RxChainStage::Tube,
        AudioEngine::RxChainStage::Pudu,
    };
}

} // namespace

void AudioEngine::setTxChainStages(const QVector<TxChainStage>& stages)
{
    m_txChainPacked.store(packChain(stages), std::memory_order_release);
    QStringList names;
    for (auto s : stages) {
        const QString n = stageName(s);
        if (!n.isEmpty()) names.append(n);
    }
    AppSettings::instance().setValue(
        "ClientCompTxChainStages", names.join(","));
}

QVector<AudioEngine::TxChainStage> AudioEngine::txChainStages() const
{
    return unpackChain(m_txChainPacked.load(std::memory_order_acquire));
}

bool AudioEngine::isTxBypassed() const
{
    return !m_txBypassSnapshot.isEmpty();
}

void AudioEngine::setTxBypassed(bool on)
{
    if (on == isTxBypassed()) return;

    auto setStageEnabled = [this](TxChainStage s, bool enabled) {
        switch (s) {
            case TxChainStage::Eq:
                if (m_clientEqTx) {
                    m_clientEqTx->setEnabled(enabled);
                    saveClientEqSettings();
                }
                break;
            case TxChainStage::Comp:
                if (m_clientCompTx) {
                    m_clientCompTx->setEnabled(enabled);
                    saveClientCompSettings();
                }
                break;
            case TxChainStage::Gate:
                if (m_clientGateTx) {
                    m_clientGateTx->setEnabled(enabled);
                    saveClientGateSettings();
                }
                break;
            case TxChainStage::DeEss:
                if (m_clientDeEssTx) {
                    m_clientDeEssTx->setEnabled(enabled);
                    saveClientDeEssSettings();
                }
                break;
            case TxChainStage::Tube:
                if (m_clientTubeTx) {
                    m_clientTubeTx->setEnabled(enabled);
                    saveClientTubeSettings();
                }
                break;
            case TxChainStage::Enh:   // PUDU
                if (m_clientPuduTx) {
                    m_clientPuduTx->setEnabled(enabled);
                    saveClientPuduSettings();
                }
                break;
            case TxChainStage::Reverb:
                if (m_clientReverbTx) {
                    m_clientReverbTx->setEnabled(enabled);
                    saveClientReverbSettings();
                }
                break;
            case TxChainStage::None:
                break;
        }
    };

    auto isEnabled = [this](TxChainStage s) -> bool {
        switch (s) {
            case TxChainStage::Eq:     return m_clientEqTx     && m_clientEqTx->isEnabled();
            case TxChainStage::Comp:   return m_clientCompTx   && m_clientCompTx->isEnabled();
            case TxChainStage::Gate:   return m_clientGateTx   && m_clientGateTx->isEnabled();
            case TxChainStage::DeEss:  return m_clientDeEssTx  && m_clientDeEssTx->isEnabled();
            case TxChainStage::Tube:   return m_clientTubeTx   && m_clientTubeTx->isEnabled();
            case TxChainStage::Enh:    return m_clientPuduTx   && m_clientPuduTx->isEnabled();
            case TxChainStage::Reverb: return m_clientReverbTx && m_clientReverbTx->isEnabled();
            case TxChainStage::None:   return false;
        }
        return false;
    };

    static const QVector<TxChainStage> kAllStages{
        TxChainStage::Eq,
        TxChainStage::Comp,
        TxChainStage::Gate,
        TxChainStage::DeEss,
        TxChainStage::Tube,
        TxChainStage::Enh,
        TxChainStage::Reverb,
    };

    if (on) {
        m_txBypassSnapshot.clear();
        for (auto s : kAllStages) {
            if (isEnabled(s)) {
                m_txBypassSnapshot.append(s);
                setStageEnabled(s, false);
            }
        }
    } else {
        for (auto s : m_txBypassSnapshot) setStageEnabled(s, true);
        m_txBypassSnapshot.clear();
    }

    emit txBypassChanged(on);
}

bool AudioEngine::isRxBypassed() const
{
    return !m_rxBypassSnapshot.isEmpty();
}

void AudioEngine::setRxBypassed(bool on)
{
    if (on == isRxBypassed()) return;

    auto setStageEnabled = [this](RxChainStage s, bool enabled) {
        switch (s) {
            case RxChainStage::Eq:
                if (m_clientEqRx) {
                    m_clientEqRx->setEnabled(enabled);
                    saveClientEqSettings();
                }
                break;
            case RxChainStage::Gate:
                if (m_clientGateRx) {
                    m_clientGateRx->setEnabled(enabled);
                    saveClientGateRxSettings();
                }
                break;
            case RxChainStage::Comp:
                if (m_clientCompRx) {
                    m_clientCompRx->setEnabled(enabled);
                    saveClientCompRxSettings();
                }
                break;
            case RxChainStage::Tube:
                if (m_clientTubeRx) {
                    m_clientTubeRx->setEnabled(enabled);
                    saveClientTubeRxSettings();
                }
                break;
            case RxChainStage::Pudu:
                if (m_clientPuduRx) {
                    m_clientPuduRx->setEnabled(enabled);
                    saveClientPuduRxSettings();
                }
                break;
            case RxChainStage::DeEss:
                if (m_clientDeEssRx) {
                    m_clientDeEssRx->setEnabled(enabled);
                    saveClientDeEssRxSettings();
                }
                break;
            case RxChainStage::None:
                break;
        }
    };

    auto isEnabled = [this](RxChainStage s) -> bool {
        switch (s) {
            case RxChainStage::Eq:    return m_clientEqRx    && m_clientEqRx->isEnabled();
            case RxChainStage::Gate:  return m_clientGateRx  && m_clientGateRx->isEnabled();
            case RxChainStage::Comp:  return m_clientCompRx  && m_clientCompRx->isEnabled();
            case RxChainStage::Tube:  return m_clientTubeRx  && m_clientTubeRx->isEnabled();
            case RxChainStage::Pudu:  return m_clientPuduRx  && m_clientPuduRx->isEnabled();
            case RxChainStage::DeEss: return m_clientDeEssRx && m_clientDeEssRx->isEnabled();
            case RxChainStage::None:  return false;
        }
        return false;
    };

    static const QVector<RxChainStage> kAllStages{
        RxChainStage::Eq,
        RxChainStage::Gate,
        RxChainStage::Comp,
        RxChainStage::Tube,
        RxChainStage::Pudu,
        RxChainStage::DeEss,
    };

    if (on) {
        m_rxBypassSnapshot.clear();
        for (auto s : kAllStages) {
            if (isEnabled(s)) {
                m_rxBypassSnapshot.append(s);
                setStageEnabled(s, false);
            }
        }
    } else {
        for (auto s : m_rxBypassSnapshot) setStageEnabled(s, true);
        m_rxBypassSnapshot.clear();
    }

    emit rxBypassChanged(on);
}

void AudioEngine::setRxChainStages(const QVector<RxChainStage>& stages)
{
    m_rxChainPacked.store(packRxChain(stages), std::memory_order_release);
    QStringList names;
    for (auto s : stages) {
        const QString n = rxStageName(s);
        if (!n.isEmpty()) names.append(n);
    }
    AppSettings::instance().setValue(
        "ClientRxChainStages", names.join(","));
}

QVector<AudioEngine::RxChainStage> AudioEngine::rxChainStages() const
{
    return unpackRxChain(m_rxChainPacked.load(std::memory_order_acquire));
}

void AudioEngine::loadClientRxChainOrder()
{
    auto& s = AppSettings::instance();
    QVector<RxChainStage> stages;
    bool sawUnknown = false;
    const QString stored = s.value("ClientRxChainStages", "").toString();
    if (!stored.isEmpty()) {
        for (const QString& name : stored.split(',', Qt::SkipEmptyParts)) {
            const auto stage = rxStageFromName(name.trimmed());
            if (stage != RxChainStage::None) stages.append(stage);
            else                              sawUnknown = true;
        }
    }
    // Any unknown name in the stored list is a strong signal that the
    // settings file is from a different (or old) version of AetherSDR.
    // Reset to the canonical default rather than silently filtering the
    // unknown entries — that filtering shuffles the remaining stages
    // into a misleading order.
    const bool resetFromStale = sawUnknown;
    if (sawUnknown || stages.isEmpty()) stages = defaultRxChain();

    // Append any canonical stages missing from the loaded list so future
    // phases slot in without a migration.
    for (auto canon : defaultRxChain()) {
        if (!stages.contains(canon)) stages.append(canon);
    }
    m_rxChainPacked.store(packRxChain(stages), std::memory_order_release);

    // Overwrite the stale value on disk so the user's settings file
    // doesn't keep showing names from a previous build.
    if (resetFromStale) {
        QStringList names;
        for (auto st : stages) {
            const QString n = rxStageName(st);
            if (!n.isEmpty()) names.append(n);
        }
        s.setValue("ClientRxChainStages", names.join(","));
    }
}

void AudioEngine::saveClientRxChainOrder() const
{
    QStringList names;
    for (auto s : rxChainStages()) {
        const QString n = rxStageName(s);
        if (!n.isEmpty()) names.append(n);
    }
    AppSettings::instance().setValue("ClientRxChainStages", names.join(","));
}

void AudioEngine::setTxChainOrder(TxChainOrder order)
{
    // Legacy two-stage API used by the existing ClientCompEditor combo.
    // Find Eq and Comp in the current chain; swap their relative
    // positions to match the requested order, preserving every other
    // stage's slot.  Falls back to just [Eq, Comp] / [Comp, Eq] if
    // the chain is empty.
    auto stages = txChainStages();
    if (stages.isEmpty()) stages = defaultChain();

    const int eqIdx   = stages.indexOf(TxChainStage::Eq);
    const int compIdx = stages.indexOf(TxChainStage::Comp);
    if (eqIdx >= 0 && compIdx >= 0) {
        const bool compFirst = compIdx < eqIdx;
        const bool wantCompFirst = (order == TxChainOrder::CompThenEq);
        if (compFirst != wantCompFirst) stages.swapItemsAt(eqIdx, compIdx);
    }
    setTxChainStages(stages);
}

AudioEngine::TxChainOrder AudioEngine::txChainOrder() const
{
    const auto stages = txChainStages();
    const int eqIdx   = stages.indexOf(TxChainStage::Eq);
    const int compIdx = stages.indexOf(TxChainStage::Comp);
    if (eqIdx >= 0 && compIdx >= 0 && compIdx < eqIdx) {
        return TxChainOrder::CompThenEq;
    }
    return (eqIdx >= 0 && compIdx >= 0) ? TxChainOrder::EqThenComp
                                        : TxChainOrder::CompThenEq;
}

void AudioEngine::loadClientCompSettings()
{
    if (!m_clientCompTx) return;
    auto& s = AppSettings::instance();
    m_clientCompTx->setEnabled(
        s.value("ClientCompTxEnabled", "False").toString() == "True");
    m_clientCompTx->setThresholdDb(
        s.value("ClientCompTxThresholdDb", "-18.0").toFloat());
    m_clientCompTx->setRatio(
        s.value("ClientCompTxRatio", "3.0").toFloat());
    m_clientCompTx->setAttackMs(
        s.value("ClientCompTxAttackMs", "20.0").toFloat());
    m_clientCompTx->setReleaseMs(
        s.value("ClientCompTxReleaseMs", "200.0").toFloat());
    m_clientCompTx->setKneeDb(
        s.value("ClientCompTxKneeDb", "6.0").toFloat());
    m_clientCompTx->setMakeupDb(
        s.value("ClientCompTxMakeupDb", "0.0").toFloat());
    m_clientCompTx->setLimiterEnabled(
        s.value("ClientCompTxLimEnabled", "True").toString() == "True");
    m_clientCompTx->setLimiterCeilingDb(
        s.value("ClientCompTxLimCeilingDb", "-1.0").toFloat());

    // Load the generalised chain — stored as a comma-separated list of
    // stage names (e.g. "Gate,Eq,DeEss,Comp,Tube,Enh").  Migrate from
    // the older two-state ClientCompTxChainOrder (0 = CompThenEq,
    // 1 = EqThenComp) if present.
    QVector<TxChainStage> stages;
    const QString stored = s.value("ClientCompTxChainStages", "").toString();
    if (!stored.isEmpty()) {
        for (const QString& name : stored.split(',', Qt::SkipEmptyParts)) {
            const auto stage = stageFromName(name.trimmed());
            if (stage != TxChainStage::None) stages.append(stage);
        }
    } else if (s.contains("ClientCompTxChainOrder")) {
        const int legacy = s.value("ClientCompTxChainOrder", "0").toInt();
        // Preserve the user's Comp-vs-Eq preference from the old two-
        // option setting — bracket it with the default canonical
        // layout for the not-yet-implemented stages.
        stages = (legacy == 1)
            ? QVector<TxChainStage>{TxChainStage::Gate, TxChainStage::Eq,
                                     TxChainStage::DeEss, TxChainStage::Comp,
                                     TxChainStage::Tube, TxChainStage::Enh}
            : QVector<TxChainStage>{TxChainStage::Gate, TxChainStage::Comp,
                                     TxChainStage::Eq, TxChainStage::DeEss,
                                     TxChainStage::Tube, TxChainStage::Enh};
    }
    if (stages.isEmpty()) stages = defaultChain();

    // Append any canonical stages that are missing from the loaded
    // list — guarantees all 6 processor boxes are always visible in
    // the chain widget so users can reorder them ahead of time and
    // future phases slot in automatically without a second migration.
    for (auto canon : defaultChain()) {
        if (!stages.contains(canon)) stages.append(canon);
    }

    m_txChainPacked.store(packChain(stages), std::memory_order_release);
}

void AudioEngine::saveClientCompSettings() const
{
    if (!m_clientCompTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientCompTxEnabled",     toBool(m_clientCompTx->isEnabled()));
    s.setValue("ClientCompTxThresholdDb", QString::number(m_clientCompTx->thresholdDb()));
    s.setValue("ClientCompTxRatio",       QString::number(m_clientCompTx->ratio()));
    s.setValue("ClientCompTxAttackMs",    QString::number(m_clientCompTx->attackMs()));
    s.setValue("ClientCompTxReleaseMs",   QString::number(m_clientCompTx->releaseMs()));
    s.setValue("ClientCompTxKneeDb",      QString::number(m_clientCompTx->kneeDb()));
    s.setValue("ClientCompTxMakeupDb",    QString::number(m_clientCompTx->makeupDb()));
    s.setValue("ClientCompTxLimEnabled",  toBool(m_clientCompTx->limiterEnabled()));
    s.setValue("ClientCompTxLimCeilingDb",
               QString::number(m_clientCompTx->limiterCeilingDb()));
    // Chain stages persist as a comma-separated name list — already
    // written live by setTxChainStages() but re-emitted here so a
    // saveClientCompSettings() call dumps everything in sync.
    QStringList names;
    for (auto st : txChainStages()) {
        const QString n = stageName(st);
        if (!n.isEmpty()) names.append(n);
    }
    s.setValue("ClientCompTxChainStages", names.join(","));
}

void AudioEngine::loadClientCompRxSettings()
{
    if (!m_clientCompRx) return;
    auto& s = AppSettings::instance();
    m_clientCompRx->setEnabled(
        s.value("ClientCompRxEnabled", "False").toString() == "True");
    m_clientCompRx->setThresholdDb(
        s.value("ClientCompRxThresholdDb", "-18.0").toFloat());
    m_clientCompRx->setRatio(
        s.value("ClientCompRxRatio", "3.0").toFloat());
    m_clientCompRx->setAttackMs(
        s.value("ClientCompRxAttackMs", "20.0").toFloat());
    m_clientCompRx->setReleaseMs(
        s.value("ClientCompRxReleaseMs", "200.0").toFloat());
    m_clientCompRx->setKneeDb(
        s.value("ClientCompRxKneeDb", "6.0").toFloat());
    m_clientCompRx->setMakeupDb(
        s.value("ClientCompRxMakeupDb", "0.0").toFloat());
    m_clientCompRx->setLimiterEnabled(
        s.value("ClientCompRxLimEnabled", "True").toString() == "True");
    m_clientCompRx->setLimiterCeilingDb(
        s.value("ClientCompRxLimCeilingDb", "-1.0").toFloat());
}

void AudioEngine::saveClientCompRxSettings() const
{
    if (!m_clientCompRx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientCompRxEnabled",     toBool(m_clientCompRx->isEnabled()));
    s.setValue("ClientCompRxThresholdDb", QString::number(m_clientCompRx->thresholdDb()));
    s.setValue("ClientCompRxRatio",       QString::number(m_clientCompRx->ratio()));
    s.setValue("ClientCompRxAttackMs",    QString::number(m_clientCompRx->attackMs()));
    s.setValue("ClientCompRxReleaseMs",   QString::number(m_clientCompRx->releaseMs()));
    s.setValue("ClientCompRxKneeDb",      QString::number(m_clientCompRx->kneeDb()));
    s.setValue("ClientCompRxMakeupDb",    QString::number(m_clientCompRx->makeupDb()));
    s.setValue("ClientCompRxLimEnabled",  toBool(m_clientCompRx->limiterEnabled()));
    s.setValue("ClientCompRxLimCeilingDb",
               QString::number(m_clientCompRx->limiterCeilingDb()));
}

void AudioEngine::loadClientGateSettings()
{
    if (!m_clientGateTx) return;
    auto& s = AppSettings::instance();
    m_clientGateTx->setEnabled(
        s.value("ClientGateTxEnabled", "False").toString() == "True");
    // Mode first — it snaps ratio + floor to presets, so apply before
    // those two so a persisted mode doesn't overwrite a custom ratio.
    const int modeInt = s.value("ClientGateTxMode", "0").toInt();
    m_clientGateTx->setMode(modeInt == 1
        ? ClientGate::Mode::Gate
        : ClientGate::Mode::Expander);
    m_clientGateTx->setThresholdDb(
        s.value("ClientGateTxThresholdDb", "-40.0").toFloat());
    m_clientGateTx->setReturnDb(
        s.value("ClientGateTxReturnDb", "2.0").toFloat());
    m_clientGateTx->setRatio(
        s.value("ClientGateTxRatio", "2.0").toFloat());
    m_clientGateTx->setAttackMs(
        s.value("ClientGateTxAttackMs", "0.5").toFloat());
    m_clientGateTx->setHoldMs(
        s.value("ClientGateTxHoldMs", "20.0").toFloat());
    m_clientGateTx->setReleaseMs(
        s.value("ClientGateTxReleaseMs", "100.0").toFloat());
    m_clientGateTx->setFloorDb(
        s.value("ClientGateTxFloorDb", "-15.0").toFloat());
    m_clientGateTx->setLookaheadMs(
        s.value("ClientGateTxLookaheadMs", "0.0").toFloat());
}

void AudioEngine::saveClientGateSettings() const
{
    if (!m_clientGateTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientGateTxEnabled", toBool(m_clientGateTx->isEnabled()));
    s.setValue("ClientGateTxMode",
        QString::number(static_cast<int>(m_clientGateTx->mode())));
    s.setValue("ClientGateTxThresholdDb",
        QString::number(m_clientGateTx->thresholdDb()));
    s.setValue("ClientGateTxReturnDb",
        QString::number(m_clientGateTx->returnDb()));
    s.setValue("ClientGateTxRatio",
        QString::number(m_clientGateTx->ratio()));
    s.setValue("ClientGateTxAttackMs",
        QString::number(m_clientGateTx->attackMs()));
    s.setValue("ClientGateTxHoldMs",
        QString::number(m_clientGateTx->holdMs()));
    s.setValue("ClientGateTxReleaseMs",
        QString::number(m_clientGateTx->releaseMs()));
    s.setValue("ClientGateTxFloorDb",
        QString::number(m_clientGateTx->floorDb()));
    s.setValue("ClientGateTxLookaheadMs",
        QString::number(m_clientGateTx->lookaheadMs()));
}

void AudioEngine::loadClientGateRxSettings()
{
    if (!m_clientGateRx) return;
    auto& s = AppSettings::instance();
    m_clientGateRx->setEnabled(
        s.value("ClientGateRxEnabled", "False").toString() == "True");
    const int modeInt = s.value("ClientGateRxMode", "0").toInt();
    m_clientGateRx->setMode(modeInt == 1
        ? ClientGate::Mode::Gate
        : ClientGate::Mode::Expander);
    m_clientGateRx->setThresholdDb(
        s.value("ClientGateRxThresholdDb", "-40.0").toFloat());
    m_clientGateRx->setReturnDb(
        s.value("ClientGateRxReturnDb", "2.0").toFloat());
    m_clientGateRx->setRatio(
        s.value("ClientGateRxRatio", "2.0").toFloat());
    m_clientGateRx->setAttackMs(
        s.value("ClientGateRxAttackMs", "0.5").toFloat());
    m_clientGateRx->setHoldMs(
        s.value("ClientGateRxHoldMs", "20.0").toFloat());
    m_clientGateRx->setReleaseMs(
        s.value("ClientGateRxReleaseMs", "100.0").toFloat());
    m_clientGateRx->setFloorDb(
        s.value("ClientGateRxFloorDb", "-15.0").toFloat());
    m_clientGateRx->setLookaheadMs(
        s.value("ClientGateRxLookaheadMs", "0.0").toFloat());
}

void AudioEngine::saveClientGateRxSettings() const
{
    if (!m_clientGateRx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientGateRxEnabled", toBool(m_clientGateRx->isEnabled()));
    s.setValue("ClientGateRxMode",
        QString::number(static_cast<int>(m_clientGateRx->mode())));
    s.setValue("ClientGateRxThresholdDb",
        QString::number(m_clientGateRx->thresholdDb()));
    s.setValue("ClientGateRxReturnDb",
        QString::number(m_clientGateRx->returnDb()));
    s.setValue("ClientGateRxRatio",
        QString::number(m_clientGateRx->ratio()));
    s.setValue("ClientGateRxAttackMs",
        QString::number(m_clientGateRx->attackMs()));
    s.setValue("ClientGateRxHoldMs",
        QString::number(m_clientGateRx->holdMs()));
    s.setValue("ClientGateRxReleaseMs",
        QString::number(m_clientGateRx->releaseMs()));
    s.setValue("ClientGateRxFloorDb",
        QString::number(m_clientGateRx->floorDb()));
    s.setValue("ClientGateRxLookaheadMs",
        QString::number(m_clientGateRx->lookaheadMs()));
}

void AudioEngine::loadClientDeEssSettings()
{
    if (!m_clientDeEssTx) return;
    auto& s = AppSettings::instance();
    m_clientDeEssTx->setEnabled(
        s.value("ClientDeEssTxEnabled", "False").toString() == "True");
    m_clientDeEssTx->setFrequencyHz(
        s.value("ClientDeEssTxFrequencyHz", "6000.0").toFloat());
    m_clientDeEssTx->setQ(
        s.value("ClientDeEssTxQ", "2.0").toFloat());
    m_clientDeEssTx->setThresholdDb(
        s.value("ClientDeEssTxThresholdDb", "-30.0").toFloat());
    m_clientDeEssTx->setAmountDb(
        s.value("ClientDeEssTxAmountDb", "-6.0").toFloat());
    m_clientDeEssTx->setAttackMs(
        s.value("ClientDeEssTxAttackMs", "1.0").toFloat());
    m_clientDeEssTx->setReleaseMs(
        s.value("ClientDeEssTxReleaseMs", "100.0").toFloat());
}

void AudioEngine::saveClientDeEssSettings() const
{
    if (!m_clientDeEssTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientDeEssTxEnabled",
        toBool(m_clientDeEssTx->isEnabled()));
    s.setValue("ClientDeEssTxFrequencyHz",
        QString::number(m_clientDeEssTx->frequencyHz()));
    s.setValue("ClientDeEssTxQ",
        QString::number(m_clientDeEssTx->q()));
    s.setValue("ClientDeEssTxThresholdDb",
        QString::number(m_clientDeEssTx->thresholdDb()));
    s.setValue("ClientDeEssTxAmountDb",
        QString::number(m_clientDeEssTx->amountDb()));
    s.setValue("ClientDeEssTxAttackMs",
        QString::number(m_clientDeEssTx->attackMs()));
    s.setValue("ClientDeEssTxReleaseMs",
        QString::number(m_clientDeEssTx->releaseMs()));
}

void AudioEngine::loadClientDeEssRxSettings()
{
    if (!m_clientDeEssRx) return;
    auto& s = AppSettings::instance();
    m_clientDeEssRx->setEnabled(
        s.value("ClientDeEssRxEnabled", "False").toString() == "True");
    m_clientDeEssRx->setFrequencyHz(
        s.value("ClientDeEssRxFrequencyHz", "6000.0").toFloat());
    m_clientDeEssRx->setQ(
        s.value("ClientDeEssRxQ", "2.0").toFloat());
    m_clientDeEssRx->setThresholdDb(
        s.value("ClientDeEssRxThresholdDb", "-30.0").toFloat());
    m_clientDeEssRx->setAmountDb(
        s.value("ClientDeEssRxAmountDb", "-6.0").toFloat());
    m_clientDeEssRx->setAttackMs(
        s.value("ClientDeEssRxAttackMs", "1.0").toFloat());
    m_clientDeEssRx->setReleaseMs(
        s.value("ClientDeEssRxReleaseMs", "100.0").toFloat());
}

void AudioEngine::saveClientDeEssRxSettings() const
{
    if (!m_clientDeEssRx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientDeEssRxEnabled",
        toBool(m_clientDeEssRx->isEnabled()));
    s.setValue("ClientDeEssRxFrequencyHz",
        QString::number(m_clientDeEssRx->frequencyHz()));
    s.setValue("ClientDeEssRxQ",
        QString::number(m_clientDeEssRx->q()));
    s.setValue("ClientDeEssRxThresholdDb",
        QString::number(m_clientDeEssRx->thresholdDb()));
    s.setValue("ClientDeEssRxAmountDb",
        QString::number(m_clientDeEssRx->amountDb()));
    s.setValue("ClientDeEssRxAttackMs",
        QString::number(m_clientDeEssRx->attackMs()));
    s.setValue("ClientDeEssRxReleaseMs",
        QString::number(m_clientDeEssRx->releaseMs()));
}

void AudioEngine::loadClientTubeSettings()
{
    if (!m_clientTubeTx) return;
    auto& s = AppSettings::instance();
    m_clientTubeTx->setEnabled(
        s.value("ClientTubeTxEnabled", "False").toString() == "True");
    const int modelInt = s.value("ClientTubeTxModel", "0").toInt();
    m_clientTubeTx->setModel(
        modelInt == 1 ? ClientTube::Model::B :
        modelInt == 2 ? ClientTube::Model::C :
                        ClientTube::Model::A);
    m_clientTubeTx->setDriveDb(
        s.value("ClientTubeTxDriveDb", "0.0").toFloat());
    m_clientTubeTx->setBiasAmount(
        s.value("ClientTubeTxBias", "0.0").toFloat());
    m_clientTubeTx->setTone(
        s.value("ClientTubeTxTone", "0.0").toFloat());
    m_clientTubeTx->setOutputGainDb(
        s.value("ClientTubeTxOutputDb", "0.0").toFloat());
    m_clientTubeTx->setDryWet(
        s.value("ClientTubeTxDryWet", "1.0").toFloat());
    m_clientTubeTx->setEnvelopeAmount(
        s.value("ClientTubeTxEnvelope", "0.0").toFloat());
    m_clientTubeTx->setAttackMs(
        s.value("ClientTubeTxAttackMs", "5.0").toFloat());
    m_clientTubeTx->setReleaseMs(
        s.value("ClientTubeTxReleaseMs", "35.0").toFloat());
}

void AudioEngine::saveClientTubeSettings() const
{
    if (!m_clientTubeTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientTubeTxEnabled",  toBool(m_clientTubeTx->isEnabled()));
    s.setValue("ClientTubeTxModel",
        QString::number(static_cast<int>(m_clientTubeTx->model())));
    s.setValue("ClientTubeTxDriveDb",
        QString::number(m_clientTubeTx->driveDb()));
    s.setValue("ClientTubeTxBias",
        QString::number(m_clientTubeTx->biasAmount()));
    s.setValue("ClientTubeTxTone",
        QString::number(m_clientTubeTx->tone()));
    s.setValue("ClientTubeTxOutputDb",
        QString::number(m_clientTubeTx->outputGainDb()));
    s.setValue("ClientTubeTxDryWet",
        QString::number(m_clientTubeTx->dryWet()));
    s.setValue("ClientTubeTxEnvelope",
        QString::number(m_clientTubeTx->envelopeAmount()));
    s.setValue("ClientTubeTxAttackMs",
        QString::number(m_clientTubeTx->attackMs()));
    s.setValue("ClientTubeTxReleaseMs",
        QString::number(m_clientTubeTx->releaseMs()));
}

void AudioEngine::loadClientTubeRxSettings()
{
    if (!m_clientTubeRx) return;
    auto& s = AppSettings::instance();
    m_clientTubeRx->setEnabled(
        s.value("ClientTubeRxEnabled", "False").toString() == "True");
    const int modelInt = s.value("ClientTubeRxModel", "0").toInt();
    m_clientTubeRx->setModel(
        modelInt == 1 ? ClientTube::Model::B :
        modelInt == 2 ? ClientTube::Model::C :
                        ClientTube::Model::A);
    m_clientTubeRx->setDriveDb(
        s.value("ClientTubeRxDriveDb", "0.0").toFloat());
    m_clientTubeRx->setBiasAmount(
        s.value("ClientTubeRxBias", "0.0").toFloat());
    m_clientTubeRx->setTone(
        s.value("ClientTubeRxTone", "0.0").toFloat());
    m_clientTubeRx->setOutputGainDb(
        s.value("ClientTubeRxOutputDb", "0.0").toFloat());
    m_clientTubeRx->setDryWet(
        s.value("ClientTubeRxDryWet", "1.0").toFloat());
    m_clientTubeRx->setEnvelopeAmount(
        s.value("ClientTubeRxEnvelope", "0.0").toFloat());
    m_clientTubeRx->setAttackMs(
        s.value("ClientTubeRxAttackMs", "5.0").toFloat());
    m_clientTubeRx->setReleaseMs(
        s.value("ClientTubeRxReleaseMs", "35.0").toFloat());
}

void AudioEngine::saveClientTubeRxSettings() const
{
    if (!m_clientTubeRx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientTubeRxEnabled",  toBool(m_clientTubeRx->isEnabled()));
    s.setValue("ClientTubeRxModel",
        QString::number(static_cast<int>(m_clientTubeRx->model())));
    s.setValue("ClientTubeRxDriveDb",
        QString::number(m_clientTubeRx->driveDb()));
    s.setValue("ClientTubeRxBias",
        QString::number(m_clientTubeRx->biasAmount()));
    s.setValue("ClientTubeRxTone",
        QString::number(m_clientTubeRx->tone()));
    s.setValue("ClientTubeRxOutputDb",
        QString::number(m_clientTubeRx->outputGainDb()));
    s.setValue("ClientTubeRxDryWet",
        QString::number(m_clientTubeRx->dryWet()));
    s.setValue("ClientTubeRxEnvelope",
        QString::number(m_clientTubeRx->envelopeAmount()));
    s.setValue("ClientTubeRxAttackMs",
        QString::number(m_clientTubeRx->attackMs()));
    s.setValue("ClientTubeRxReleaseMs",
        QString::number(m_clientTubeRx->releaseMs()));
}

void AudioEngine::loadClientPuduSettings()
{
    if (!m_clientPuduTx) return;
    auto& s = AppSettings::instance();
    m_clientPuduTx->setEnabled(
        s.value("ClientPuduTxEnabled", "False").toString() == "True");
    const int modeInt = s.value("ClientPuduTxMode", "0").toInt();
    m_clientPuduTx->setMode(modeInt == 1
        ? ClientPudu::Mode::Behringer
        : ClientPudu::Mode::Aphex);
    m_clientPuduTx->setPooDriveDb(
        s.value("ClientPuduTxPooDriveDb", "6.0").toFloat());
    m_clientPuduTx->setPooTuneHz(
        s.value("ClientPuduTxPooTuneHz", "100.0").toFloat());
    m_clientPuduTx->setPooMix(
        s.value("ClientPuduTxPooMix", "0.3").toFloat());
    m_clientPuduTx->setDooTuneHz(
        s.value("ClientPuduTxDooTuneHz", "5000.0").toFloat());
    m_clientPuduTx->setDooHarmonicsDb(
        s.value("ClientPuduTxDooHarmonicsDb", "6.0").toFloat());
    m_clientPuduTx->setDooMix(
        s.value("ClientPuduTxDooMix", "0.3").toFloat());
}

void AudioEngine::setTxPostDspMonitor(ClientPuduMonitor* m) noexcept
{
    // Release-store so the audio thread sees the new pointer on its
    // next block via matching acquire-load at the tap site.
    m_txPostDspMonitor.store(m, std::memory_order_release);
}

void AudioEngine::setTxFinalMonitor(ClientPuduMonitor* m) noexcept
{
    m_txFinalMonitor.store(m, std::memory_order_release);
}

void AudioEngine::saveClientPuduSettings() const
{
    if (!m_clientPuduTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientPuduTxEnabled", toBool(m_clientPuduTx->isEnabled()));
    s.setValue("ClientPuduTxMode",
        QString::number(static_cast<int>(m_clientPuduTx->mode())));
    s.setValue("ClientPuduTxPooDriveDb",
        QString::number(m_clientPuduTx->pooDriveDb()));
    s.setValue("ClientPuduTxPooTuneHz",
        QString::number(m_clientPuduTx->pooTuneHz()));
    s.setValue("ClientPuduTxPooMix",
        QString::number(m_clientPuduTx->pooMix()));
    s.setValue("ClientPuduTxDooTuneHz",
        QString::number(m_clientPuduTx->dooTuneHz()));
    s.setValue("ClientPuduTxDooHarmonicsDb",
        QString::number(m_clientPuduTx->dooHarmonicsDb()));
    s.setValue("ClientPuduTxDooMix",
        QString::number(m_clientPuduTx->dooMix()));
}

void AudioEngine::loadClientPuduRxSettings()
{
    if (!m_clientPuduRx) return;
    auto& s = AppSettings::instance();
    m_clientPuduRx->setEnabled(
        s.value("ClientPuduRxEnabled", "False").toString() == "True");
    const int modeInt = s.value("ClientPuduRxMode", "0").toInt();
    m_clientPuduRx->setMode(modeInt == 1
        ? ClientPudu::Mode::Behringer
        : ClientPudu::Mode::Aphex);
    m_clientPuduRx->setPooDriveDb(
        s.value("ClientPuduRxPooDriveDb", "6.0").toFloat());
    m_clientPuduRx->setPooTuneHz(
        s.value("ClientPuduRxPooTuneHz", "100.0").toFloat());
    m_clientPuduRx->setPooMix(
        s.value("ClientPuduRxPooMix", "0.3").toFloat());
    m_clientPuduRx->setDooTuneHz(
        s.value("ClientPuduRxDooTuneHz", "5000.0").toFloat());
    m_clientPuduRx->setDooHarmonicsDb(
        s.value("ClientPuduRxDooHarmonicsDb", "6.0").toFloat());
    m_clientPuduRx->setDooMix(
        s.value("ClientPuduRxDooMix", "0.3").toFloat());
}

void AudioEngine::saveClientPuduRxSettings() const
{
    if (!m_clientPuduRx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientPuduRxEnabled", toBool(m_clientPuduRx->isEnabled()));
    s.setValue("ClientPuduRxMode",
        QString::number(static_cast<int>(m_clientPuduRx->mode())));
    s.setValue("ClientPuduRxPooDriveDb",
        QString::number(m_clientPuduRx->pooDriveDb()));
    s.setValue("ClientPuduRxPooTuneHz",
        QString::number(m_clientPuduRx->pooTuneHz()));
    s.setValue("ClientPuduRxPooMix",
        QString::number(m_clientPuduRx->pooMix()));
    s.setValue("ClientPuduRxDooTuneHz",
        QString::number(m_clientPuduRx->dooTuneHz()));
    s.setValue("ClientPuduRxDooHarmonicsDb",
        QString::number(m_clientPuduRx->dooHarmonicsDb()));
    s.setValue("ClientPuduRxDooMix",
        QString::number(m_clientPuduRx->dooMix()));
}

void AudioEngine::loadClientReverbSettings()
{
    if (!m_clientReverbTx) return;
    auto& s = AppSettings::instance();
    m_clientReverbTx->setEnabled(
        s.value("ClientReverbTxEnabled", "False").toString() == "True");
    m_clientReverbTx->setSize(
        s.value("ClientReverbTxSize", "0.5").toFloat());
    m_clientReverbTx->setDecayS(
        s.value("ClientReverbTxDecayS", "1.2").toFloat());
    m_clientReverbTx->setDamping(
        s.value("ClientReverbTxDamping", "0.5").toFloat());
    m_clientReverbTx->setPreDelayMs(
        s.value("ClientReverbTxPreDelayMs", "20.0").toFloat());
    m_clientReverbTx->setMix(
        s.value("ClientReverbTxMix", "0.15").toFloat());
}

void AudioEngine::saveClientReverbSettings()
{
    if (!m_clientReverbTx) return;
    auto& s = AppSettings::instance();
    auto toBool = [](bool on) { return on ? QString("True") : QString("False"); };
    s.setValue("ClientReverbTxEnabled",    toBool(m_clientReverbTx->isEnabled()));
    s.setValue("ClientReverbTxSize",
        QString::number(m_clientReverbTx->size()));
    s.setValue("ClientReverbTxDecayS",
        QString::number(m_clientReverbTx->decayS()));
    s.setValue("ClientReverbTxDamping",
        QString::number(m_clientReverbTx->damping()));
    s.setValue("ClientReverbTxPreDelayMs",
        QString::number(m_clientReverbTx->preDelayMs()));
    s.setValue("ClientReverbTxMix",
        QString::number(m_clientReverbTx->mix()));
    emit clientReverbStateChanged();
}

void AudioEngine::loadClientFinalLimiterSettings()
{
    if (!m_clientFinalLimiterTx) return;
    auto& s = AppSettings::instance();
    m_clientFinalLimiterTx->setEnabled(
        s.value("ClientFinalLimiterTxEnabled", "True").toString() == "True");
    m_clientFinalLimiterTx->setCeilingDb(
        s.value("ClientFinalLimiterTxCeilingDb", "-1.0").toFloat());
    m_clientFinalLimiterTx->setOutputTrimDb(
        s.value("ClientFinalLimiterTxOutputTrimDb", "0.0").toFloat());
    m_clientFinalLimiterTx->setDcBlockEnabled(
        s.value("ClientFinalLimiterTxDcBlock", "True").toString() == "True");
}

void AudioEngine::saveClientFinalLimiterSettings() const
{
    if (!m_clientFinalLimiterTx) return;
    auto& s = AppSettings::instance();
    s.setValue("ClientFinalLimiterTxEnabled",
        m_clientFinalLimiterTx->isEnabled() ? QString("True") : QString("False"));
    s.setValue("ClientFinalLimiterTxCeilingDb",
        QString::number(m_clientFinalLimiterTx->ceilingDb()));
    s.setValue("ClientFinalLimiterTxOutputTrimDb",
        QString::number(m_clientFinalLimiterTx->outputTrimDb()));
    s.setValue("ClientFinalLimiterTxDcBlock",
        m_clientFinalLimiterTx->dcBlockEnabled() ? QString("True") : QString("False"));
}

void AudioEngine::loadClientQuindarSettings()
{
    if (!m_clientQuindarTone) return;
    auto& s = AppSettings::instance();
    m_clientQuindarTone->setEnabled(
        s.value("QuindarEnabled", "False").toString() == "True");
    const QString styleStr = s.value("QuindarStyle", "Tone").toString();
    m_clientQuindarTone->setStyle(styleStr == "Morse"
        ? ClientQuindarTone::Style::Morse
        : ClientQuindarTone::Style::Tone);
    m_clientQuindarTone->setLevelDb(
        s.value("QuindarLevelDb", "-6.0").toFloat());
    m_clientQuindarTone->setIntroFreqHz(
        s.value("QuindarIntroFreqHz", "2525.0").toFloat());
    m_clientQuindarTone->setOutroFreqHz(
        s.value("QuindarOutroFreqHz", "2475.0").toFloat());
    m_clientQuindarTone->setDurationMs(
        s.value("QuindarDurationMs", "250").toInt());
    m_clientQuindarTone->setMorseWpm(
        s.value("QuindarMorseWpm", "45").toInt());
    m_clientQuindarTone->setMorsePitchHz(
        s.value("QuindarMorsePitchHz", "750.0").toFloat());
}

void AudioEngine::saveClientQuindarSettings() const
{
    if (!m_clientQuindarTone) return;
    auto& s = AppSettings::instance();
    s.setValue("QuindarEnabled",
        m_clientQuindarTone->isEnabled() ? QString("True") : QString("False"));
    s.setValue("QuindarStyle",
        m_clientQuindarTone->style() == ClientQuindarTone::Style::Morse
            ? QString("Morse") : QString("Tone"));
    s.setValue("QuindarLevelDb",
        QString::number(m_clientQuindarTone->levelDb()));
    s.setValue("QuindarIntroFreqHz",
        QString::number(m_clientQuindarTone->introFreqHz()));
    s.setValue("QuindarOutroFreqHz",
        QString::number(m_clientQuindarTone->outroFreqHz()));
    s.setValue("QuindarDurationMs",
        QString::number(m_clientQuindarTone->durationMs()));
    s.setValue("QuindarMorseWpm",
        QString::number(m_clientQuindarTone->morseWpm()));
    s.setValue("QuindarMorsePitchHz",
        QString::number(m_clientQuindarTone->morsePitchHz()));
}

static QString wisdomDir()
{
#ifdef _WIN32
    // Windows: use %APPDATA%/AetherSDR/
    QString dir = QDir::homePath() + "/AppData/Roaming/AetherSDR/";
#else
    QString dir = QDir::homePath() + "/.config/AetherSDR/AetherSDR/";
#endif
    QDir().mkpath(dir);
    return dir;
}

QString AudioEngine::wisdomFilePath()
{
    return wisdomDir() + "aethersdr_fftw_wisdom";
}

bool AudioEngine::needsWisdomGeneration()
{
#ifndef HAVE_FFTW3
    return false;
#else
    const QString path = wisdomFilePath();
    if (!QFile::exists(path))
        return true;

    if (!SpectralNR::loadWisdom(wisdomDir().toStdString())) {
        qCWarning(lcAudio) << "AudioEngine: NR2 FFTW wisdom exists but could not be imported;"
                           << "will regenerate" << path;
        return true;
    }

    return false;
#endif
}

void AudioEngine::generateWisdom(std::function<void(int,int,const std::string&)> progress)
{
    SpectralNR::generateWisdom(wisdomDir().toStdString(), std::move(progress));
}

void AudioEngine::setNr2Enabled(bool on)
{
    if (m_nr2Enabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
    if (on) {
        // Disable all other NR modes — they're mutually exclusive
        if (m_rn2Enabled)  setRn2Enabled(false);
        if (m_bnrEnabled)  setBnrEnabled(false);
        if (m_nr4Enabled)  setNr4Enabled(false);
        if (m_dfnrEnabled) setDfnrEnabled(false);
        if (m_mnrEnabled)  setMnrEnabled(false);
        // Wisdom should already be generated by MainWindow::enableNr2WithWisdom().
        // Import only here: full wisdom generation can take minutes and must
        // never run on the audio worker thread.
#ifdef HAVE_FFTW3
        if (!SpectralNR::loadWisdom(wisdomDir().toStdString()))
            qCWarning(lcAudio) << "AudioEngine: NR2 FFTW wisdom unavailable on enable;"
                               << "using runtime FFTW_MEASURE plans";
#endif
        m_nr2 = std::make_unique<SpectralNR>(256, DEFAULT_SAMPLE_RATE);
        if (m_nr2->hasPlanFailed()) {
            qCWarning(lcAudio) << "AudioEngine: NR2 FFTW plan creation failed — disabling";
            m_nr2.reset();
            emit nr2EnabledChanged(false);
            return;
        }
        // Restore user-adjusted parameters from AppSettings
        auto& s = AppSettings::instance();
        m_nr2->setGainMax(s.value("NR2GainMax", "1.00").toFloat());  // default 1.0 = no amplification (#1507)
        m_nr2->setGainSmooth(s.value("NR2GainSmooth", "0.85").toFloat());
        m_nr2->setQspp(s.value("NR2Qspp", "0.20").toFloat());
        m_nr2->setGainMethod(s.value("NR2GainMethod", "2").toInt());
        m_nr2->setNpeMethod(s.value("NR2NpeMethod", "0").toInt());
        m_nr2->setAeFilter(s.value("NR2AeFilter", "True").toString() == "True");
        m_nr2Enabled = true;
    } else {
        m_nr2Enabled = false;
        m_nr2.reset();
    }
    qCDebug(lcAudio) << "AudioEngine: NR2" << (on ? "enabled" : "disabled");
    emit nr2EnabledChanged(on);
}

void AudioEngine::setNr2GainMax(float v)    { if (m_nr2) m_nr2->setGainMax(v); }
void AudioEngine::setNr2Qspp(float v)      { if (m_nr2) m_nr2->setQspp(v); }
void AudioEngine::setNr2GainSmooth(float v) { if (m_nr2) m_nr2->setGainSmooth(v); }
void AudioEngine::setNr2GainMethod(int m)   { if (m_nr2) m_nr2->setGainMethod(m); }
void AudioEngine::setNr2NpeMethod(int m)    { if (m_nr2) m_nr2->setNpeMethod(m); }
void AudioEngine::setNr2AeFilter(bool on)   { if (m_nr2) m_nr2->setAeFilter(on); }


#ifdef HAVE_SPECBLEACH

void AudioEngine::setNr4Enabled(bool on)
{
    if (m_nr4Enabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
    if (on) {
        if (m_nr2Enabled)  setNr2Enabled(false);
        if (m_rn2Enabled)  setRn2Enabled(false);
        if (m_bnrEnabled)  setBnrEnabled(false);
        if (m_dfnrEnabled) setDfnrEnabled(false);
        if (m_mnrEnabled)  setMnrEnabled(false);
        m_nr4 = std::make_unique<SpecbleachFilter>();
        if (!m_nr4->isValid()) {
            qCWarning(lcAudio) << "AudioEngine: NR4 initialization failed";
            m_nr4.reset();
            emit nr4EnabledChanged(false);
            return;
        }
        // Restore all saved params
        auto& s = AppSettings::instance();
        m_nr4->setReductionAmount(s.value("NR4ReductionAmount", "10.0").toFloat());
        m_nr4->setSmoothingFactor(s.value("NR4SmoothingFactor", "0.0").toFloat());
        m_nr4->setWhiteningFactor(s.value("NR4WhiteningFactor", "0.0").toFloat());
        m_nr4->setAdaptiveNoise(s.value("NR4AdaptiveNoise", "True").toString() == "True");
        m_nr4->setNoiseEstimationMethod(s.value("NR4NoiseEstimationMethod", "0").toInt());
        m_nr4->setMaskingDepth(s.value("NR4MaskingDepth", "0.50").toFloat());
        m_nr4->setSuppressionStrength(s.value("NR4SuppressionStrength", "0.50").toFloat());
        m_nr4Enabled = true;
    } else {
        m_nr4Enabled = false;
        m_nr4.reset();
    }
    qCDebug(lcAudio) << "AudioEngine: NR4" << (on ? "enabled" : "disabled");
    emit nr4EnabledChanged(on);
}

void AudioEngine::setNr4ReductionAmount(float dB) { if (m_nr4) m_nr4->setReductionAmount(dB); }
void AudioEngine::setNr4SmoothingFactor(float pct) { if (m_nr4) m_nr4->setSmoothingFactor(pct); }
void AudioEngine::setNr4WhiteningFactor(float pct) { if (m_nr4) m_nr4->setWhiteningFactor(pct); }
void AudioEngine::setNr4AdaptiveNoise(bool on) { if (m_nr4) m_nr4->setAdaptiveNoise(on); }
void AudioEngine::setNr4NoiseEstimationMethod(int m) { if (m_nr4) m_nr4->setNoiseEstimationMethod(m); }
void AudioEngine::setNr4MaskingDepth(float v) { if (m_nr4) m_nr4->setMaskingDepth(v); }
void AudioEngine::setNr4SuppressionStrength(float v) { if (m_nr4) m_nr4->setSuppressionStrength(v); }
#else // !HAVE_SPECBLEACH — stubs
void AudioEngine::setNr4Enabled(bool on) { if (on) emit nr4EnabledChanged(false); }
void AudioEngine::setNr4ReductionAmount(float) {}
void AudioEngine::setNr4SmoothingFactor(float) {}
void AudioEngine::setNr4WhiteningFactor(float) {}
void AudioEngine::setNr4AdaptiveNoise(bool) {}
void AudioEngine::setNr4NoiseEstimationMethod(int) {}
void AudioEngine::setNr4MaskingDepth(float) {}
void AudioEngine::setNr4SuppressionStrength(float) {}
#endif // HAVE_SPECBLEACH

// MNR (macOS MMSE-Wiener noise reduction)
void AudioEngine::setMnrEnabled(bool on)
{
    if (m_mnrEnabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
#ifdef __APPLE__
    if (on) {
        // Disable all other noise-reduction modes — they're mutually exclusive
        if (m_nr2Enabled)  setNr2Enabled(false);
        if (m_rn2Enabled)  setRn2Enabled(false);
        if (m_bnrEnabled)  setBnrEnabled(false);
        if (m_nr4Enabled)  setNr4Enabled(false);
        if (m_dfnrEnabled) setDfnrEnabled(false);
        m_mnr = std::make_unique<MacNRFilter>();
        if (!m_mnr->isValid()) {
            qCWarning(lcAudio) << "AudioEngine: MNR vDSP setup failed — disabling";
            m_mnr.reset();
            return;
        }
        // Restore strength from settings (default 1.0 = full suppression)
        m_mnrStrength.store(std::clamp(
            AppSettings::instance().value("MnrStrength", "1.00").toFloat(), 0.0f, 1.0f));
        m_mnr->setStrength(m_mnrStrength.load());
    } else {
        m_mnr.reset();
    }
#endif
    m_mnrEnabled = on;
    emit mnrEnabledChanged(on);
}

void AudioEngine::setMnrStrength(float normalized)
{
    m_mnrStrength.store(std::clamp(normalized, 0.0f, 1.0f));
    AppSettings::instance().setValue("MnrStrength",
        QString::number(m_mnrStrength.load(), 'f', 2));
#ifdef __APPLE__
    if (m_mnr) m_mnr->setStrength(m_mnrStrength.load());
#endif
}

float AudioEngine::mnrStrength() const
{
    return m_mnrStrength.load();
}

void AudioEngine::setRn2Enabled(bool on)
{
    if (m_rn2Enabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
    if (on) {
        // Disable all other NR modes — they're mutually exclusive
        if (m_nr2Enabled)  setNr2Enabled(false);
        if (m_bnrEnabled)  setBnrEnabled(false);
        if (m_nr4Enabled)  setNr4Enabled(false);
        if (m_dfnrEnabled) setDfnrEnabled(false);
        if (m_mnrEnabled)  setMnrEnabled(false);
        m_rn2 = std::make_unique<RNNoiseFilter>();
        if (!m_rn2->isValid()) {
            qCWarning(lcAudio) << "AudioEngine: RN2 rnnoise_create() failed — disabling";
            m_rn2.reset();
            emit rn2EnabledChanged(false);
            return;
        }
        // Set flag AFTER object is fully constructed
        m_rn2Enabled = true;
    } else {
        m_rn2Enabled = false;
        m_rn2.reset();
    }
    qCDebug(lcAudio) << "AudioEngine: RN2 (RNNoise)" << (on ? "enabled" : "disabled");
    emit rn2EnabledChanged(on);
}

// ─── BNR (NVIDIA NIM GPU noise removal) ──────────────────────────────────────

void AudioEngine::setBnrEnabled(bool on)
{
    if (m_bnrEnabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
    if (on) {
        // Mutual exclusion with all other NR modes
        if (m_nr2Enabled)  setNr2Enabled(false);
        if (m_rn2Enabled)  setRn2Enabled(false);
        if (m_nr4Enabled)  setNr4Enabled(false);
        if (m_dfnrEnabled) setDfnrEnabled(false);
        if (m_mnrEnabled)  setMnrEnabled(false);

        m_bnr = std::make_unique<NvidiaBnrFilter>(this);
        connect(m_bnr.get(), &NvidiaBnrFilter::connectionChanged,
                this, &AudioEngine::bnrConnectionChanged);

        // Resamplers: 24kHz mono ↔ 48kHz mono
        // BNR returns variable-sized chunks (up to 200ms = 9600 samples at 48kHz),
        // so use a large maxBlockSamples to avoid r8brain buffer overflow.
        m_bnrUp   = std::make_unique<Resampler>(24000, 48000, 16384);
        m_bnrDown = std::make_unique<Resampler>(48000, 24000, 16384);
        m_bnrOutBuf.clear();
        m_bnrPrimed = false;
        // Set flag AFTER objects are fully constructed
        m_bnrEnabled = true;

        // Try connecting — if the container is still booting, retry with a timer.
        if (!m_bnr->connectToServer(m_bnrAddress)) {
            // Retry up to 5 times, 2s apart
            auto* retryTimer = new QTimer(this);
            retryTimer->setInterval(2000);
            auto retryCount = std::make_shared<int>(0);
            connect(retryTimer, &QTimer::timeout, this,
                    [this, retryTimer, retryCount]() {
                if (!m_bnr || *retryCount >= 5) {
                    retryTimer->stop();
                    retryTimer->deleteLater();
                    if (m_bnr && !m_bnr->isConnected()) {
                        qCWarning(lcAudio) << "AudioEngine: BNR connect failed after retries";
                        m_bnr.reset();
                        m_bnrUp.reset();
                        m_bnrDown.reset();
                        m_bnrEnabled = false;
                        emit bnrEnabledChanged(false);
                    }
                    return;
                }
                ++(*retryCount);
                qDebug() << "AudioEngine: BNR connect retry" << *retryCount << "of 5";
                if (m_bnr->connectToServer(m_bnrAddress)) {
                    retryTimer->stop();
                    retryTimer->deleteLater();
                }
            });
            retryTimer->start();
        }
    } else {
        m_bnrEnabled = false;
        if (m_bnr) m_bnr->disconnect();
        m_bnr.reset();
        m_bnrUp.reset();
        m_bnrDown.reset();
    }
    qCDebug(lcAudio) << "AudioEngine: BNR (NVIDIA NIM)" << (on ? "enabled" : "disabled");
    emit bnrEnabledChanged(on);
}

void AudioEngine::setBnrAddress(const QString& addr)
{
    m_bnrAddress = addr;
}

void AudioEngine::setBnrIntensity(float ratio)
{
    if (m_bnr) m_bnr->setIntensityRatio(ratio);
}

float AudioEngine::bnrIntensity() const
{
    return m_bnr ? m_bnr->intensityRatio() : 1.0f;
}

bool AudioEngine::bnrConnected() const
{
    return m_bnr && m_bnr->isConnected();
}

void AudioEngine::processBnr(const QByteArray& stereoPcm)
{
    // ── Feed input to BNR container (non-blocking) ───────────────────────

    // 1. 24kHz stereo float32 → 24kHz mono float32 (average L+R)
    const auto* src = reinterpret_cast<const float*>(stereoPcm.constData());
    const int stereoFrames = stereoPcm.size() / (2 * static_cast<int>(sizeof(float)));

    if (static_cast<int>(m_nr2Mono.size()) < stereoFrames)
        m_nr2Mono.resize(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i)
        m_nr2Mono[i] = (src[2 * i] + src[2 * i + 1]) * 0.5f;

    // 2. 24kHz mono float32 → 48kHz mono float32 (r8brain)
    QByteArray mono48k = m_bnrUp->process(m_nr2Mono.data(), stereoFrames);

    // 3. Already float32 — pass directly to BNR
    const auto* mono48kSrc = reinterpret_cast<const float*>(mono48k.constData());
    const int mono48kSamples = mono48k.size() / static_cast<int>(sizeof(float));

    // 4. Push to BNR container (non-blocking), pull any denoised data
    QByteArray denoised = m_bnr->process(mono48kSrc, mono48kSamples);

    // ── Convert denoised data and add to jitter buffer ───────────────────

    if (!denoised.isEmpty()) {
        // 5. BNR returns float32 48kHz mono — downsample to 24kHz mono float32
        const auto* df = reinterpret_cast<const float*>(denoised.constData());
        const int dn = denoised.size() / static_cast<int>(sizeof(float));

        QByteArray mono24k = m_bnrDown->process(df, dn);

        // 6. Mono float32 → stereo float32 (duplicate L=R)
        const auto* m24 = reinterpret_cast<const float*>(mono24k.constData());
        const int n24 = mono24k.size() / static_cast<int>(sizeof(float));
        QByteArray stereo(n24 * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* ds = reinterpret_cast<float*>(stereo.data());
        for (int i = 0; i < n24; ++i) {
            ds[2 * i]     = m24[i];
            ds[2 * i + 1] = m24[i];
        }

        m_bnrOutBuf.append(stereo);

        // Cap jitter buffer at ~500ms (24kHz stereo float32 = 192000 bytes/sec)
        constexpr int maxBufBytes = 96000;  // 500ms
        if (m_bnrOutBuf.size() > maxBufBytes)
            m_bnrOutBuf.remove(0, m_bnrOutBuf.size() - maxBufBytes);
    }

    // ── Play from jitter buffer ──────────────────────────────────────────

    // Wait for ~50ms of buffered audio before starting playback (priming)
    constexpr int primeBytes = 9600;  // 50ms of 24kHz stereo float32
    if (!m_bnrPrimed) {
        if (m_bnrOutBuf.size() >= primeBytes)
            m_bnrPrimed = true;
        else
            return;  // still priming — silence (no audio output)
    }

    // Play the same amount of audio as the incoming chunk to maintain sync
    const int wantBytes = stereoPcm.size();
    if (m_bnrOutBuf.size() >= wantBytes) {
        QByteArray chunk = m_bnrOutBuf.left(wantBytes);
        m_bnrOutBuf.remove(0, wantBytes);

        if (m_audioDevice && m_audioDevice->isOpen()) {
            const int scopeSampleRate = m_resampleTo48k ? 48000 : DEFAULT_SAMPLE_RATE;
            const QByteArray& resampled = m_resampleTo48k ? resampleStereo(chunk) : chunk;
            const QByteArray* output = &resampled;
            QByteArray trimmed;
            const float trimDb = m_rxOutputTrimDb.load();
            if (std::fabs(trimDb) > 0.01f) {
                const float gain = std::pow(10.0f, trimDb / 20.0f);
                trimmed.resize(resampled.size());
                const auto* src = reinterpret_cast<const float*>(resampled.constData());
                auto* dst = reinterpret_cast<float*>(trimmed.data());
                const int nSamples = resampled.size() / static_cast<int>(sizeof(float));
                for (int i = 0; i < nSamples; ++i) dst[i] = src[i] * gain;
                output = &trimmed;
            }
            m_rxBuffer.append(*output);
            emitScopeFromFloat32Stereo(*output, scopeSampleRate, false);
            emitRxPostChainScopeFromFloat32Stereo(*output, scopeSampleRate);
            updateRxBufferStats();
        }
        emit levelChanged(computeRMS(chunk));
    }
    // If buffer underrun, skip this callback (brief silence, not choppy)
}

// ─── DFNR (DeepFilterNet3 neural noise reduction) ────────────────────────────

#ifdef HAVE_DFNR

void AudioEngine::setDfnrEnabled(bool on)
{
    if (m_dfnrEnabled == on) return;
    std::lock_guard<std::recursive_mutex> lock(m_dspMutex);
    if (on) {
        // Mutual exclusion with all other NR modes
        if (m_nr2Enabled)  setNr2Enabled(false);
        if (m_rn2Enabled)  setRn2Enabled(false);
        if (m_nr4Enabled)  setNr4Enabled(false);
        if (m_bnrEnabled)  setBnrEnabled(false);
        if (m_mnrEnabled)  setMnrEnabled(false);
        m_dfnr = std::make_unique<DeepFilterFilter>();
        if (!m_dfnr->isValid()) {
            qCWarning(lcAudio) << "AudioEngine: DFNR df_create() failed — disabling";
            m_dfnr.reset();
            emit dfnrEnabledChanged(false);
            return;
        }
        // Restore saved attenuation limit
        auto& s = AppSettings::instance();
        m_dfnr->setAttenLimit(s.value("DfnrAttenLimit", "100").toFloat());
        m_dfnr->setPostFilterBeta(s.value("DfnrPostFilterBeta", "0.0").toFloat());
        // Set flag AFTER object is fully constructed
        m_dfnrEnabled = true;
    } else {
        m_dfnrEnabled = false;
        m_dfnr.reset();
    }
    qCDebug(lcAudio) << "AudioEngine: DFNR (DeepFilterNet3)" << (on ? "enabled" : "disabled");
    emit dfnrEnabledChanged(on);
}

void AudioEngine::setDfnrAttenLimit(float db)
{
    if (m_dfnr) m_dfnr->setAttenLimit(db);
}

float AudioEngine::dfnrAttenLimit() const
{
    return m_dfnr ? m_dfnr->attenLimit() : 100.0f;
}

void AudioEngine::setDfnrPostFilterBeta(float beta)
{
    if (m_dfnr) m_dfnr->setPostFilterBeta(beta);
}

#else // !HAVE_DFNR — stubs
void AudioEngine::setDfnrEnabled(bool) {}
void AudioEngine::setDfnrAttenLimit(float) {}
float AudioEngine::dfnrAttenLimit() const { return 100.0f; }
void AudioEngine::setDfnrPostFilterBeta(float) {}
#endif // HAVE_DFNR

void AudioEngine::processNr2(const QByteArray& stereoPcm)
{
    const int totalFloats = stereoPcm.size() / static_cast<int>(sizeof(float));
    const int stereoFrames = totalFloats / 2;
    const auto* src = reinterpret_cast<const float*>(stereoPcm.constData());

    // Resize pre-allocated buffers if needed
    if (static_cast<int>(m_nr2Mono.size()) < stereoFrames) {
        m_nr2Mono.resize(stereoFrames);
        m_nr2Processed.resize(stereoFrames);
    }

    // Stereo float32 → mono float32 (average L+R)
    for (int i = 0; i < stereoFrames; ++i)
        m_nr2Mono[i] = (src[2 * i] + src[2 * i + 1]) * 0.5f;

    // Process through SpectralNR (float32 I/O)
    m_nr2->process(m_nr2Mono.data(), m_nr2Processed.data(), stereoFrames);

    // Mono float32 → stereo float32, then re-apply the pan the radio had set
    // before NR mono-mixed it away (#1460).
    // Hard-clamp to ±1.0: if gainMax was tuned above 1.0 (not recommended),
    // unclamped samples would cause digital crackling at the audio sink (#1507).
    const int outBytes = stereoFrames * 2 * static_cast<int>(sizeof(float));
    m_nr2Output.resize(outBytes);
    auto* dst = reinterpret_cast<float*>(m_nr2Output.data());
    for (int i = 0; i < stereoFrames; ++i) {
        const float s = std::clamp(m_nr2Processed[i], -1.0f, 1.0f);
        dst[2 * i]     = s;
        dst[2 * i + 1] = s;
    }
    applyRxPanInPlace(dst, stereoFrames, m_rxPan.load());
}

QByteArray AudioEngine::applyBoost(const QByteArray& pcm, float gain) const
{
    const int nSamples = pcm.size() / sizeof(int16_t);
    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    QByteArray out(pcm.size(), Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < nSamples; ++i) {
        float s = src[i] * gain;
        // Soft clamp to avoid harsh digital clipping
        if (s > 32767.0f) s = 32767.0f;
        else if (s < -32767.0f) s = -32767.0f;
        dst[i] = static_cast<int16_t>(s);
    }
    return out;
}

float AudioEngine::computeRMS(const QByteArray& pcm) const
{
    const int samples = pcm.size() / static_cast<int>(sizeof(float));
    if (samples == 0) return 0.0f;

    const float* data = reinterpret_cast<const float*>(pcm.constData());
    double sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        sum += static_cast<double>(data[i]) * data[i];
    }
    return static_cast<float>(std::sqrt(sum / samples));
}

void AudioEngine::accumulatePcMicMeterInt16Stereo(const QByteArray& int16stereo)
{
    const auto block = TxMicChannelNormalizer::measureInt16StereoLevelBlock(int16stereo);
    if (block.frames <= 0) {
        return;
    }

    m_pcMicPeak = std::max(m_pcMicPeak, block.peak);
    m_pcMicSumSq += block.sumSq;
    m_pcMicSampleCount += block.frames;
    if (m_pcMicSampleCount >= kMicMeterWindowSamples) {
        const float rms = static_cast<float>(std::sqrt(m_pcMicSumSq / m_pcMicSampleCount));
        emit pcMicLevelChanged(TxMicChannelNormalizer::dbfs(m_pcMicPeak),
                               TxMicChannelNormalizer::dbfs(rms));
        m_pcMicPeak = 0.0f;
        m_pcMicSumSq = 0.0;
        m_pcMicSampleCount = 0;
    }
}

void AudioEngine::logTxInputChannelDiagnostics(const TxMicChannelNormalizer::Diagnostics& diagnostics,
                                               const char* route)
{
    if (!diagnostics.oneSidedStereo) {
        return;
    }

    QElapsedTimer& throttle = (route && std::strcmp(route, "DAX radio") == 0)
        ? m_lastDaxRadioChannelLog
        : m_lastTxMicChannelLog;
    if (throttle.isValid() && throttle.elapsed() < 1000) {
        return;
    }

    if (throttle.isValid())
        throttle.restart();
    else
        throttle.start();

    qCDebug(lcAudio) << "AudioEngine:" << (route ? route : "TX mic")
                     << "one-sided stereo input"
                     << "leftRmsDbfs:" << TxMicChannelNormalizer::dbfs(diagnostics.leftRms)
                     << "rightRmsDbfs:" << TxMicChannelNormalizer::dbfs(diagnostics.rightRms)
                     << "leftPeakDbfs:" << TxMicChannelNormalizer::dbfs(diagnostics.leftPeak)
                     << "rightPeakDbfs:" << TxMicChannelNormalizer::dbfs(diagnostics.rightPeak)
                     << "selected:"
                     << TxMicChannelNormalizer::channelModeName(diagnostics.selectedMode);
}

// ─── TX stream ────────────────────────────────────────────────────────────────

bool AudioEngine::startTxStream(const QHostAddress& radioAddress, quint16 radioPort)
{
    if (m_audioSource) return true;  // already running

    m_txAddress = radioAddress;
    m_txPort    = radioPort;
    m_txPacketCount = 0;
    m_txAccumulator.clear();
    m_txMicChannelState.reset();
    m_lastTxMicChannelLog.invalidate();

    // TX mic capture uses Int16 — we convert to float32 after capture.
    // (makeFormat() returns Float for the RX sink, but mic hardware is Int16.)
    QAudioFormat fmt;
    fmt.setSampleRate(DEFAULT_SAMPLE_RATE);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice dev = m_inputDevice.isNull()
        ? QMediaDevices::defaultAudioInput() : m_inputDevice;

    if (dev.isNull()) {
        qCWarning(lcAudio) << "AudioEngine: no audio input device available";
        return false;
    }

    qCDebug(lcAudio) << "AudioEngine: input device caps:"
        << dev.minimumSampleRate() << "-" << dev.maximumSampleRate() << "Hz"
        << dev.minimumChannelCount() << "-" << dev.maximumChannelCount() << "ch";

    // Negotiate the best sample rate for TX mic input.
    // macOS: prefer 48kHz — Core Audio claims 24kHz support but its internal
    // resampler produces gravelly artifacts at non-standard rates. Let r8brain
    // handle the 48k→24k conversion instead (clean 2:1 integer-ratio downsample).
    // Linux/Windows: prefer 24kHz (radio native — no resampling needed).
    bool formatFound = false;
#ifdef Q_OS_MAC
    constexpr int rates[] = {48000, 44100, 24000};
#else
    constexpr int rates[] = {24000, 48000, 44100};
#endif
#ifdef Q_OS_WIN
    // Windows WASAPI shared mode handles rate conversion transparently,
    // but Qt's isFormatSupported() returns false for many valid devices
    // (Voicemeeter, FlexRadio DAX, etc.). Default to 48kHz stereo and
    // let WASAPI handle it — only fall back if open actually fails later.
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    formatFound = true;
#else
    for (int channels : {2, 1}) {
        for (int rate : rates) {
            fmt.setChannelCount(channels);
            fmt.setSampleRate(rate);
            if (dev.isFormatSupported(fmt)) {
                formatFound = true;
                break;
            }
        }
        if (formatFound) break;
    }
#endif

    if (!formatFound) {
        qCWarning(lcAudio) << "AudioEngine: input device supports no usable format"
            << "(tried 24/48/44.1 kHz, stereo and mono)";
        return false;
    }

    qCInfo(lcAudio) << "AudioEngine: selected TX input format:"
        << fmt.sampleRate() << "Hz" << fmt.channelCount() << "ch";

    // Record actual negotiated input format for resampling in onTxAudioReady
    m_txInputRate = fmt.sampleRate();
    m_txInputChannels = fmt.channelCount();
    m_txInputMono = (m_txInputChannels == 1);
    m_txNeedsResample = (m_txInputRate != 24000);

    // Create polyphase resampler for high-quality rate conversion
    if (m_txNeedsResample)
        m_txResampler = std::make_unique<Resampler>(m_txInputRate, 24000, 16384);
    else
        m_txResampler.reset();

    qCDebug(lcAudio) << "AudioEngine: TX input device:" << dev.description()
             << "id:" << dev.id()
             << "rate:" << fmt.sampleRate() << "ch:" << fmt.channelCount()
             << "resample:" << m_txNeedsResample;

#ifdef Q_OS_MAC
    // macOS: QAudioSource pull mode broken — use push mode with QBuffer
    const quint64 txLifecycleGeneration = ++m_txLifecycleGeneration;
    m_micBuffer = new QBuffer(this);
    m_micBuffer->open(QIODevice::ReadWrite);
    m_audioSource = new QAudioSource(dev, fmt, this);
    m_audioSource->start(m_micBuffer);

    if (m_audioSource->state() == QAudio::StoppedState) {
        qCWarning(lcAudio) << "AudioEngine: failed to start audio source";
        delete m_audioSource; m_audioSource = nullptr;
        delete m_micBuffer; m_micBuffer = nullptr;
        return false;
    }

    // Poll push-mode buffer
    m_txPollTimer = new QTimer(this);
    m_txPollTimer->setInterval(5);
    connect(m_txPollTimer, &QTimer::timeout, this, &AudioEngine::onTxAudioReady);
    m_txPollTimer->start();

    // Guard against CoreAudio silently stopping the source after extended
    // runtime (~16h). Detect the silent stop, pause the timer, and restart
    // cleanly so onTxAudioReady never touches a stale m_micBuffer. (#1149)
    connect(m_audioSource, &QAudioSource::stateChanged, this,
            [this, txLifecycleGeneration](QAudio::State state) {
        if (state != QAudio::StoppedState) {
            return;
        }
        if (txLifecycleGeneration != m_txLifecycleGeneration) {
            return;
        }
        if (!m_audioSource || !m_txPollTimer) {
            return;  // intentional stop already handled
        }

        const QAudio::Error error = m_audioSource->error();
        m_txPollTimer->stop();
        if (error != QAudio::NoError) {
            qCWarning(lcAudio) << "AudioEngine: QAudioSource stopped with error, not auto-restarting TX"
                               << error;
            QMetaObject::invokeMethod(this, [this]() {
                if (m_audioSource) {
                    stopTxStream();
                }
            }, Qt::QueuedConnection);
            return;
        }

        const qint64 runtimeMs = m_txSourceStartTime.isValid() ? m_txSourceStartTime.elapsed() : 0;
        if (!m_txSourceStartTime.isValid() || runtimeMs < kTxAutoRestartMinRuntimeMs) {
            qCWarning(lcAudio) << "AudioEngine: QAudioSource stopped too soon, not auto-restarting TX"
                               << runtimeMs << "ms";
            QMetaObject::invokeMethod(this, [this]() {
                if (m_audioSource) {
                    stopTxStream();
                }
            }, Qt::QueuedConnection);
            return;
        }

        QHostAddress addr = m_txAddress;
        quint16 port = m_txPort;
        QMetaObject::invokeMethod(this, [this, addr, port]() {
            qCWarning(lcAudio) << "AudioEngine: QAudioSource stopped silently (#1149), restarting TX";
            stopTxStream();
            startTxStream(addr, port);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
#else
    // Linux/Windows: pull mode works fine
    m_audioSource = new QAudioSource(dev, fmt, this);
    m_micDevice = m_audioSource->start();
    if (!m_micDevice) {
        qCWarning(lcAudio) << "AudioEngine: failed to open audio source at"
                           << fmt.sampleRate() << "Hz" << fmt.channelCount() << "ch"
                           << "error:" << m_audioSource->error()
                           << "device:" << dev.description();
#ifdef Q_OS_WIN
        // Windows: WASAPI may reject our negotiated format at open time.
        // Try additional rates before giving up.
        delete m_audioSource; m_audioSource = nullptr;
        bool txOpened = false;
        constexpr int fallbackRates[] = {48000, 44100, 24000, 16000};
        for (int rate : fallbackRates) {
            if (rate == fmt.sampleRate()) continue;
            for (int ch : {2, 1}) {
                fmt.setSampleRate(rate);
                fmt.setChannelCount(ch);
                m_audioSource = new QAudioSource(dev, fmt, this);
                m_micDevice = m_audioSource->start();
                if (m_micDevice) {
                    qCInfo(lcAudio) << "AudioEngine: TX source opened at fallback"
                                    << rate << "Hz" << ch << "ch";
                    m_txInputRate = rate;
                    m_txInputChannels = ch;
                    m_txInputMono = (m_txInputChannels == 1);
                    m_txNeedsResample = (rate != 24000);
                    if (m_txNeedsResample) {
                        m_txResampler = std::make_unique<Resampler>(rate, 24000, 16384);
                    } else {
                        m_txResampler.reset();
                    }
                    txOpened = true;
                    break;
                }
                delete m_audioSource; m_audioSource = nullptr;
            }
            if (txOpened) break;
        }
        if (!txOpened) {
            qCWarning(lcAudio) << "AudioEngine: all TX source formats failed";
            return false;
        }
#else
        delete m_audioSource; m_audioSource = nullptr;
        return false;
#endif
    }
    connect(m_micDevice, &QIODevice::readyRead, this, &AudioEngine::onTxAudioReady);
#endif

    m_txSourceStartTime.restart();
    qCWarning(lcAudio) << "AudioEngine: TX stream started ->" << radioAddress.toString()
             << ":" << radioPort << "streamId:" << Qt::hex << m_txStreamId
             << Qt::dec << "device:" << dev.description() << "id:" << dev.id()
             << "rate:" << m_txInputRate << "ch:" << m_txInputChannels
             << "resample:" << m_txNeedsResample;
    return true;
}

void AudioEngine::stopTxStream()
{
    ++m_txLifecycleGeneration;
#ifdef Q_OS_MAC
    QTimer* pollTimer = m_txPollTimer;
    m_txPollTimer = nullptr;
    QBuffer* micBuffer = m_micBuffer;
    m_micBuffer = nullptr;
#endif
    QAudioSource* audioSource = m_audioSource;
    m_audioSource = nullptr;
    m_micDevice = nullptr;

#ifdef Q_OS_MAC
    if (pollTimer) {
        pollTimer->stop();
        delete pollTimer;
    }
#endif
    if (audioSource) {
        // Guard: calling stop() on an already-stopped QAudioSource on macOS causes
        // AudioOutputUnitStop to dereference a stale CoreAudio device handle,
        // producing EXC_ARM_DA_ALIGN / EXC_BAD_ACCESS (#1059).
        if (audioSource->state() != QAudio::StoppedState) {
            audioSource->stop();
        }
        delete audioSource;
    }
#ifdef Q_OS_MAC
    if (micBuffer) {
        delete micBuffer;
    }
#endif
    m_txSocket.close();
    m_txAccumulator.clear();
    m_txFloatAccumulator.clear();
    m_txResampler.reset();
    m_txInputChannels = 2;
    m_txInputMono = false;
    m_txInputRate = DEFAULT_SAMPLE_RATE;
    m_txNeedsResample = false;
    m_txMicChannelState.reset();
    m_lastTxMicChannelLog.invalidate();
    m_txSourceStartTime.invalidate();
}

void AudioEngine::onTxAudioReady()
{
#ifdef Q_OS_MAC
    if (!m_micBuffer || !m_audioSource) return;
    if (m_audioSource->state() == QAudio::StoppedState) return;
    if (!m_micBuffer->isOpen()) return;
    if (m_txStreamId == 0 && m_remoteTxStreamId == 0) return;
    qint64 avail = m_micBuffer->pos();
    if (avail <= 0) return;
    QByteArray data = m_micBuffer->data();
    m_micBuffer->buffer().clear();
    m_micBuffer->seek(0);
    if (data.isEmpty()) return;
#else
    if (!m_micDevice || (m_txStreamId == 0 && m_remoteTxStreamId == 0)) return;
    QByteArray data = m_micDevice->readAll();
    if (data.isEmpty()) return;
#endif

    // Canonicalize immediately after capture: TX voice is logically mono
    // carried as stereo int16, so choose/average the real mic channel before
    // any resampling, RADE/DAX branch, test tone, DSP, gain, limiter, or meter.
    TxMicChannelNormalizer::Diagnostics channelDiagnostics;
    data = TxMicChannelNormalizer::canonicalizeInt16ToMonoStereo(
        data,
        m_txInputChannels,
        m_txInputRate,
        m_txMicChannelMode,
        &m_txMicChannelState,
        &channelDiagnostics);
    if (data.isEmpty()) return;
    logTxInputChannelDiagnostics(channelDiagnostics, "TX mic");

    // Resample canonical mono int16 to 24kHz duplicated stereo if needed, then
    // convert to float32 for RADE. Normal TX path stays int16 (Opus requires
    // int16). Do not call processStereoToStereo() here: that helper would
    // average raw mic L/R and reintroduce the one-sided-channel 6.02 dB loss.
    if (m_txNeedsResample && m_txResampler) {
        // Convert canonical duplicated int16 stereo → float32 mono for the
        // mono-to-stereo resampler.
        const auto* i16 = reinterpret_cast<const int16_t*>(data.constData());
        const int frames = data.size() / static_cast<int>(2 * sizeof(int16_t));
        QByteArray f32(frames * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* fd = reinterpret_cast<float*>(f32.data());
        for (int i = 0; i < frames; ++i)
            fd[i] = i16[i * 2] / 32768.0f;

        f32 = m_txResampler->processMonoToStereo(
            reinterpret_cast<const float*>(f32.constData()),
            f32.size() / static_cast<int>(sizeof(float)));

        // Convert back to int16 for the rest of the TX path
        const auto* rsrc = reinterpret_cast<const float*>(f32.constData());
        const int rcount = f32.size() / static_cast<int>(sizeof(float));
        if (rcount <= 0) return;
        data.resize(rcount * static_cast<int>(sizeof(int16_t)));
        auto* rdst = reinterpret_cast<int16_t*>(data.data());
        for (int i = 0; i < rcount; ++i)
            rdst[i] = static_cast<int16_t>(std::clamp(rsrc[i] * 32768.0f, -32768.0f, 32767.0f));
    }

    // RADE mode: apply client-side gain + meter, then convert int16 → float32
    if (m_radeMode) {
        // Apply client-side mic gain (same int16 gain path as SSB below)
        const float gain = m_pcMicGain.load();
        if (gain < 0.999f) {
            auto* pcm = reinterpret_cast<int16_t*>(data.data());
            int sampleCount = data.size() / static_cast<int>(sizeof(int16_t));
            for (int i = 0; i < sampleCount; ++i) {
                pcm[i] = static_cast<int16_t>(std::clamp(
                    static_cast<int>(pcm[i] * gain), -32768, 32767));
            }
        }
        accumulatePcMicMeterInt16Stereo(data);
        const auto* i16 = reinterpret_cast<const int16_t*>(data.constData());
        const int ns = data.size() / static_cast<int>(sizeof(int16_t));
        QByteArray f32(ns * static_cast<int>(sizeof(float)), Qt::Uninitialized);
        auto* fd = reinterpret_cast<float*>(f32.data());
        for (int i = 0; i < ns; ++i)
            fd[i] = i16[i] / 32768.0f;
        emit txRawPcmReady(f32);
        return;
    }

    // DAX TX mode: VirtualAudioBridge handles TX audio via feedDaxTxAudio().
    // Don't send mic audio — it would conflict with the DAX stream.
    if (m_daxTxMode) return;

    // ── Client-side TX DSP: compressor + parametric EQ ──────────────────
    // Runs after mic capture and resample, before PC mic gain / metering /
    // Opus / VITA-49, so the user hears the shaped signal exactly as the
    // radio will receive it.  Chain order (CMP→EQ vs EQ→CMP) is user-
    // selectable via setTxChainOrder().
    // ── Test tone (head of chain) ───────────────────────────────
    // When enabled, replaces mic input with a sine so the user can
    // run the chain on a known signal.  Runs BEFORE the user's DSP
    // chain so the tone exits the strip with all stages applied.
    if (m_clientTxTestTone && m_clientTxTestTone->isEnabled()) {
        const int samples = data.size() / static_cast<int>(sizeof(int16_t));
        const int frames  = samples / 2;
        m_clientTxTestTone->process(
            reinterpret_cast<int16_t*>(data.data()), frames, 2);
    }

    applyClientTxDspInt16(data);

    // ── PUDU monitor tap ─────────────────────────────────────────
    // Feeds the post-DSP int16 bytes into the TX monitor if one is
    // registered.  Lock-free atomic pointer load; the monitor's
    // feedTxPostDsp() itself handles the not-recording fast-path.
    if (auto* mon = m_txPostDspMonitor.load(std::memory_order_acquire)) {
        mon->feedTxPostDsp(data);
    }

    // ── Apply client-side PC mic gain (int16) ───────────────────────────
    const float gain = m_pcMicGain.load();
    if (gain < 0.999f) {
        auto* pcm = reinterpret_cast<int16_t*>(data.data());
        int sampleCount = data.size() / static_cast<int>(sizeof(int16_t));
        for (int i = 0; i < sampleCount; ++i)
            pcm[i] = static_cast<int16_t>(std::clamp(
                static_cast<int>(pcm[i] * gain), -32768, 32767));
    }

    // ── Quindar tones (#2262) ───────────────────────────────────────────
    // Sits AFTER the user DSP chain and PC mic gain but BEFORE the final
    // brickwall limiter, so the generated tone is unprocessed by Comp/EQ
    // (no comp pumping, no EQ tilt) but is still bounded by the configured
    // ceiling.  Driven by TransmitModel's PTT coordinator on phone modes;
    // the stage replaces samples wholesale during Engaging/Disengaging
    // phases and is a no-op the rest of the time.
    if (m_clientQuindarTone) {
        const int frames = data.size() / static_cast<int>(sizeof(int16_t) * 2);
        m_clientQuindarTone->process(
            reinterpret_cast<int16_t*>(data.data()), frames, 2);
    }

    // ── Final brickwall limiter (TX tail) ───────────────────────────────
    // Sits at the very end of the chain — after every user-configurable
    // stage AND after PC mic gain — so no sample escapes louder than the
    // configured ceiling regardless of upstream behaviour.  Its meters
    // (input / output peak, GR, active) are what the strip's "Final
    // Output Stage" panel reads.
    applyClientFinalLimiterTxInt16(data);

    // ── Final-output monitor tap ────────────────────────────────
    // Mirrors the post-PUDU monitor but reads at the chain's tail
    // (post-limiter), so a recording captures EXACTLY what the radio
    // is told to transmit.  Lock-free pointer load; the monitor's
    // feedTxPostDsp() handles the not-recording fast path.
    if (auto* mon = m_txFinalMonitor.load(std::memory_order_acquire)) {
        mon->feedTxPostDsp(data);
    }

    // ── TX post-final-limiter scope tap ─────────────────────────
    // Sampled here, AFTER everything the strip can do to the audio
    // (user chain, PC mic gain, brickwall limiter), so the strip's
    // "Waveform CE-SSB" panel shows the exact int16 stream that gets
    // packetised into VITA-49 and sent to the radio.
    emitTxPostChainScopeFromInt16Stereo(data, DEFAULT_SAMPLE_RATE);

    // ── Client-side PC mic level metering (int16) ───────────────────────
    accumulatePcMicMeterInt16Stereo(data);

    emitScopeFromInt16Stereo(data, DEFAULT_SAMPLE_RATE, true);

    // ── Opus TX path: always active for remote_audio_tx ────────────────
    // Sends Opus during both RX (VOX/met_in_rx metering) and TX (voice).
    // The radio requires Opus on remote_audio_tx (enforces compression=OPUS).
    // Data is int16 stereo — accumulate directly for Opus encoding.
    if (m_opusTxEnabled) {
        m_opusTxAccumulator.append(data);
        // 240 stereo sample frames × 2 channels × 2 bytes = 960 bytes per 10ms frame
        constexpr int OPUS_FRAME_BYTES = 240 * 2 * sizeof(int16_t);

        while (m_opusTxAccumulator.size() >= OPUS_FRAME_BYTES) {
            if (!m_opusTxCodec) {
                m_opusTxCodec = std::make_unique<OpusCodec>();
                if (!m_opusTxCodec->isValid()) {
                    qCWarning(lcAudio) << "AudioEngine: Opus TX codec init failed, falling back to uncompressed";
                    m_opusTxEnabled = false;
                    m_opusTxCodec.reset();
                    break;
                }
            }

            QByteArray frame = m_opusTxAccumulator.left(OPUS_FRAME_BYTES);
            m_opusTxAccumulator.remove(0, OPUS_FRAME_BYTES);

            QByteArray opus = m_opusTxCodec->encode(frame);
            if (opus.isEmpty()) continue;

            // Build VITA-49 Opus packet matching SmartSDR exactly:
            // Header: 28 bytes + opus payload, NO trailer.
            // FlexLib Opus packets are byte-centric — payload is NOT
            // padded to 32-bit word alignment. Size field in header
            // is still in 32-bit words (rounded up) per VITA-49 spec.
            const int pktBytes = 28 + opus.size();  // exact, no padding
            const int sizeWords = (pktBytes + 3) / 4;  // for header field only
            QByteArray pkt(pktBytes, '\0');
            auto* p = reinterpret_cast<quint32*>(pkt.data());

            // Word 0: type=3 (ExtDataWithStream), C=1, T=0, TSI=3, TSF=1
            p[0] = qToBigEndian<quint32>(
                (3u << 28) | (1u << 27) | (3u << 22) | (1u << 20)
                | ((m_txPacketCount & 0x0F) << 16) | sizeWords);
            m_txPacketCount = (m_txPacketCount + 1) & 0x0F;
            p[1] = qToBigEndian(m_remoteTxStreamId);    // remote_audio_tx stream
            p[2] = qToBigEndian<quint32>(0x00001C2D);   // OUI (FlexRadio)
            p[3] = qToBigEndian<quint32>(0x534C0000 | 0x8005);  // ICC=0x534C, PCC=0x8005
            p[4] = 0; p[5] = 0; p[6] = 0;              // timestamps (all zero)

            memcpy(pkt.data() + 28, opus.constData(), opus.size());

            // Queue for paced delivery instead of sending immediately.
            // The 10ms pacing timer drains one packet per tick for even
            // timing over SmartLink/WAN. Cap queue to ~200ms to prevent
            // runaway growth if the mic delivers faster than real-time.
            m_opusTxQueue.append(pkt);
            if (m_opusTxQueue.size() > 20)
                m_opusTxQueue.removeFirst();
        }
        return;
    }

    // ── Uncompressed TX path (not used — radio forces Opus) ────────────
    m_txAccumulator.append(data);

    while (m_txAccumulator.size() >= TX_PCM_BYTES_PER_PACKET) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(m_txAccumulator.constData());

        // Convert int16 → float32 for VITA-49 packet (radio expects float32)
        float floatBuf[TX_SAMPLES_PER_PACKET * 2];
        for (int i = 0; i < TX_SAMPLES_PER_PACKET * 2; ++i)
            floatBuf[i] = pcm[i] / 32768.0f;

        QByteArray packet = buildVitaTxPacket(floatBuf, TX_SAMPLES_PER_PACKET);
        emit txPacketReady(packet);

        m_txAccumulator.remove(0, TX_PCM_BYTES_PER_PACKET);
    }
}

QByteArray AudioEngine::buildVitaTxPacket(const float* samples, int numStereoSamples)
{
    const int payloadBytes = numStereoSamples * 2 * 4;  // stereo × sizeof(float)
    const int packetWords = (payloadBytes / 4) + VITA_HEADER_WORDS;
    const int packetBytes = packetWords * 4;

    QByteArray packet(packetBytes, '\0');
    quint32* words = reinterpret_cast<quint32*>(packet.data());

    // ── Word 0: Header (DAX TX format, matches FlexLib DAXTXAudioStream) ─
    // Bits 31-28: packet type = 1 (IFDataWithStream)
    // Bit  27:    C = 1 (class ID present)
    // Bit  26:    T = 0 (no trailer)
    // Bits 25-24: reserved = 0
    // Bits 23-22: TSI = 3 (Other)
    // Bits 21-20: TSF = 1 (SampleCount)
    // Bits 19-16: packet count (4-bit)
    // Bits 15-0:  packet size (in 32-bit words)
    quint32 hdr = 0;
    hdr |= (0x1u << 28);          // pkt_type = IFDataWithStream (DAX TX)
    hdr |= (1u << 27);            // C = 1
    // T = 0 (bit 26)
    hdr |= (0x3u << 22);          // TSI = 3 (Other) — matches FlexLib/nDAX
    hdr |= (0x1u << 20);          // TSF = SampleCount
    hdr |= ((m_txPacketCount & 0xF) << 16);
    hdr |= (packetWords & 0xFFFF);
    words[0] = qToBigEndian(hdr);

    // ── Word 1: Stream ID (dax_tx stream for DAX TX audio) ──────────────
    words[1] = qToBigEndian(m_txStreamId);

    // ── Word 2: Class ID OUI (24-bit, right-justified in 32-bit word) ────
    words[2] = qToBigEndian(FLEX_OUI);

    // ── Word 3: InformationClassCode (upper 16) | PacketClassCode (lower 16)
    words[3] = qToBigEndian(
        (static_cast<quint32>(FLEX_INFO_CLASS) << 16) | PCC_IF_NARROW);

    // ── Words 4-6: Timestamps ─────────────────────────────────────────────
    // ── Words 4-6: Timestamps ─────────────────────────────────────────────
    words[4] = 0;  // integer timestamp
    words[5] = 0;  // fractional timestamp high
    words[6] = 0;  // fractional timestamp low

    // ── Payload: float32 stereo, big-endian ───────────────────────────────
    quint32* payload = words + VITA_HEADER_WORDS;
    for (int i = 0; i < numStereoSamples * 2; ++i) {
        quint32 raw;
        std::memcpy(&raw, &samples[i], 4);
        payload[i] = qToBigEndian(raw);
    }

    // Increment packet count (4-bit, mod 16)
    m_txPacketCount = (m_txPacketCount + 1) & 0xF;

    return packet;
}

void AudioEngine::sendVoiceTxPacket(const QByteArray& pcmData, quint32 streamId)
{
    // Accumulate into a separate buffer for VOX/met_in_rx audio
    m_voxAccumulator.append(pcmData);

    while (m_voxAccumulator.size() >= TX_PCM_BYTES_PER_PACKET) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(m_voxAccumulator.constData());

        float floatBuf[TX_SAMPLES_PER_PACKET * 2];
        for (int i = 0; i < TX_SAMPLES_PER_PACKET * 2; ++i)
            floatBuf[i] = pcm[i] / 32768.0f;

        // Build packet using the remote_audio_tx stream ID
        quint32 savedId = m_txStreamId;
        m_txStreamId = streamId;
        QByteArray packet = buildVitaTxPacket(floatBuf, TX_SAMPLES_PER_PACKET);
        m_txStreamId = savedId;

        emit txPacketReady(packet);
        m_voxAccumulator.remove(0, TX_PCM_BYTES_PER_PACKET);
    }
}

void AudioEngine::setOutputDevice(const QAudioDevice& dev)
{
    m_outputDevice = dev;
    qCDebug(lcAudio) << "AudioEngine: output device set to" << dev.description();

    // Persist selection
    auto& s = AppSettings::instance();
    s.setValue("AudioOutputDeviceId", dev.id());
    s.save();

    // Restart RX stream if running
    if (m_audioSink) {
        stopRxStream();
        startRxStream();
    }
}

void AudioEngine::setInputDevice(const QAudioDevice& dev)
{
    m_inputDevice = dev;
    qCDebug(lcAudio) << "AudioEngine: input device set to" << dev.description();

    // Persist selection
    auto& s = AppSettings::instance();
    s.setValue("AudioInputDeviceId", dev.id());
    s.save();

    // Restart TX stream if running
    if (m_audioSource) {
        QHostAddress addr = m_txAddress;
        quint16 port = m_txPort;
        stopTxStream();
        startTxStream(addr, port);
    }
}

#ifdef Q_OS_MAC
void AudioEngine::setAllowBluetoothTelephonyOutput(bool on)
{
    const bool changed = (m_allowBluetoothTelephonyOutput.exchange(on) != on);
    if (!changed || !m_audioSink) {
        return;
    }

    stopRxStream();
    startRxStream();
}
#endif

// ─── RADE digital voice support ──────────────────────────────────────────────

void AudioEngine::setRadeMode(bool on)
{
    if (m_radeMode == on) return;
    m_radeMode = on;
    // RADE TX: onTxAudioReady() emits txRawPcmReady (float32) then returns
    // early — the Opus voice TX path never runs. RADEEngine receives the
    // raw PCM, encodes it to a modem waveform, and emits it via
    // sendModemTxAudio() → buildVitaTxPacket() → dax_tx VITA-49 stream.
    // The radio routes that stream to the TX modulator only when dax=1.
    // activateRADE() sets the slice to DIGU/DIGL, which fires
    // updateDaxTxMode() → setDax(true) → transmit set dax=1 before PTT.
    // Do NOT emit daxRouteRequested(0) here — dax=0 tells the radio to
    // use the physical mic and discard every dax_tx packet, producing no
    // TX waveform. feedDaxTxAudio/m_daxTxUseRadioRoute are irrelevant:
    // RADE bypasses feedDaxTxAudio entirely.
    if (!on)
        m_radeRxBuffer.clear();
    clearTxAccumulators();
}

void AudioEngine::sendModemTxAudio(const QByteArray& float32pcm)
{
    if (m_txStreamId == 0) return;

    if (m_radioTransmitting)
        emitScopeFromFloat32Stereo(float32pcm, DEFAULT_SAMPLE_RATE, true);

    m_txFloatAccumulator.append(float32pcm);

    constexpr int FLOAT_BYTES_PER_PKT = TX_SAMPLES_PER_PACKET * 2 * sizeof(float); // 1024
    while (m_txFloatAccumulator.size() >= FLOAT_BYTES_PER_PKT) {
        auto* samples = reinterpret_cast<const float*>(m_txFloatAccumulator.constData());
        QByteArray pkt = buildVitaTxPacket(samples, TX_SAMPLES_PER_PACKET);
        emit txPacketReady(pkt);
        m_txFloatAccumulator.remove(0, FLOAT_BYTES_PER_PKT);
    }
}

void AudioEngine::setDaxTxMode(bool on)
{
    const bool previous = m_daxTxMode.exchange(on);
    if (previous != on) {
        qCDebug(lcDax) << "AudioEngine: DAX TX mode"
                       << (on ? "enabled" : "disabled")
                       << "route=" << (m_daxTxUseRadioRoute ? "radio-dax" : "float32-dax-tx")
                       << "stream=0x" + QString::number(m_txStreamId, 16);
    }
}

void AudioEngine::setTransmitting(bool tx)
{
    if (m_transmitting == tx) return;
    m_transmitting = tx;

    if (!tx) {
        // On unkey: drop any partial packet residue so next burst starts cleanly.
        m_txAccumulator.clear();
        m_txFloatAccumulator.clear();
        m_daxPreTxBuffer.clear();
        m_opusTxQueue.clear();
    }
}

void AudioEngine::setRadioTransmitting(bool tx)
{
    const bool previous = m_radioTransmitting.exchange(tx);
    if (previous != tx)
        emit radioTransmittingChanged(tx);
}

void AudioEngine::setDaxTxUseRadioRoute(bool on)
{
    if (m_daxTxUseRadioRoute == on) return;
    m_daxTxUseRadioRoute = on;
    // Switching route changes payload format; drop partial buffered samples.
    m_txFloatAccumulator.clear();
    m_daxPreTxBuffer.clear();
    m_daxRadioTxChannelState.reset();
    m_lastDaxRadioChannelLog.invalidate();
    qCDebug(lcDax) << "AudioEngine: DAX TX route"
                   << (on ? "radio-dax pcc=0x0123" : "float32 pcc=0x03e3")
                   << "stream=0x" + QString::number(m_txStreamId, 16);
}

void AudioEngine::feedDaxTxAudio(const QByteArray& inPcm)
{
    if (m_txStreamId == 0 || inPcm.isEmpty()) return;

    // Client-side TX DSP (compressor + EQ) is intentionally NOT
    // applied here.  This path is fed exclusively by TCI and DAX
    // (WSJT-X, fldigi, PipeWire bridge, etc.) — digital modes carry
    // pre-shaped tones that would be destroyed by a voice-tuned
    // compressor or EQ.  Mic voice TX goes through onTxAudioReady,
    // which keeps the full DSP chain.
    const QByteArray& float32pcm = inPcm;

    // Measure DAX TX input level and emit via pcMicLevelChanged so the
    // P/CW mic gauge shows DAX audio level regardless of mic profile (#517)
    {
        const auto* src = reinterpret_cast<const float*>(float32pcm.constData());
        const int samples = float32pcm.size() / sizeof(float);
        float peak = 0.0f;
        double sumSq = 0.0;
        for (int i = 0; i < samples; ++i) {
            float s = std::abs(src[i]);
            if (s > peak) peak = s;
            sumSq += static_cast<double>(src[i]) * src[i];
        }
        m_pcMicPeak = std::max(m_pcMicPeak, peak);
        m_pcMicSumSq += sumSq;
        m_pcMicSampleCount += samples;
        if (m_pcMicSampleCount >= kMicMeterWindowSamples) {
            float rms = static_cast<float>(std::sqrt(m_pcMicSumSq / m_pcMicSampleCount));
            float peakDb = (m_pcMicPeak > 1e-10f) ? 20.0f * std::log10(m_pcMicPeak) : -150.0f;
            float rmsDb  = (rms > 1e-10f)         ? 20.0f * std::log10(rms)          : -150.0f;
            emit pcMicLevelChanged(peakDb, rmsDb);
            m_pcMicPeak = 0.0f;
            m_pcMicSumSq = 0.0;
            m_pcMicSampleCount = 0;
        }
    }

    const bool daxAudioWillTransmit = m_radioTransmitting
        && (!m_daxTxUseRadioRoute || !(m_transmitting && !m_daxTxMode));
    if (daxAudioWillTransmit)
        emitScopeFromFloat32Stereo(float32pcm, DEFAULT_SAMPLE_RATE, true);

    if (!m_daxTxUseRadioRoute) {
        // Low-latency route: keep radio on mic path (dax=0) and packetize
        // exactly like voice TX (PCC 0x03E3 float32 stereo).
        constexpr int FLOAT_BYTES_PER_PKT = TX_SAMPLES_PER_PACKET * 2 * sizeof(float);

        // Gate on raw radio TX state, not ownership. When an external app
        // (WSJT-X) triggers PTT, m_transmitting is false (we don't own TX)
        // but the radio IS transmitting and needs our DAX audio. (#752)
        if (!m_radioTransmitting) {
            m_daxPreTxBuffer.clear();
            m_txFloatAccumulator.clear();
            return;
        }

        m_txFloatAccumulator.append(float32pcm);
        while (m_txFloatAccumulator.size() >= FLOAT_BYTES_PER_PKT) {
            auto* samples = reinterpret_cast<const float*>(m_txFloatAccumulator.constData());
            QByteArray pkt = buildVitaTxPacket(samples, TX_SAMPLES_PER_PACKET);
            emit txPacketReady(pkt);
            m_txFloatAccumulator.remove(0, FLOAT_BYTES_PER_PKT);
        }
        return;
    }

    // Radio-native DAX route (dax=1): block DAX audio only when mic voice TX is active.
    if (m_transmitting && !m_daxTxMode) return;
    m_daxPreTxBuffer.clear();

    // Convert float32 stereo → int16 mono (reduced BW format, PCC 0x0123).
    // This route is still a digital/DAX bypass: no voice DSP, gain, Quindar, or
    // final limiter. The mono collapse only avoids the same one-sided stereo
    // 6.02 dB loss that can affect virtual/aggregate DAX sources.
    TxMicChannelNormalizer::Diagnostics daxDiagnostics;
    QByteArray mono = TxMicChannelNormalizer::collapseFloat32ToInt16MonoBigEndian(
        float32pcm,
        2,
        DEFAULT_SAMPLE_RATE,
        m_daxRadioTxChannelMode,
        &m_daxRadioTxChannelState,
        &daxDiagnostics);
    if (mono.isEmpty()) return;
    logTxInputChannelDiagnostics(daxDiagnostics, "DAX radio");

    m_txFloatAccumulator.append(mono);

    // Build and send VITA-49 packets: 128 mono int16 samples per packet
    constexpr int MONO_BYTES_PER_PKT = TX_SAMPLES_PER_PACKET * sizeof(qint16);  // 256 bytes
    while (m_txFloatAccumulator.size() >= MONO_BYTES_PER_PKT) {
        const int payloadBytes = MONO_BYTES_PER_PKT;
        const int packetWords = (payloadBytes / 4) + VITA_HEADER_WORDS;
        const int packetBytes = packetWords * 4;

        QByteArray pkt(packetBytes, '\0');
        quint32* words = reinterpret_cast<quint32*>(pkt.data());

        // Header: IFDataWithStream, C=1, TSI=3(Other), TSF=1(SampleCount)
        quint32 hdr = 0;
        hdr |= (0x1u << 28);          // pkt_type = IFDataWithStream
        hdr |= (1u << 27);            // C = 1 (class ID present)
        hdr |= (0x3u << 22);          // TSI = 3 (Other) — matches FlexLib/nDAX
        hdr |= (0x1u << 20);          // TSF = 1 (SampleCount)
        hdr |= ((m_txPacketCount & 0xF) << 16);
        hdr |= (packetWords & 0xFFFF);
        words[0] = qToBigEndian(hdr);
        words[1] = qToBigEndian(m_txStreamId);
        words[2] = qToBigEndian(FLEX_OUI);
        words[3] = qToBigEndian(
            (static_cast<quint32>(FLEX_INFO_CLASS) << 16) | PCC_DAX_REDUCED);
        words[4] = 0;  // integer timestamp (zero)
        words[5] = 0;  // fractional timestamp high (zero)
        words[6] = 0;  // fractional timestamp low (zero)

        // Copy pre-converted big-endian int16 mono payload
        std::memcpy(pkt.data() + VITA_HEADER_BYTES,
                    m_txFloatAccumulator.constData(), payloadBytes);

        m_txPacketCount = (m_txPacketCount + 1) & 0xF;
        emit txPacketReady(pkt);
        m_txFloatAccumulator.remove(0, MONO_BYTES_PER_PKT);
    }
}

void AudioEngine::feedDecodedSpeech(const QByteArray& pcm)
{
    if (!m_audioSink || !m_audioDevice || !m_audioDevice->isOpen()) return;

    // Decoded RADE speech goes into its own buffer. The drain timer mixes
    // m_radeRxBuffer with m_rxBuffer sample-wise so both are heard simultaneously
    // without doubling the fill rate. A dedicated resampler preserves the filter
    // state independently from the m_rxResampler used by feedAudioData().
    if (m_resampleTo48k) {
        if (!m_radeRxResampler)
            m_radeRxResampler = std::make_unique<Resampler>(24000, 48000);
        const auto* src = reinterpret_cast<const float*>(pcm.constData());
        m_radeRxBuffer.append(
            m_radeRxResampler->processStereoToStereo(
                src, pcm.size() / (2 * static_cast<int>(sizeof(float)))));
    } else {
        m_radeRxBuffer.append(pcm);
    }
}

} // namespace AetherSDR
