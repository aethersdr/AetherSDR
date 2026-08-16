#pragma once

#include <QElapsedTimer>
#include <QObject>

#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <vector>

#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

namespace AetherSDR::hl2 {

// The HL2 receive DSP stage: turns raw IQ blocks (from MetisClient::iqBlockReady)
// into demodulated audio (WdspChannel), a panadapter spectrum (Hl2Spectrum), and
// an S-meter. Buffers the odd 126-sample EP6 blocks into WdspChannel's fixed
// processing block. Below the seam; the eventual Hl2Backend owns one and runs it
// on the backend's I/O thread. This stage is receive-only — Hl2TxDsp is its
// transmit counterpart — and it MUTES on transmit, clocking its audio channel
// with silence so the pipeline cannot fill with our own signal.
class Hl2RxDsp : public QObject {
    Q_OBJECT

public:
    explicit Hl2RxDsp(QObject* parent = nullptr);
    ~Hl2RxDsp() override;

    // WDSP's internal DSP rate. Constant at 48 kHz and independent of both the
    // HL2 IQ rate and the audio rate — see the note in configure().
    static constexpr int kWdspDspSampleRateHz = 48000;

    // RX filter length. This is also the manual-notch resolution: WDSP's
    // narrowest notch is 1600 / (taps/256) Hz at kWdspDspSampleRateHz, and it
    // WIDENS a narrower request instead of rejecting it. 8192 taps buys a 50 Hz
    // floor (pihpsdr's value); WDSP's own default of 2048 would floor it at
    // 200 Hz, wide enough to swallow a CW signal next to the carrier being
    // notched. Keep kMinNotchWidthHz in step if this changes — the UI offers
    // widths from it.
    static constexpr int kRxFilterTaps = 8192;
    static constexpr double kMinNotchWidthHz =
        1600.0 / (static_cast<double>(kRxFilterTaps) / 256.0)
        * (static_cast<double>(kWdspDspSampleRateHz) / 48000.0);
    static_assert(kMinNotchWidthHz <= 50.0,
                  "RX filter taps no longer allow a 50 Hz notch; the width "
                  "presets in the TNF menu assume one.");

    struct Config {
        int inputSampleRateHz = 48000;   // HL2 IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's native
        // RX rate (AudioEngine::DEFAULT_SAMPLE_RATE); emitting it directly means
        // the relay hands the engine byte-compatible float32 stereo with no
        // resampling. WDSP does the IF->audio decimation, and every HL2 IQ rate
        // (48/96/192/384 kHz) divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        int fftSize = 1024;              // panadapter FFT size
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // RX AGC. WdspChannel's own defaults are mode 3 (medium) and a 120 dB
        // ceiling — 120 dB is the TOP of WDSP's AGC gain range, so leaving it
        // there runs the HL2 wide open and slams the demodulated audio past
        // full scale (measured: peak 3.19, 10% of samples clipping on WWV).
        // 39 dB is the slice model's default threshold of 65 through the
        // 0..100 -> 0..60 dB map (see Hl2Backend::setSliceAgc). Measured clean
        // on live hardware; the previous 65 dB clipped 60% of samples.
        int agcMode = 3;
        double maximumAgcGainDb = 39.0;   // = slice default 65 * 0.6
        // false (live): processIq is non-blocking — real-time input paces WDSP's
        // async worker and audio flows with ~1 block latency. true: processIq
        // waits for each output block (deterministic for a burst/offline feed).
        bool blockForOutput = false;
    };

