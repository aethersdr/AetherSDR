#include "CwDecoder.h"
#include "LogManager.h"
#include "ggmorse/ggmorse.h"
#include <cstring>
#include <cmath>

namespace AetherSDR {

CwDecoder::CwDecoder(QObject* parent)
    : QObject(parent)
{}

CwDecoder::~CwDecoder()
{
    stop();
}

float CwDecoder::estimatedPitch() const
{
    QMutexLocker lock(&m_bufMutex);
    return m_pitchLocked || !m_typedSource || m_source.current() ? m_pitch.load() : 0.0f;
}

float CwDecoder::estimatedSpeed() const
{
    QMutexLocker lock(&m_bufMutex);
    return m_speedLocked || !m_typedSource || m_source.current() ? m_speed.load() : 0.0f;
}

void CwDecoder::start()
{
    if (m_running) return;

    {
        std::lock_guard lock(m_parametersMutex);
        m_parametersDirty = true;
    }

    m_running = true;

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.clear();
        m_source = {};
        m_typedSource = false;
        ++m_inputGeneration;
    }

    // Run decode loop on worker thread (CwDecoder stays on main thread)
    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("CwDecoder");
    m_workerThread.reset(worker);
    worker->start();

    qCDebug(lcDsp) << "CwDecoder: started";
}

void CwDecoder::stop()
{
    if (!m_running) return;
    m_running = false;
    ++m_inputGeneration;

    if (m_workerThread) {
        // A JOIN, never a timeout.  The callback checks m_running and each
        // decode call is frame-bounded, so this returns promptly -- and the
        // m_pitch/m_speed writes below run OUTSIDE m_parametersMutex, which
        // the worker holds when writing the same members.  They are safe only
        // because the worker is provably dead by the time wait() returns.
        // Never destroy the buffer/owner while a slow frame is still running.
        m_workerThread->wait();
        m_workerThread.reset();
    }

    // The estimates died with the ggmorse instance — clear them so a later
    // Zero Beat can't retune the slice on a pitch from a previous run
    // (#5213).  Locked values are operator-set state, not estimates: keep
    // them, or a restart would feed 0 into a still-pressed
    // lock button on the next start.
    if (!m_pitchLocked) m_pitch = 0;
    if (!m_speedLocked) m_speed = 0;
    if (!m_pitchLocked || !m_speedLocked) {
        // Post the clearing emission through the event queue: the worker's
        // cross-thread statsUpdated deliveries are queued, so a reading it
        // posted just before m_running flipped would otherwise arrive AFTER
        // a direct emit and re-show the dead estimate.  Queued-behind, the
        // clear always lands last (and dies with the object at shutdown).
        const quint64 generation = m_inputGeneration.load();
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation == m_inputGeneration.load()) {
                emit statsUpdated(estimatedPitch(), estimatedSpeed());
            }
        }, Qt::QueuedConnection);
    }

    qCDebug(lcDsp) << "CwDecoder: stopped";
}

void CwDecoder::lockPitch(bool lock)
{
    {
        QMutexLocker inputLock(&m_bufMutex);
        std::lock_guard guard(m_parametersMutex);
        const float pitch = m_pitchLocked || !m_typedSource || m_source.current()
            ? m_pitch.load() : 0.0f;
        m_pitch = pitch;
        m_pitchLocked = lock;
        m_pendingParameters.pitchHz = lock ? pitch : -1.0f;
        m_parametersDirty = true;
    }
    qCDebug(lcDsp) << "CwDecoder: pitch" << (lock ? "locked at" : "unlocked from")
                   << m_pitch.load() << "Hz";
}

void CwDecoder::lockSpeed(bool lock)
{
    {
        QMutexLocker inputLock(&m_bufMutex);
        std::lock_guard guard(m_parametersMutex);
        const float speed = m_speedLocked || !m_typedSource || m_source.current()
            ? m_speed.load() : 0.0f;
        m_speed = speed;
        m_speedLocked = lock;
        m_pendingParameters.speedWpm = lock ? speed : -1.0f;
        m_parametersDirty = true;
    }
    qCDebug(lcDsp) << "CwDecoder: speed" << (lock ? "locked at" : "unlocked from")
                   << m_speed.load() << "WPM";
}

