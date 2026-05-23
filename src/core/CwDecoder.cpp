#include "CwDecoder.h"
#include "LogManager.h"
#include "ggmorse/ggmorse.h"
#include <cstring>

namespace MasterSDR {

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

    // Create ggmorse instance for 24kHz mono int16 input
    GGMorse::Parameters params;
    params.sampleRateInp = 24000.0f;
    params.sampleRateOut = 24000.0f;
    params.samplesPerFrame = GGMorse::kDefaultSamplesPerFrame;
    params.sampleFormatInp = GGMORSE_SAMPLE_FORMAT_I16;
    params.sampleFormatOut = GGMORSE_SAMPLE_FORMAT_I16;

    m_ggmorse = std::make_unique<GGMorse>(params);

    // Auto-detect pitch and speed
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = -1;  // auto
    dp.speed_wpm = -1;     // auto
    m_ggmorse->setParametersDecode(dp);

    m_running = true;

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.clear();
    }

    // Run decode loop on worker thread (CwDecoder stays on main thread)
    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("CwDecoder");
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    m_workerThread = worker;
    worker->start();

    qCDebug(lcDsp) << "CwDecoder: started";
}

void CwDecoder::stop()
{
    if (!m_running) return;
    m_running = false;

    if (m_workerThread) {
        m_workerThread->wait(2000);
        m_workerThread = nullptr;
    }

    m_ggmorse.reset();
    qCDebug(lcDsp) << "CwDecoder: stopped";
}

void CwDecoder::lockPitch(bool lock)
{
    m_pitchLocked = lock;
    if (!m_ggmorse) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = lock ? m_pitch.load() : -1.0f;
    dp.speed_wpm = m_speedLocked ? m_speed.load() : -1.0f;
    dp.frequencyRangeMin_hz = m_pitchRangeMin;
    dp.frequencyRangeMax_hz = m_pitchRangeMax;
    m_ggmorse->setParametersDecode(dp);
    qCDebug(lcDsp) << "CwDecoder: pitch" << (lock ? "locked at" : "unlocked from")
                   << m_pitch.load() << "Hz";
}

void CwDecoder::lockSpeed(bool lock)
{
    m_speedLocked = lock;
    if (!m_ggmorse) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = m_pitchLocked ? m_pitch.load() : -1.0f;
    dp.speed_wpm = lock ? m_speed.load() : -1.0f;
    dp.frequencyRangeMin_hz = m_pitchRangeMin;
    dp.frequencyRangeMax_hz = m_pitchRangeMax;
    m_ggmorse->setParametersDecode(dp);
    qCDebug(lcDsp) << "CwDecoder: speed" << (lock ? "locked at" : "unlocked from")
                   << m_speed.load() << "WPM";
}

void CwDecoder::setKnownParameters(float pitchHz, float speedWpm)
{
    if (pitchHz <= 0.0f || speedWpm <= 0.0f) return;

    const bool unchanged = qFuzzyCompare(m_pitch.load(), pitchHz)
        && qFuzzyCompare(m_speed.load(), speedWpm)
        && m_pitchLocked && m_speedLocked;
    if (unchanged) return;

    // Lock both pitch and speed to the P/CW applet values.  The local
    // CWX keyer / iambic keyer / etc. all run at the slider WPM, so
    // sidetone is generated at exactly that rate — ggmorse with both
    // values locked gets a reliable unit length and correctly classifies
    // 1u / 3u / 7u gaps so inter-word boundaries become " " separators.
    m_pitch = pitchHz;
    m_speed = speedWpm;
    m_pitchLocked = true;
    m_speedLocked = true;

    // Widen pitch range to comfortably include the known value (default
    // is 500–700 Hz but operators commonly use 700 / 750 / 800).  Also
    // drives ggmorse's internal HPF cutoff.
    constexpr float kPitchRangePad = 150.0f;
    m_pitchRangeMin = std::max(100.0f, pitchHz - kPitchRangePad);
    m_pitchRangeMax = pitchHz + kPitchRangePad;

    if (!m_ggmorse) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = pitchHz;
    dp.speed_wpm = speedWpm;
    dp.frequencyRangeMin_hz = m_pitchRangeMin;
    dp.frequencyRangeMax_hz = m_pitchRangeMax;
    m_ggmorse->setParametersDecode(dp);
    qCDebug(lcDsp) << "CwDecoder: known params pitch=" << pitchHz
                   << "Hz speed=" << speedWpm << "WPM";
}

void CwDecoder::setPitchRange(int minHz, int maxHz)
{
    m_pitchRangeMin = static_cast<float>(minHz);
    m_pitchRangeMax = static_cast<float>(maxHz);
    if (!m_ggmorse) return;
    GGMorse::ParametersDecode dp = GGMorse::getDefaultParametersDecode();
    dp.frequency_hz = m_pitchLocked ? m_pitch.load() : -1.0f;
    dp.speed_wpm = m_speedLocked ? m_speed.load() : -1.0f;
    dp.frequencyRangeMin_hz = m_pitchRangeMin;
    dp.frequencyRangeMax_hz = m_pitchRangeMax;
    m_ggmorse->setParametersDecode(dp);
    qCDebug(lcDsp) << "CwDecoder: pitch range" << minHz << "-" << maxHz << "Hz";
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
    // ggmorse requests samplesPerFrame * resampleFactor * sampleSize bytes per callback.
    // At 24kHz int16, factor=6 (24000/4000), frame=128: 128*6*2 = 1536 bytes.
    const int resampleFactor = static_cast<int>(m_ggmorse->getSampleRateInp() / GGMorse::kBaseSampleRate);
    const int bytesPerFrame = m_ggmorse->getSamplesPerFrame() * resampleFactor * m_ggmorse->getSampleSizeBytesInp();
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

        int framesThisCall = 0;

        bool gotData = m_ggmorse->decode([this, &framesThisCall](void* data, uint32_t nMaxBytes) -> uint32_t {
            if (!m_running) return 0;

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
            const auto& stats = m_ggmorse->getStatistics();
            const auto& rxData = m_ggmorse->getRxData();
            qCDebug(lcDsp) << "CwDecoder:" << feedCount << "frames fed, pitch:"
                     << stats.estimatedPitch_Hz << "Hz, speed:"
                     << stats.estimatedSpeed_wpm << "WPM, decode:" << gotData
                     << "rxLen:" << rxData.size()
                     << "lastResult:" << m_ggmorse->lastDecodeResult();
        }

        const auto& stats = m_ggmorse->getStatistics();

        // Accept all decodes — color-coded by confidence in the UI
        GGMorse::TxRx rxData;
        if (m_ggmorse->takeRxData(rxData) > 0 && stats.costFunction < 1.0f) {
            QString text = QString::fromLatin1(
                reinterpret_cast<const char*>(rxData.data()),
                static_cast<int>(rxData.size()));
            emit textDecoded(text, stats.costFunction);
        }

        if (stats.estimatedPitch_Hz > 0) {
            m_pitch = stats.estimatedPitch_Hz;
            m_speed = stats.estimatedSpeed_wpm;
            emit statsUpdated(m_pitch, m_speed);
        }
    }

    qCDebug(lcDsp) << "CwDecoder: decode loop exiting, total frames:" << feedCount;
}

} // namespace MasterSDR