    // (Re)build the WdspChannel + Hl2Spectrum for this config. Returns false (and
    // sets error, if given) when the WDSP channel cannot be created.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    // Runtime AGC change. agcMode is the WDSP RXA AGC mode; maximumGainDb is
    // the AGC ceiling. Kept in m_config so a later reconfigure() (rate change)
    // rebuilds the channel with the operator's current AGC rather than the
    // construction-time default.
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // RX frequency shift in Hz relative to the NCO — how the backend tunes the
    // slice inside the passband without moving the DDC. Kept in m_config so a
    // later reconfigure() rebuilds the channel with the operator's offset.
    Q_INVOKABLE void setShift(double shiftHz);
    // Cap how often a panadapter frame is produced, in frames per second.
    //
    // The FFT is SKIPPED entirely when a frame is not due, which is the whole
    // point: this backend's natural frame rate is the IQ sample rate over the
    // FFT size — 48000/1024 = 47 fps at the narrowest span but 384000/1024 =
    // 375 fps at the widest — so the display rate used to track the operator's
    // ZOOM rather than their Display->FFT FPS slider, and widening the span
    // multiplied the render load eightfold.
    //
    // Limiting HERE rather than downstream is what makes it cheap. At 384 kHz
    // and a 25 fps target this skips ~93% of the FFTs and the 1024-bin
    // magnitude/log pass behind each one; coalescing the frames after the fact
    // would compute every one of them and then spend MORE cpu combining them.
    //
    // fps <= 0 removes the cap. The rate is applied on a wall clock, so it
    // holds across a sample-rate change without needing to be recomputed.
    Q_INVOKABLE void setSpectrumRateFps(int fps);