void CwDecoder::setKnownParameters(float pitchHz, float speedWpm)
{
    if (pitchHz <= 0.0f || speedWpm <= 0.0f) return;

    std::lock_guard lock(m_parametersMutex);
    const bool unchanged = qFuzzyCompare(m_pendingParameters.pitchHz, pitchHz)
        && qFuzzyCompare(m_pendingParameters.speedWpm, speedWpm)
        && m_pitchLocked && m_speedLocked;
    if (unchanged) return;

    // Lock both pitch and speed to the P/CW applet values.  The local
    // CWX keyer / iambic keyer / etc. all run at the slider WPM, so
    // sidetone is generated at exactly that rate — ggmorse with both
    // values locked gets a reliable unit length and correctly classifies
    // 1u / 3u / 7u gaps so inter-word boundaries become " " separators.
    m_pendingParameters.pitchHz = pitchHz;
    m_pendingParameters.speedWpm = speedWpm;
    m_pitch = pitchHz;
    m_speed = speedWpm;
    m_pitchLocked = true;
    m_speedLocked = true;

    // Widen pitch range to comfortably include the known value (default
    // is 500–700 Hz but operators commonly use 700 / 750 / 800).  Also
    // drives ggmorse's internal HPF cutoff.
    constexpr float kPitchRangePad = 150.0f;
    m_pendingParameters.pitchRangeMin = std::max(100.0f, pitchHz - kPitchRangePad);
    m_pendingParameters.pitchRangeMax = pitchHz + kPitchRangePad;

    m_parametersDirty = true;
    qCDebug(lcDsp) << "CwDecoder: known params pitch=" << pitchHz
                   << "Hz speed=" << speedWpm << "WPM";
}

void CwDecoder::setPitchRange(int minHz, int maxHz)
{
    std::lock_guard lock(m_parametersMutex);
    m_pendingParameters.pitchRangeMin = static_cast<float>(minHz);
    m_pendingParameters.pitchRangeMax = static_cast<float>(maxHz);
    m_parametersDirty = true;
    qCDebug(lcDsp) << "CwDecoder: pitch range" << minHz << "-" << maxHz << "Hz";
}

void CwDecoder::setSpeedRange(int minWpm, int maxWpm)
{
    std::lock_guard lock(m_parametersMutex);
    m_pendingParameters.speedRangeMin = static_cast<float>(minWpm);
    m_pendingParameters.speedRangeMax = static_cast<float>(maxWpm);
    m_parametersDirty = true;
    qCDebug(lcDsp) << "CwDecoder: speed range" << minWpm << "-" << maxWpm << "WPM";
}

void CwDecoder::feedAudio(const QByteArray& pcm24kStereo)
{
    constexpr qsizetype kStereoBytes = 2 * sizeof(float);
    if (!m_running || pcm24kStereo.size() % kStereoBytes != 0
        || pcm24kStereo.size() / kStereoBytes > PcmFrame::kMaxFrames) {
        return;
    }
    const qsizetype frames = pcm24kStereo.size() / kStereoBytes;
    QByteArray mono(frames * sizeof(int16_t), Qt::Uninitialized);
    for (qsizetype i = 0; i < frames; ++i) {
        float pair[2];
        std::memcpy(pair, pcm24kStereo.constData() + i * kStereoBytes, sizeof(pair));
        if (!std::isfinite(pair[0]) || !std::isfinite(pair[1])) {
            resetInput();
            return;
        }
        const float average = pair[0] * 0.5f + pair[1] * 0.5f;
        const int16_t sample = static_cast<int16_t>(std::clamp(
            double(average) * 32768.0, -32768.0, 32767.0));
        std::memcpy(mono.data() + i * sizeof(sample), &sample, sizeof(sample));
    }
    appendMono(mono, {}, false, false);
}

void CwDecoder::feedPcmBlock(const DecoderPcmBlock& block)
{
    if (!m_running || !block.current() || block.samples.size() > PcmFrame::kMaxFrames) {
        return;
    }
    QByteArray mono(block.samples.size() * sizeof(int16_t), Qt::Uninitialized);
    for (qsizetype i = 0; i < block.samples.size(); ++i) {
        if (!std::isfinite(block.samples[i])) {
            resetInput();
            return;
        }
        const int16_t sample = static_cast<int16_t>(std::clamp(
            double(block.samples[i]) * 32768.0, -32768.0, 32767.0));
        std::memcpy(mono.data() + i * sizeof(sample), &sample, sizeof(sample));
    }
    appendMono(mono, block.source, true, block.discontinuity);
}

