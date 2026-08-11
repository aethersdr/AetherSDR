#include "core/backends/sim/SimSignalSource.h"

namespace AetherSDR {

SimSignalSource::SimSignalSource(QObject* parent) : QObject(parent)
{
    // A member QObject has NO parent by default, and moveToThread() moves
    // children only — an unparented member timer stays on the constructing
    // thread and Qt refuses to start it from the worker ("timers cannot be
    // started from another thread"). Parenting it here makes it ride along.
    // Safe despite being a member: member destructors run before ~QObject,
    // so the timer removes itself from the child list before the base would
    // delete children. (Caught by sim_signal_source_test — zero frames.)
    m_timer.setParent(this);

    // Frame cadence: kFrameLen (128) samples at kSampleRate (24 kHz) = 5.333 ms
    // of audio per frame — NOT an integer number of milliseconds, so no fixed
    // interval can be right on its own. The timer only sets the CHECK cadence;
    // onTick() emits as many whole frames as real elapsed time has earned and
    // carries the remainder, so the long-run rate is exactly 24 kHz.
    //
    // 10 ms coarse, not 3 ms precise: this object lives on its own worker
    // (#4878), where a precise short timer buys nothing and a coarse one lets
    // the kernel batch wakeups. 10 ms earns 1–2 frames per tick; the burst cap
    // below covers stalls.
    m_timer.setTimerType(Qt::CoarseTimer);
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, &SimSignalSource::onTick);

    // The out-of-the-box scene: a pink-noise floor plus a birdie carrier — a
    // scene that immediately shows what NR/notch can do.
    m_audio.setEnabled(NoiseMixer::Channel::Pink, true);
    m_audio.setLevelDb(NoiseMixer::Channel::Pink, -22.0);
    m_audio.setEnabled(NoiseMixer::Channel::Birdie, true);
    m_audio.setLevelDb(NoiseMixer::Channel::Birdie, -18.0);
    m_audio.setKnob(NoiseMixer::Channel::Birdie, QStringLiteral("hz"), 1200.0);
}

void SimSignalSource::start()
{
    m_clock.invalidate();   // fresh pacing baseline; first frames next tick
    m_debtNs = 0;
    m_timer.start();
}

void SimSignalSource::stop()
{
    m_timer.stop();
    m_clock.invalidate();
    m_debtNs = 0;
}

void SimSignalSource::setKeyed(bool keyed)
{
    m_keyed = keyed;
}

void SimSignalSource::setScopeStalled(bool stalled)
{
    m_scopeStalled = stalled;
}

QByteArray SimSignalSource::toStereoBytes(const QVector<float>& mono)
{
    // AudioEngine::feedAudioData() wants 24 kHz STEREO float32: duplicate each
    // mono sample into L and R.
    QByteArray out;
    out.resize(mono.size() * 2 * static_cast<int>(sizeof(float)));
    auto* p = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < mono.size(); ++i) {
        p[2 * i] = mono[i];
        p[2 * i + 1] = mono[i];
    }
    return out;
}

