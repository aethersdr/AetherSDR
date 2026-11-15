#include "core/WfmDemodulator.h"
#include "core/WfmDsp.h"
#include "core/WaveOutWriter.h"
#include "core/LogManager.h"
#include "models/DaxIqModel.h"

#include <QMediaDevices>
#include <cmath>
#include <cstring>

namespace AetherSDR {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QAudioDevice findCaptureDevice(const QStringList& hints)
{
    const auto inputs = QMediaDevices::audioInputs();
    for (const QAudioDevice& dev : inputs) {
        const QString desc = dev.description().toLower();
        for (const QString& h : hints)
            if (desc.contains(h.toLower())) return dev;
    }
    return {};
}

// ---------------------------------------------------------------------------
// WfmDemodulator — lifecycle
// ---------------------------------------------------------------------------

WfmDemodulator::WfmDemodulator(QObject* parent) : QObject(parent) {}

WfmDemodulator::~WfmDemodulator() { stop(); }

void WfmDemodulator::start(DaxIqModel* daxIq, const QString& deviceId,
                           const QString& panId)
{
    if (m_active) stop();

    m_daxIq           = daxIq;
    m_panId           = panId;
    m_panSent         = false;
    m_usingDaxCapture = false;
    m_dsp.reset();

    qCDebug(lcAudio) << "WfmDemodulator::start device=" << deviceId;

    // --- Primary path: SmartSDR DAX IQ capture device (Windows) ---
    const QStringList daxHints = { "dax iq rx 1", "dax iq 1",
                                   "dax reserved iq rx 1", "dax iq" };
    QAudioDevice capDev = findCaptureDevice(daxHints);

    if (!capDev.isNull()) {
        // Capture at the device's NATIVE rate. Forcing 48 kHz here makes the
        // OS mixer resample silently to whatever rate DAX is really set to
        // (24/48/96/192 k) — a 24 k stream upsampled by Windows has no energy
        // above ±12 kHz (the historical "waterfall notch"). WfmDsp resamples
        // to exactly 48 kHz itself, flat to 0.95·Nyquist.
        const QAudioFormat preferred = capDev.preferredFormat();
        const int nativeRate =
            preferred.sampleRate() > 0 ? preferred.sampleRate() : AUDIO_RATE;

        QAudioFormat fmt;
        fmt.setSampleRate(nativeRate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!capDev.isFormatSupported(fmt))
            fmt = preferred;

        const int rate = fmt.sampleRate();
        m_captureFormat = fmt;

        qCInfo(lcAudio) << "WfmDemodulator: DAX device=" << capDev.description()
                        << "rate=" << rate
                        << "ch=" << fmt.channelCount()
                        << "fmt=" << fmt.sampleFormat();

        if (rate < AUDIO_RATE)
            qCWarning(lcAudio) << "WfmDemodulator: DAX IQ runs at" << rate
                               << "Hz — usable IQ window is only ±" << rate / 2000.0
                               << "kHz. Set the DAX IQ channel to 48 kHz or higher.";

        if (fmt.channelCount() == 2) {
            m_waveOut = new WaveOutWriter(this);
            if (!m_waveOut->open(deviceId, AUDIO_RATE, 2)) {
                qCWarning(lcAudio) << "WfmDemodulator: cannot open audio output" << deviceId;
                delete m_waveOut; m_waveOut = nullptr;
                return;
            }

            m_iqDevice = new DaxIqCaptureDevice(this);
            m_iqDevice->open(QIODevice::WriteOnly);
            connect(m_iqDevice, &DaxIqCaptureDevice::pcmReady,
                    this,       &WfmDemodulator::onDaxIqPcm);

            m_iqSource = new QAudioSource(capDev, fmt, this);
            m_iqSource->start(m_iqDevice);

            if (m_iqSource->error() == QAudio::NoError) {
                m_usingDaxCapture = true;
                ensureDsp(rate);
                qCInfo(lcAudio) << "WfmDemodulator: DAX capture active at" << rate << "Hz";
                // Bind DAX IQ channel to the slice's panadapter so the IQ stream
                // is centred on (and follows) the pan centre. Without this the
                // channel keeps whatever pan it was last assigned to and the
                // signal can fall outside the ±fs/2 IQ window entirely.
                if (!m_panId.isEmpty()) {
                    m_panSent = m_iqPanBinder && m_iqPanBinder(m_panId, DAX_CHANNEL);
                }
            } else {
                qCWarning(lcAudio) << "WfmDemodulator: DAX capture failed, error="
                                   << m_iqSource->error() << "→ fallback VITA-49";
                m_iqSource->stop();
                delete m_iqSource;  m_iqSource = nullptr;
                delete m_iqDevice;  m_iqDevice = nullptr;
                delete m_waveOut;   m_waveOut  = nullptr;
            }
        } else {
            qCWarning(lcAudio) << "WfmDemodulator: DAX device is not stereo ("
                               << fmt.channelCount() << "ch) → fallback VITA-49";
        }
    } else {
        qCDebug(lcAudio) << "WfmDemodulator: DAX device not found";
    }

    // --- Fallback: VITA-49 DaxIqModel (Linux/macOS, or no DAX) ---
    if (!m_usingDaxCapture) {
        if (!m_daxIq || m_daxIq->stream(DAX_CHANNEL).exists || m_daxIq->createPending(DAX_CHANNEL)) {
            qCWarning(lcAudio) << "WfmDemodulator: IQ channel is already in use";
            return;
        }
        m_waveOut = new WaveOutWriter(this);
        if (!m_waveOut->open(deviceId, AUDIO_RATE, 2)) {
            qCWarning(lcAudio) << "WfmDemodulator: cannot open audio output" << deviceId;
            delete m_waveOut; m_waveOut = nullptr;
            return;
        }
        connect(m_daxIq.data(), &DaxIqModel::iqSamplesReady, this, &WfmDemodulator::onIqSamples);
        connect(m_daxIq.data(), &DaxIqModel::streamChanged,  this, &WfmDemodulator::onStreamChanged);
        if (!m_daxIq->createStream(DAX_CHANNEL)) {
            m_waveOut->close();
            m_waveOut->deleteLater();
            m_waveOut = nullptr;
            disconnect(m_daxIq.data(), nullptr, this, nullptr);
            return;
        }
        m_iqRevision = m_daxIq->streamRevision(DAX_CHANNEL);
        qCDebug(lcAudio) << "WfmDemodulator: VITA-49 fallback, panId=" << m_panId;
    }

    m_active = true;
}

void WfmDemodulator::stop()
{
    if (!m_active) return;
    m_active = false;

    if (m_iqSource) { m_iqSource->stop(); delete m_iqSource; m_iqSource = nullptr; }
    if (m_iqDevice) { m_iqDevice->close(); delete m_iqDevice; m_iqDevice = nullptr; }
    if (m_daxIq) {
        if (!m_usingDaxCapture && m_daxIq->streamRevision(DAX_CHANNEL) == m_iqRevision) {
            m_daxIq->removeStream(DAX_CHANNEL);
        }
        disconnect(m_daxIq.data(), nullptr, this, nullptr);
        m_daxIq = nullptr;
    }
    if (m_waveOut) { m_waveOut->close(); delete m_waveOut; m_waveOut = nullptr; }
    m_dsp.reset();
}

// ---------------------------------------------------------------------------
// Offset / Doppler control
// ---------------------------------------------------------------------------

void WfmDemodulator::setFreqOffsetHz(float offsetHz)
{
    m_freqOffsetHz = offsetHz;
    if (m_dsp) m_dsp->setFreqOffsetHz(offsetHz);
}

float WfmDemodulator::maxFreqOffsetHz() const
{
    // 14 800 = 0.95·24 kHz − 8 kHz guard, the 48 k-rate value, used until the
    // actual IQ rate is known (VITA path creates the DSP on first samples).
    return m_dsp ? m_dsp->maxFreqOffsetHz() : 14800.0f;
}

void WfmDemodulator::ensureDsp(int iqRateHz)
{
    if (m_dsp && m_dsp->iqRateHz() == iqRateHz) return;
    m_dsp = std::make_unique<WfmDsp>(iqRateHz);
    m_dsp->setFreqOffsetHz(m_freqOffsetHz);
    qCInfo(lcAudio) << "WfmDemodulator: DSP chain IQ rate" << iqRateHz
                    << "Hz → 48000 Hz out, max offset"
                    << m_dsp->maxFreqOffsetHz() << "Hz";
}

// ---------------------------------------------------------------------------
// Slot handlers
// ---------------------------------------------------------------------------

void WfmDemodulator::onStreamChanged(int channel)
{
    if (!m_daxIq) { return; }
    const auto& s = m_daxIq->stream(DAX_CHANNEL);
    if (channel != DAX_CHANNEL || m_panSent || m_panId.isEmpty()) return;
    if (!s.exists || s.streamId == 0) return;
    m_panSent = m_iqPanBinder && m_iqPanBinder(m_panId, DAX_CHANNEL);
}

void WfmDemodulator::onIqSamples(int channel, QVector<float> iq, int sampleRate)
{
    if (channel != DAX_CHANNEL || !m_active || !m_waveOut) return;
    ensureDsp(sampleRate > 0 ? sampleRate : AUDIO_RATE);
    processIqFloat(iq.constData(), iq.size() / 2);
}

void WfmDemodulator::onDaxIqPcm(const QByteArray& pcm)
{
    if (!m_active || !m_waveOut || !m_dsp) return;

    // Convert the capture format to interleaved float IQ
    if (m_captureFormat.sampleFormat() == QAudioFormat::Int16) {
        const int n = pcm.size() / (2 * static_cast<int>(sizeof(qint16)));
        if (n <= 0) return;
        const auto* raw = reinterpret_cast<const qint16*>(pcm.constData());
        m_iqConvBuf.resize(static_cast<size_t>(n) * 2);
        for (int i = 0; i < n * 2; ++i)
            m_iqConvBuf[static_cast<size_t>(i)] = raw[i] / 32768.0f;
        processIqFloat(m_iqConvBuf.data(), n);
    } else if (m_captureFormat.sampleFormat() == QAudioFormat::Float) {
        const int n = pcm.size() / (2 * static_cast<int>(sizeof(float)));
        if (n <= 0) return;
        processIqFloat(reinterpret_cast<const float*>(pcm.constData()), n);
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qCWarning(lcAudio) << "WfmDemodulator: unsupported capture format"
                               << m_captureFormat.sampleFormat();
        }
    }
}

