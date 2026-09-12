#include "CwDecoder.h"
#include "LogManager.h"
#include "ggmorse/ggmorse.h"
#include <cstring>

namespace AetherSDR {

CwDecoder::CwDecoder(QObject* parent)
    : QObject(parent)
{}

CwDecoder::~CwDecoder()
{
    stop();
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

    if (m_workerThread) {
        // The callback checks m_running and each decode call is frame-bounded.
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
        QMetaObject::invokeMethod(this, [this] {
            emit statsUpdated(m_pitch, m_speed);
        }, Qt::QueuedConnection);
    }

    qCDebug(lcDsp) << "CwDecoder: stopped";
}

void CwDecoder::lockPitch(bool lock)
{
    {
        std::lock_guard guard(m_parametersMutex);
        m_pitchLocked = lock;
        m_pendingParameters.pitchHz = lock ? m_pitch.load() : -1.0f;
        m_parametersDirty = true;
    }
    qCDebug(lcDsp) << "CwDecoder: pitch" << (lock ? "locked at" : "unlocked from")
                   << m_pitch.load() << "Hz";
}

void CwDecoder::lockSpeed(bool lock)
{
    {
        std::lock_guard guard(m_parametersMutex);
        m_speedLocked = lock;
        m_pendingParameters.speedWpm = lock ? m_speed.load() : -1.0f;
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
    if (!m_running) return;

    // Downmix stereo float32 → mono int16 for ggmorse (requires int16)
    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoSamples = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    QByteArray mono(stereoSamples * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(mono.data());
    for (int i = 0; i < stereoSamples; ++i) {
        float avg = (src[2 * i] + src[2 * i + 1]) * 0.5f;
        dst[i] = static_cast<int16_t>(std::clamp(avg * 32768.0f, -32768.0f, 32767.0f));
    }

    QMutexLocker lock(&m_bufMutex);
    m_ringBuf.append(mono);

    // Trim to capacity (drop oldest)
    if (m_ringBuf.size() > RING_CAPACITY) {
        m_ringBuf.remove(0, m_ringBuf.size() - RING_CAPACITY);
    }
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

    GGMorse ggmorse(params);

    // ggmorse requests samplesPerFrame * resampleFactor * sampleSize bytes per callback.
    // At 24kHz int16, factor=6 (24000/4000), frame=128: 128*6*2 = 1536 bytes.
    const int resampleFactor = static_cast<int>(ggmorse.getSampleRateInp() / GGMorse::kBaseSampleRate);
    const int bytesPerFrame = ggmorse.getSamplesPerFrame() * resampleFactor * ggmorse.getSampleSizeBytesInp();
    int feedCount = 0;

    qCDebug(lcDsp) << "CwDecoder: decode loop running, bytesPerFrame:" << bytesPerFrame;

    while (m_running) {
        // Wait until we have at least one frame of data
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_ringBuf.size() < bytesPerFrame) {
                lock.unlock();
                QThread::msleep(20);
                continue;
            }
        }

        DecodeParameters pending;
        bool applyParameters = false;
        {
            std::lock_guard lock(m_parametersMutex);
            if (m_parametersDirty) {
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

        bool gotData = ggmorse.decode([this, &framesThisCall](void* data, uint32_t nMaxBytes) -> uint32_t {
            // Return after one frame so continuously arriving audio cannot
            // postpone pending parameter changes or stop indefinitely.
            if (!m_running || framesThisCall > 0) {
                return 0;
            }

            QMutexLocker lock(&m_bufMutex);
            // ggmorse requires exactly nMaxBytes — partial returns cause it to abort
            if (static_cast<uint32_t>(m_ringBuf.size()) < nMaxBytes) return 0;

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

        const auto& stats = ggmorse.getStatistics();

        // Accept all decodes — color-coded by confidence in the UI
        GGMorse::TxRx rxData;
        if (ggmorse.takeRxData(rxData) > 0 && stats.costFunction < 1.0f) {
            QString text = QString::fromLatin1(
                reinterpret_cast<const char*>(rxData.data()),
                static_cast<int>(rxData.size()));
            emit textDecoded(text, stats.costFunction);
        }

        if (stats.estimatedPitch_Hz > 0) {
            float pitch;
            float speed;
            {
                std::lock_guard lock(m_parametersMutex);
                // A just-completed old frame must not overwrite a newer lock
                // request. Locked setpoints live in the pending snapshot.
                // Nonpositive locks still mean automatic detection to GGMorse.
                if (!m_pitchLocked || m_pendingParameters.pitchHz <= 0.0f) {
                    m_pitch = stats.estimatedPitch_Hz;
                }
                if (!m_speedLocked || m_pendingParameters.speedWpm <= 0.0f) {
                    m_speed = stats.estimatedSpeed_wpm;
                }
                pitch = m_pitch;
                speed = m_speed;
            }
            emit statsUpdated(pitch, speed);
        }
    }

    qCDebug(lcDsp) << "CwDecoder: decode loop exiting, total frames:" << feedCount;
}

} // namespace AetherSDR