    // Impulse noise blanker, on the raw IQ ahead of the demodulator.
    //
    // THE ONLY NOISE BLANKER THIS RADIO HAS. The HL2 ships raw IQ and runs no
    // firmware DSP, so — exactly like the manual notch — this either happens on
    // this host or it does not happen at all. That is why the NB button is
    // visible on a radio that reports hasRadioSideDsp = false.
    //
    // Runs inside WdspChannel::processIq(), on the wire samples, immediately
    // ahead of fexchange2. Placement is not a style choice: an impulse is
    // narrow in time and wide in frequency, and once the bandpass has spread it
    // over milliseconds there is no spike left to remove.
    //
    // ON THE AUDIO PATH ONLY. processIqBlock() feeds the panadapter from its
    // own conjugated copy BEFORE it reaches the channel, so the spectrum and
    // waterfall show UNBLANKED IQ. That is deliberate — the display is a
    // measurement of what is on the air and blanking it would hide the very
    // impulses the operator is deciding whether to blank — but it is a visible
    // difference from a Flex, which blanks in its own DDC upstream of both.
    // If that ever needs to change, the stage moves up here rather than gaining
    // a second copy inside the channel.
    //
    // `level` is 0..100, larger being more aggressive; WdspChannel owns the map
    // onto WDSP's inverted threshold.
    //
    // Held OUTSIDE Config, like the shift and the notch set and for the same
    // reason: configure() REPLACES m_config, so a rate change carrying a
    // caller's default would switch the blanker off while the operator's NB
    // button stayed lit. Anything that must outlive a rebuild lives in its own
    // member and is re-applied at the end of configure().
    Q_INVOKABLE void setNoiseBlanker(bool on, int level);
    // What the operator ASKED for. Survives configure() and is what a rebuild
    // re-applies.
    [[nodiscard]] bool noiseBlankerEnabled() const { return m_nbOn; }
    [[nodiscard]] int noiseBlankerLevel() const { return m_nbLevel; }
    // What the WDSP stage ACTUALLY has, which is not the same question. The
    // request crosses a queued connection to get here and WdspChannel can
    // refuse it outright (a control operation already in flight), so a readback
    // that reported the request back would be certifying its own input.
    //
    // ATOMIC because these two are the only members of this class read from
    // OUTSIDE its thread: Hl2Backend answers the bridge's `hl2 nb.get` from the
    // GUI thread while this object lives on the I/O thread. Relaxed is enough —
    // they are independent scalars and nothing is ordered against them.
    [[nodiscard]] bool appliedNoiseBlankerEnabled() const
    {
        return m_nbAppliedOn.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int appliedNoiseBlankerLevel() const
    {
        return m_nbAppliedLevel.load(std::memory_order_relaxed);
    }

    // ── Manual notch filters ──────────────────────────────────────────────
    //
    // `index` is WDSP's POSITIONAL handle, and Hl2Backend is what maps stable
    // notch ids onto it — this class just does as it is told, in the order it
    // is told, so the two stay in step across every receiver.
    //
    // Centres are ABSOLUTE RF Hz. setNotchTuneFrequency() must follow the NCO,
    // or the notches stay where the NCO used to be.
    //
    // The set is MIRRORED here as well as in WDSP, because reconfigure()
    // destroys the notch database along with the channel. Without the copy, an
    // operator's notches vanish on a sample-rate change — the same reason the
    // shift is kept.
    Q_INVOKABLE void addNotch(int index, double centerHz, double widthHz, bool active);
    Q_INVOKABLE void editNotch(int index, double centerHz, double widthHz, bool active);
    Q_INVOKABLE void removeNotch(int index);
    // Empty both the mirror and WDSP's database. This is what makes seeding
    // IDEMPOTENT: Hl2Backend::seedNotches() clears before it replays, so a
    // second seed against a chain that already holds the set replaces it
    // instead of appending a duplicate of every notch.
    Q_INVOKABLE void clearNotches();
    Q_INVOKABLE void setNotchesEnabled(bool on);
    Q_INVOKABLE void setNotchTuneFrequency(double tuneHz);

    // Mirror size, and WDSP's own count. These must agree — the positional
    // index map depends on it — so both are exposed for the test that pins it.
    [[nodiscard]] int notchCount() const;
    [[nodiscard]] int wdspNotchCount() const;

    // Mute the DEMODULATOR while transmitting.
    //
    // Suppressing audio further downstream is not enough: this pipeline keeps
    // demodulating our own transmission, WDSP's filters and buffers fill with
    // it, and the moment the mute lifts that backlog drains to the speakers —
    // heard as the tail end of a transmission playing back after unkey.
    //
    // Muted, the SPECTRUM still runs on real IQ so the panadapter keeps
    // updating, but the audio channel is clocked with SILENCE. The pipeline
    // therefore stays running at constant latency and contains nothing but
    // silence when transmit ends.
    Q_INVOKABLE void setAudioMuted(bool muted);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

    // The WDSP channel id this chain was actually given, or -1 before configure().
    //
    // Ids come from a PROCESS-WIDE pool of 32 shared with the transmit chain and
    // every other backend, so this is whatever was free — NOT the receiver's own
    // index, and not stable across a reconnect. Hl2Backend records it in the
    // index-space map (Hl2Receivers.h) precisely so nothing has to derive it.
    [[nodiscard]] int wdspChannelId() const noexcept
    {
        return m_channel ? m_channel->channelIdForTest() : -1;
    }

    // Demodulated-audio DC blocker, one pole per channel.
    //
    // WDSP's AM/SAM detector is an ENVELOPE detector — amd.c emits
    // sqrt(I^2 + Q^2), which is strictly non-negative — so the carrier arrives
    // as a DC pedestal. Nothing upstream removes it:
    //
    //   * `levelfade` (ON by default, RXA.c) computes
    //     `audio += dc_insert - dc` from an 8 Hz-corner and a 0.11 Hz-corner
    //     average. That cancels FADING and deliberately holds the pedestal at
    //     the long-term carrier level; it is not a DC blocker.
    //   * The AM/SAM passband is symmetric about the carrier ({-4000, +4000}
    //     in Hl2Backend::defaultPassbandForMode) because both detectors need
    //     it that way, which puts 0 Hz mid-band.
    //
    // Left in, the pedestal eats output headroom and skews every
    // zero-referenced consumer downstream — the WAVE applet draws `peak` and
    // `rms`, both magnitudes, mirrored about a hard centreline, so a DC-shifted
    // trace renders as a second inverted phantom copy of itself in the lower
    // half.
    //
    // SCOPE: this runs on WdspChannel's OUTPUT, downstream of the whole RXA
    // chain, so it fixes the audio we deliver but NOT what WDSP itself saw.
    // wcpAGC sits after amd INSIDE RXA and still rides the pedestal; correcting
    // that would need DC removal between the two, which WDSP exposes no hook
    // for. Turning `levelfade` off does not help either — it only stops the
    // fade correction, leaving sqrt(I^2+Q^2) just as non-negative.
    //
    // WHY HERE AND NOT IN WdspChannel: the root cause is amd's envelope
    // detector, which belongs to WDSP, so a blocker on WdspChannel's own RX
    // output would fix it once for every future consumer instead of per caller.
    // Hl2RxDsp is the only WDSP RECEIVE consumer in the tree today — Hl2TxDsp is
    // the one other user and is transmit-only — so per-caller costs nothing yet.
    // It does mean the next WDSP RX path added will NOT inherit this: whoever
    // adds one should push the blocker down into WdspChannel rather than repeat
    // it here.
    //
    // Applied to EVERY mode rather than switched on for AM/SAM: SSB, CW and
    // the digital modes are already zero-mean so it is a no-op there, FM wants
    // it for the same reason AM does, and an unconditional filter has no
    // mode-change state that can be got wrong.
    //
    // Public, with the pole helper below, so hl2_am_dcblock_test can pin the
    // unconfigured bypass and the rate-dependent corner directly. Neither is
    // reachable through configure(), which only ever hands it a valid rate.
    struct DcBlocker {
        float r = 0.0f;    // pole radius, set by configure(); <= 0 bypasses
        float x1 = 0.0f;
        float y1 = 0.0f;

        [[nodiscard]] float process(float x) noexcept
        {
            // Bypass rather than filter when unconfigured. `y = x - x1 + r*y1`
            // at r = 0 is a DIFFERENTIATOR, not a passthrough, so a missed
            // configure() would otherwise gut the low end of the audio.
            if (!(r > 0.0f))
                return x;
            float y = x - x1 + r * y1;
            // Flush to zero. During silence y1 decays geometrically toward
            // denormal, and denormal arithmetic is punitively slow on x86.
            if (!(std::fabs(y) > 1e-20f))
                y = 0.0f;
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept { x1 = 0.0f; y1 = 0.0f; }
    };

    // -3 dB corner, Hz. Well below the 100 Hz that even a wide AM passband
    // carries as audio, and far below the CW pitch, so it removes the pedestal
    // without touching program material.
    static constexpr double kDcBlockerCornerHz = 20.0;

    // Pole radius placing DcBlocker's -3 dB corner at `cornerHz`. Split out of
    // configure() so the corner can be asserted at more than one audio rate:
    // hardcoding the pole for one rate leaves the filter working and the corner
    // silently wrong, which no end-to-end DC measurement notices.
    [[nodiscard]] static float dcBlockerPole(double cornerHz,
                                             double sampleRateHz) noexcept
    {
        return static_cast<float>(
            std::exp(-2.0 * std::numbers::pi * cornerHz / sampleRateHz));
    }

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady per FFT
    // frame and audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    void meterUpdate(float dbfs);                           // audio-RMS S-meter

private:
    // True when the next panadapter frame may be computed. Stays true until one
    // actually completes, since a frame spans several EP6 blocks.
    bool spectrumFrameDue();

    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<Hl2Spectrum> m_spectrum;
    double m_shiftHz = 0.0;   // current slice offset from the NCO, Hz
    // Noise-blanker state, kept out of m_config so configure() cannot clear it.
    // m_nbOn/m_nbLevel are the REQUEST; m_nbApplied* are what the WDSP stage
    // took. They diverge exactly when something went wrong, which is the whole
    // reason the bridge readback reports the applied pair.
    bool m_nbOn = false;
    int  m_nbLevel = 50;      // 0..100, the slice model's units
    std::atomic<bool> m_nbAppliedOn {false};
    std::atomic<int>  m_nbAppliedLevel {50};
    Config m_config;

    // Notch set, mirrored so reconfigure() can replay it — see the note on
    // addNotch(). Index in this vector IS the WDSP notch index; keeping them
    // identical is the entire trick, and it works because every mutation here
    // performs the same insert/erase WDSP performs.
    struct Notch {
        double centerHz = 0.0;
        double widthHz = 0.0;
        bool active = true;
    };
    std::vector<Notch> m_notches;
    bool m_notchesEnabled = true;
    double m_notchTuneHz = 0.0;

    bool m_audioMuted = false;
    // Panadapter frame-rate cap. 0 = uncapped. m_spectrumClock is started on
    // the first block and only read/written on the DSP thread.
    int m_spectrumIntervalMs = 0;
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;
    std::vector<std::complex<float>> m_iqBuffer;   // IQ awaiting a full DSP block
    // Wire IQ conjugated into the analytic convention, for the SPECTRUM only —
    // the demodulator takes the raw wire. A member rather than a local: this
    // runs per IQ block on the I/O thread.
    std::vector<std::complex<float>> m_conjugated;
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    // Applied to m_left/m_right on the way into m_stereo. See DcBlocker above
    // for why AM/SAM need it, and for what it deliberately does not fix.
    DcBlocker m_dcBlockL, m_dcBlockR;
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch
};

}  // namespace AetherSDR::hl2