void CwDecoder::appendMono(const QByteArray& mono, const PcmEpochLease& source,
                          bool typed, bool discontinuity)
{
    QMutexLocker lock(&m_bufMutex);
    if (!m_running || (typed && !source.current())) {
        return;
    }
    if (discontinuity || typed != m_typedSource
        || (typed && source.stream() != m_source.stream())
        || (typed && m_ringBuf.size() + mono.size() > RING_CAPACITY)) {
        ++m_inputGeneration;
        m_ringBuf.clear();
        queueResetStats(m_inputGeneration.load());
    }
    m_source = source;
    m_typedSource = typed;
    m_ringBuf.append(mono);
    // Preserve the TX sidetone byte API's trim-oldest backlog policy. Typed RX
    // overflow is a source discontinuity and retires its detector above.
    if (!typed && m_ringBuf.size() > RING_CAPACITY) {
        m_ringBuf.remove(0, m_ringBuf.size() - RING_CAPACITY);
    }
}

void CwDecoder::resetInput()
{
    QMutexLocker lock(&m_bufMutex);
    ++m_inputGeneration;
    m_ringBuf.clear();
    m_source = {};
    m_typedSource = false;
    queueResetStats(m_inputGeneration.load());
}

void CwDecoder::queueResetStats(quint64 generation)
{
    // Preserve #5645's coherent parameter snapshot and locked setpoints.
    // Neutral publication is queued so callbacks never run under either mutex.
    {
        std::lock_guard lock(m_parametersMutex);
        if (!m_pitchLocked) { m_pitch = 0; }
        if (!m_speedLocked) { m_speed = 0; }
    }
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == m_inputGeneration.load()) {
            emit statsUpdated(estimatedPitch(), estimatedSpeed());
        }
    }, Qt::QueuedConnection);
}