// ---------------------------------------------------------------------------
// processIqFloat — WfmDsp (NCO → 48 kHz → discriminator → FIR), then
// clamp + volume → Float32 stereo → ring buffer → QAudioSink
// ---------------------------------------------------------------------------

void WfmDemodulator::processIqFloat(const float* iq, int frames)
{
    if (!m_dsp || frames <= 0) return;

    // Idle-input gate (NOT a squelch): a stopped DAX IQ stream delivers only
    // zeros or ±1 LSB dither (RMS ≈ 2e-5). The atan2 discriminator is
    // amplitude-invariant, so it would happily demodulate that dither into
    // full-scale noise. Any real IQ window — even antenna disconnected — has
    // an ADC noise floor orders of magnitude above the enter threshold.
    // Hysteresis (enter 1e-4 / leave 2e-4) prevents chattering at the edge.
    double pwr = 0.0;
    for (int i = 0; i < frames * 2; ++i)
        pwr += static_cast<double>(iq[i]) * iq[i];
    const float iqRms = std::sqrt(static_cast<float>(pwr / frames));
    constexpr float kIdleEnterRms = 1e-4f;   // −80 dBFS per complex sample
    constexpr float kIdleLeaveRms = 2e-4f;
    const bool idle = m_idle ? (iqRms < kIdleLeaveRms) : (iqRms < kIdleEnterRms);
    if (idle != m_idle) {
        m_idle = idle;
        qCInfo(lcAudio) << "WfmDemodulator: IQ stream"
                        << (idle ? "idle (silence to VAC)" : "active")
                        << "rms=" << iqRms;
    }

    // Run the DSP even when idle so filter/resampler state and the output
    // sample count stay correct; only the written audio is muted.
    m_dsp->process(iq, frames, m_audioBuf);
    const int n = static_cast<int>(m_audioBuf.size());
    if (n <= 0) return;

    QByteArray pcm(n * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* out = reinterpret_cast<float*>(pcm.data());
    if (m_idle) {
        std::memset(out, 0, static_cast<size_t>(pcm.size()));
    } else {
        for (int i = 0; i < n; ++i) {
            const float s = std::clamp(m_audioBuf[static_cast<size_t>(i)], -1.0f, 1.0f)
                          * m_volume;
            out[2 * i]     = s;
            out[2 * i + 1] = s;
        }
    }

    // Diagnostic log ~every 2 s
    static int s_blk = 0;
    if (++s_blk % 100 == 0) {
        float iqRms = 0.0f, audMax = 0.0f;
        for (int i = 0; i < frames; ++i)
            iqRms += iq[2 * i] * iq[2 * i] + iq[2 * i + 1] * iq[2 * i + 1];
        for (int i = 0; i < n; ++i)
            audMax = std::max(audMax, std::abs(out[2 * i]));
        iqRms = std::sqrt(iqRms / static_cast<float>(frames));
        qCDebug(lcAudio) << "WfmDemodulator blk#" << s_blk
                         << "IQ_rms=" << iqRms
                         << "audio_max=" << audMax
                         << "offset=" << m_freqOffsetHz
                         << "path=" << (m_usingDaxCapture ? "dax-capture" : "vita49");
    }

    m_waveOut->write(pcm);
}

} // namespace AetherSDR