void SimSignalSource::onTick()
{
    // Emit exactly as many whole frames as REAL elapsed time has earned since
    // the last tick — the pacing scheme is unchanged from the GUI-thread
    // original; only the thread it runs on moved (#4878).
    if (!m_clock.isValid()) {
        m_clock.start();
        m_debtNs = 0;
        return;   // establish the t0 baseline; first frames go out next tick
    }
    m_debtNs += m_clock.nsecsElapsed();
    m_clock.restart();

    constexpr qint64 kNsPerFrame =
        (static_cast<qint64>(NoiseMixer::kFrameLen) * 1'000'000'000)
        / NoiseMixer::kSampleRate;   // ~5'333'333 ns

    // Cap the catch-up burst so a long stall (debugger pause, scheduling gap)
    // can't dump seconds of audio at once; drop the excess debt instead.
    constexpr int kMaxFramesPerTick = 8;   // ~43 ms — plenty of headroom
    int frames = static_cast<int>(m_debtNs / kNsPerFrame);
    if (frames > kMaxFramesPerTick) {
        frames = kMaxFramesPerTick;
        m_debtNs = 0;   // resync after a gap rather than spiral
    } else {
        m_debtNs -= static_cast<qint64>(frames) * kNsPerFrame;
    }

    for (int i = 0; i < frames; ++i) {
        // Muted while keyed — a demo radio must never sound live on TX
        // (Principle VI).
        const QVector<float> frame = m_keyed
            ? QVector<float>(NoiseMixer::kFrameLen, 0.0f)
            : m_audio.mixFrame();
        const QByteArray stereo = toStereoBytes(frame);
        emit audioFrameReady(stereo);
        // Per-slice audio too — the TCI receiver channels are fed from
        // sliceAudioFrameReady (see the SimBackend original for the history).
        emit sliceAudioFrameReady(kSliceId, stereo);

        // A panadapter row a few times a second (~21 fps at the 5.33 ms
        // frame). The stallscope fault freezes the spectrum while audio keeps
        // flowing, to exercise the scope-stall watchdog.
        if (!m_scopeStalled && ++m_frames % kSpectrumRowEveryNFrames == 0) {
            constexpr int kBins = 1024;
            constexpr double kFloorDbm = -120.0;
            constexpr double kAudioSpanHz = 8000.0;   // ±4 kHz around the VFO
            const QVector<float> row =
                m_audio.spectrum(kBins, kFloorDbm, kAudioSpanHz, kBins / 2);
            const QByteArray bytes(
                reinterpret_cast<const char*>(row.constData()),
                row.size() * static_cast<int>(sizeof(float)));
            // One row PER LIVE PAN, addressed by index. The mix is computed
            // once — every demo pan views the same antenna — but each pan
            // gets its own emission, so the seam carries real multi-pan load
            // and RadioModel routes rows to the right pane (#4887 phase 4).
            for (const int pan : m_panIndices)
                emit spectrumFrameReady(pan, bytes);
        }
    }
}

void SimSignalSource::setPanIndices(const QList<int>& indices)
{
    m_panIndices = indices;
}

void SimSignalSource::setNoiseEnabled(const QString& channel, bool on)
{
    bool ok = false;
    const NoiseMixer::Channel c = NoiseMixer::fromName(channel, &ok);
    if (ok) m_audio.setEnabled(c, on);
}

void SimSignalSource::setNoiseLevel(const QString& channel, double levelDb)
{
    bool ok = false;
    const NoiseMixer::Channel c = NoiseMixer::fromName(channel, &ok);
    if (ok) m_audio.setLevelDb(c, levelDb);
}

void SimSignalSource::setNoiseKnob(const QString& channel, const QString& knob,
                                   double value)
{
    bool ok = false;
    const NoiseMixer::Channel c = NoiseMixer::fromName(channel, &ok);
    if (!ok) return;

    // The birdie's "hz" knob is an AUDIO pitch the operator dials, but the
    // birdie itself is anchored to an absolute RF carrier so it behaves like a
    // real signal under tuning: re-anchor the carrier to (current VFO +
    // requested pitch) and derive the pitch from there. Writing the knob
    // straight through would leave the carrier stale and the next VFO nudge
    // would silently discard what the operator dialled.
    if (c == NoiseMixer::Channel::Birdie && knob == QLatin1String("hz")) {
        m_birdieCarrierMhz = m_sliceFreqMhz + (m_lsb ? -value : value) / 1.0e6;
        updateBirdieFromVfo();
        return;
    }
    m_audio.setKnob(c, knob, value);
}

void SimSignalSource::loadPreset(const QString& presetName)
{
    m_audio.loadPreset(presetName);
}

void SimSignalSource::setAnf(bool on)
{
    // Auto-notch: engage → notch the active tonal channels (birdie/cw), which
    // the mixer identifies via autoNotchTones(). Off → clear.
    if (on) m_audio.setNotches(m_audio.autoNotchTones());
    else    m_audio.setNotches({});
}

void SimSignalSource::setNb(bool on)
{
    m_audio.setNoiseBlank(on);
}

void SimSignalSource::setVfoMhz(double mhz)
{
    m_sliceFreqMhz = mhz;
    updateBirdieFromVfo();
}

void SimSignalSource::setLowerSideband(bool lsb)
{
    m_lsb = lsb;
    updateBirdieFromVfo();
}

void SimSignalSource::updateBirdieFromVfo()
{
    // Audio pitch from the RF offset, per sideband:
    //   USB: pitch = carrier - VFO;  LSB: pitch = VFO - carrier.
    // A carrier on the "wrong" side or outside the audio passband is silent —
    // passing 0 tells NoiseMixer "not audible". Megahertz-scale offsets MUST
    // go silent rather than alias back into the audio band as a screech.
    static constexpr double kMinAudibleHz = 50.0;    // below: zero-beat
    static constexpr double kMaxAudibleHz = 6000.0;  // generous passband,
                                                     // inside 12 kHz Nyquist
    const double offsetHz = (m_birdieCarrierMhz - m_sliceFreqMhz) * 1.0e6;
    const double audioHz = m_lsb ? -offsetHz : offsetHz;
    const bool audible = audioHz >= kMinAudibleHz && audioHz <= kMaxAudibleHz;
    m_audio.setKnob(NoiseMixer::Channel::Birdie, QStringLiteral("hz"),
                    audible ? audioHz : 0.0);
}

}  // namespace AetherSDR