void CwDecoder::decodeLoop()
{
    // Create ggmorse instance for 24kHz mono int16 input
    GGMorse::Parameters params;
    params.sampleRateInp = 24000.0f;
    params.sampleRateOut = 24000.0f;
    params.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
    params.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_I16;
    params.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_I16;

    std::unique_ptr<GGMorse> engine;
    quint64 generation = 0;

    // ggmorse requests samplesPerFrame * resampleFactor * sampleSize bytes per callback.
    // At 24kHz int16, factor=6 (24000/4000), frame=128: 128*6*2 = 1536 bytes.
    const int resampleFactor = static_cast<int>(params.sampleRateInp / GGMorse::kBaseSampleRate);
    const int bytesPerFrame = params.samplesPerFrame * resampleFactor * static_cast<int>(sizeof(int16_t));
    int feedCount = 0;

    qCDebug(lcDsp) << "CwDecoder: decode loop running, bytesPerFrame:" << bytesPerFrame;

    while (m_running) {
        PcmEpochLease source;
        bool typed = false;
        quint64 inputGeneration = 0;
        // Snapshot ownership with the ring. Revocation never needs a QObject.
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_typedSource && !m_source.current()) {
                m_ringBuf.clear();
            }
            if (m_ringBuf.size() < bytesPerFrame) {
                lock.unlock();
                QThread::msleep(20);
                continue;
            }
            source = m_source;
            typed = m_typedSource;
            inputGeneration = m_inputGeneration.load();
        }
        const bool newInput = !engine || generation != inputGeneration;
        if (newInput) {
            engine = std::make_unique<GGMorse>(params);
            generation = inputGeneration;
        }
        GGMorse& ggmorse = *engine;

        DecodeParameters pending;
        bool applyParameters = false;
        {
            std::lock_guard lock(m_parametersMutex);
            if (m_parametersDirty || newInput) {
                pending = m_pendingParameters;
                m_parametersDirty = false;
                applyParameters = true;
            }
        }
        if (applyParameters) {
            GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
            dp.frequency_hz = pending.pitchHz;
            dp.speed_wpm = pending.speedWpm;
            dp.frequencyRangeMin_hz = pending.pitchRangeMin;
            dp.frequencyRangeMax_hz = pending.pitchRangeMax;
            dp.speedRangeMin_wpm = pending.speedRangeMin;
            dp.speedRangeMax_wpm = pending.speedRangeMax;
            ggmorse.setParametersDecode(dp);
        }

        int framesThisCall = 0;

        bool gotData = ggmorse.decode([this, &framesThisCall, generation, source, typed](void* data, uint32_t nMaxBytes) -> uint32_t {
            // Return after one frame so continuously arriving audio cannot
            // postpone pending parameter changes or stop indefinitely.
            if (!m_running || framesThisCall > 0 || generation != m_inputGeneration.load()
                || (typed && !source.current())) {
                return 0;
            }

            QMutexLocker lock(&m_bufMutex);
            // ggmorse requires exactly nMaxBytes — partial returns cause it to abort
            if (generation != m_inputGeneration.load()
                || static_cast<uint32_t>(m_ringBuf.size()) < nMaxBytes) { return 0; }

            std::memcpy(data, m_ringBuf.constData(), nMaxBytes);
            m_ringBuf.remove(0, nMaxBytes);
            ++framesThisCall;
            return nMaxBytes;
        });

        feedCount += framesThisCall;

        // Log periodically
        if (feedCount % 200 == 0 && feedCount > 0) {
            const auto& stats = ggmorse.getStatistics();
            const auto& rxData = ggmorse.getRxData();
            qCDebug(lcDsp) << "CwDecoder:" << feedCount << "frames fed, pitch:"
                     << stats.estimatedPitch_Hz << "Hz, speed:"
                     << stats.estimatedSpeed_wpm << "WPM, decode:" << gotData
                     << "rxLen:" << rxData.size()
                     << "lastResult:" << ggmorse.lastDecodeResult();
        }

        if (generation != m_inputGeneration.load() || (typed && !source.current())) {
            continue;
        }
        const auto& stats = ggmorse.getStatistics();

        // Accept all decodes — color-coded by confidence in the UI
        GGMorse::TxRx rxData;
        if (ggmorse.takeRxData(rxData) > 0 && stats.costFunction < 1.0f) {
            QString text = QString::fromLatin1(
                reinterpret_cast<const char*>(rxData.data()),
                static_cast<int>(rxData.size()));
            const float cost = stats.costFunction;
            QMetaObject::invokeMethod(this, [this, generation, source, typed, text, cost] {
                if (m_running && generation == m_inputGeneration.load()
                    && (!typed || source.current())) {
                    emit textDecoded(text, cost);
                }
            }, Qt::QueuedConnection);
        }

        if (stats.estimatedPitch_Hz > 0) {
            {
                std::lock_guard lock(m_parametersMutex);
                if (generation != m_inputGeneration.load() || (typed && !source.current())) {
                    continue;
                }
                // A just-completed old frame must not overwrite a newer lock
                // request. Locked setpoints live in the pending snapshot.
                // Nonpositive locks still mean automatic detection to GGMorse.
                if (!m_pitchLocked || m_pendingParameters.pitchHz <= 0.0f) {
                    m_pitch = stats.estimatedPitch_Hz;
                }
                if (!m_speedLocked || m_pendingParameters.speedWpm <= 0.0f) {
                    m_speed = stats.estimatedSpeed_wpm;
                }
            }
            // Read the members at DELIVERY, not here.  A value copied now is
            // already stale by the time the queued emission lands if a setter
            // ran in between, and when no further frame arrives the panel keeps
            // that dead reading forever even though estimatedPitch() is right
            // (#5645 review).  Same queued-read shape stop() uses below.
            QMetaObject::invokeMethod(this, [this, generation, source, typed] {
                if (m_running && generation == m_inputGeneration.load()
                    && (!typed || source.current())) {
                    emit statsUpdated(estimatedPitch(), estimatedSpeed());
                }
            }, Qt::QueuedConnection);
        }
    }

    qCDebug(lcDsp) << "CwDecoder: decode loop exiting, total frames:" << feedCount;
}

} // namespace AetherSDR
