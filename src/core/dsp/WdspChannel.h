#pragma once

#include <QMetaType>

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Owns one complete WDSP channel and hides WDSP's process-global numeric
// channel table. Construction, reconfiguration, and filter changes are control-
// thread operations; processIq() is the allocation-free real-time operation.
class WdspChannel final
{
public:
    enum class Direction
    {
        Receive,
        Transmit
    };

    enum class Mode
    {
        Lsb,
        Usb,
        Dsb,
        Cwl,
        Cwu,
        Fm,
        Am,
        Digu,
        Spec,
        Digl,
        Sam,
        Drm,
        Wbfm
    };

    struct Config
    {
        Direction direction = Direction::Receive;
        std::size_t inputBlockSize = 1024;
        std::size_t dspBlockSize = 1024;
        int inputSampleRate = 48000;
        int dspSampleRate = 48000;
        int outputSampleRate = 48000;
        Mode mode = Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        int agcMode = 3;
        double maximumAgcGainDb = 120.0;
        // Output level difference between very weak and very strong signals.
        // WDSP defaults this to 0, which is TOTAL compression — every signal
        // and the noise floor between them come out at the same level, so the
        // ceiling is applied to noise and the result clips. pihpsdr sets 35.
        int agcSlopeDb = 35;
        // Gain applied when the AGC is OFF. Never setting it leaves WDSP's
        // default, which is why "AGC off" was the loudest and worst-clipping
        // setting of all rather than the quietest.
        double agcFixedGainDb = 10.0;
        // Channel mute envelope, in seconds. WDSP applies these when a channel
        // starts and stops, and they are the anti-click mechanism: an abrupt DSP
        // mute clicks on every transition, which on a full-duplex radio means
        // every T/R change. Values match both reference clients (Thetis
        // cmaster.c, pihpsdr receiver.c) — leaving them at zero, as this did,
        // disables the ramp entirely and is invisible until you go hunting for
        // the click.
        double muteDelayUpSec = 0.010;
        double muteSlewUpSec = 0.025;
        double muteDelayDownSec = 0.000;
        double muteSlewDownSec = 0.010;
        // Bandpass filter length and phase mode — the selectivity/latency
        // trade. More coefficients sharpen the skirt and add delay;
        // minimum-phase trades linear phase for lower latency. 2048 is WDSP's
        // own default (max(2048, dsp_size)), so this changes nothing on its own
        // — it makes the value explicit and tunable instead of implicit.
        // pihpsdr runs 8192 by comparison.
        int filterTaps = 2048;
        bool minimumPhase = false;
        bool blockForOutput = false;
        // Impulse noise blanker — see the setNoiseBlanker() block below. Kept
        // in Config, not just as a runtime setter, so that reconfigure() (a
        // sample-rate or block-size change) rebuilds the channel with the
        // operator's blanker rather than silently switching it off, the same
        // way the shift and the AGC are carried.
        bool noiseBlankerEnabled = false;
        // 0..100, the seam's units. Mapped to WDSP's threshold by
        // noiseBlankerThresholdForLevel().
        int noiseBlankerLevel = 50;
    };

    enum class ProcessResult
    {
        Ok,
        Underrun,
        Busy,
        InvalidBuffer,
        AllocationViolation,
        EngineError
    };

    static std::unique_ptr<WdspChannel> create(const Config& config,
                                               std::string* error = nullptr) noexcept;

    ~WdspChannel();

    WdspChannel(const WdspChannel&) = delete;
    WdspChannel& operator=(const WdspChannel&) = delete;
    WdspChannel(WdspChannel&&) = delete;
    WdspChannel& operator=(WdspChannel&&) = delete;

    ProcessResult processIq(std::span<const float> inputI,
                            std::span<const float> inputQ,
                            std::span<float> outputLeft,
                            std::span<float> outputRight) noexcept;

    // The caller must stop feeding processIq() before a control operation.
    bool reconfigure(const Config& config, std::string* error = nullptr) noexcept;
    bool setMode(Mode mode) noexcept;
    bool setFilter(double lowHz, double highHz) noexcept;
    // Runtime RX AGC change. agcMode is the WDSP RXA AGC mode (0 off, 1 long,
    // 2 slow, 3 medium, 4 fast); maximumGainDb is the AGC "top", the ceiling on
    // how much gain the AGC may apply. Receive channels only — returns false on
    // a transmit channel, on a non-finite ceiling, or if a control operation is
    // already in flight. Control-path work, guarded exactly like setMode(); it
    // must not be called from the processIq() callback.
    bool setAgc(int agcMode, double maximumGainDb) noexcept;
    // ── Impulse noise blanker ─────────────────────────────────────────────
    //
    // WDSP's ANB (nob.c), run on the RAW IQ ahead of the channel. It has to be
    // ahead: an impulse is short in time and wide in frequency, so once the
    // bandpass has smeared it across milliseconds there is no longer a spike to
    // blank. That is also why it cannot be an RXA stage — WDSP does not put one
    // there, and both reference clients call it outside the channel too.
    //
    // Receive channels only. A transmit channel returns false rather than
    // blanking the operator's own audio.
    //
    // `level` is 0..100 in the SEAM's units, not WDSP's: the slice model's NB
    // level, where LARGER means MORE aggressive. WDSP's own parameter runs the
    // other way (it is a multiple of the running average magnitude, so smaller
    // triggers more often), and noiseBlankerThresholdForLevel() is the one
    // place that inversion happens.
    //
    // Control-path work, guarded exactly like setMode(); not callable from
    // processIq(). Turning the blanker ON flushes it first, so it starts from a
    // known state rather than from whatever the last enabled period left.
    bool setNoiseBlanker(bool on, int level) noexcept;
    // Real-time-safe suspend, for a backend that clocks the channel with
    // SILENCE while transmitting.
    //
    // This exists because the blanker's trigger is relative, not absolute: it
    // fires on magnitude > threshold * running-average-magnitude. Feed it a
    // transmit period of zeros and that average decays to nothing, so the first
    // real receive sample afterwards is thousands of times the average and the
    // blanker gates the audio off — a hole at the start of every receive
    // period, in the same family as the NR filter-state gap. Held, the stage is
    // skipped entirely and its average is FROZEN at the pre-transmit signal
    // level — so releasing the hold does NOT flush it, and must not: flushing
    // re-arms the average from full scale and buys ~200 ms of blind blanker at
    // the head of every receive period, which is the very hole this exists to
    // close. See the note in processIq().
    //
    // Safe from the processIq() thread, unlike every other control call here:
    // it stores an atomic and the flush it schedules writes only into buffers
    // the stage already owns.
    void setNoiseBlankerHold(bool hold) noexcept;
    [[nodiscard]] bool noiseBlankerEnabled() const noexcept
    {
        return m_config.noiseBlankerEnabled;
    }
    // The seam-level -> WDSP-threshold map, exposed because it is a policy
    // decision rather than an implementation detail and a test should be able
    // to pin it. Geometric, so equal steps of the slider are equal RATIOS of
    // the trigger; 100 at level 0 (so conservative it effectively never fires)
    // down to 4 at level 100, passing through 20 at the slice model's default
    // level of 50 — 20 being the fixed value pihpsdr ships with no slider at
    // all, so the default reproduces the reference client exactly.
    [[nodiscard]] static double noiseBlankerThresholdForLevel(int level) noexcept;

    // RX frequency shift in Hz, relative to the tuned (NCO) frequency. A
    // single-DDC backend uses this to move the slice inside the passband
    // without moving the NCO, which is what keeps the panadapter still while
    // the operator tunes. 0 disables the stage. Receive channels only.
    bool setShift(double shiftHz) noexcept;

    // ── Manual notch filters ──────────────────────────────────────────────
    //
    // The host-side equivalent of a Flex TNF. On a direct-sampling radio this
    // is the only notch that exists — the HL2 protocol carries no DSP at all,
    // so a notch either happens in WDSP or nowhere (HL2 oracle addendum 3 §B4).
    //
    // Centres and the tune frequency are ABSOLUTE RF Hz. WDSP subtracts the
    // tune frequency internally when it rebuilds the filter mask, which is what
    // makes a notch track the interferer instead of the audio pitch: tune away
    // and the notch stays on the carrier. Callers therefore set the notch tune
    // frequency on every NCO change; nothing else does it for them.
    //
    // No sign correction, on a conjugated input or otherwise. The notch centres
    // share an axis with the passband bounds — calc_nbp() feeds flow/fhigh and
    // fcenter into make_nbp() together — so whatever inversion applies to one
    // applies to the other. On the HL2 that means the two documented flips (a
    // wire that is the conjugate of the analytic signal, and an RXA that
    // selects the opposite sign to its passband bounds) cancel here exactly as
    // they cancel for the demodulator; see the handedness note in
    // Hl2RxDsp::onIqBlock. Negating anything here would UNDO a working
    // cancellation and put every notch on its own mirror image.
    //
    // `index` is a POSITIONAL handle into a dense array, not a stable id.
    // addNotch() inserts at `index` and shifts everything above it up;
    // removeNotch() closes the gap and shifts everything above it down. A
    // caller holding stable ids of its own must remap after every removal or it
    // will silently edit a different notch than it meant to.
    //
    // Receive channels only — a TX channel has no notch database. All of these
    // are control-path operations guarded exactly like setMode(), so they must
    // not be called from the processIq() callback.
    //
    // reconfigure() DESTROYS the notch database, because it closes and reopens
    // the channel and the database belongs to the channel. A caller that keeps
    // notches across a sample-rate or block-size change has to re-add them
    // afterwards, the same way it re-applies the shift.
    bool addNotch(int index, double centerHz, double widthHz, bool active) noexcept;
    bool editNotch(int index, double centerHz, double widthHz, bool active) noexcept;
    bool removeNotch(int index) noexcept;
    // Global run flag for the whole database — the equivalent of a Flex
    // tnf_enabled. Individual notches keep their own `active` flag under it.
    bool setNotchesEnabled(bool on) noexcept;
    // The tuned (NCO) frequency notch centres are measured against.
    bool setNotchTuneFrequency(double tuneHz) noexcept;
    [[nodiscard]] int notchCount() const noexcept;
    // Reads back what WDSP actually stored at `index`. The width is the width
    // as REQUESTED, not as applied — WDSP keeps the request and widens it when
    // it builds the mask, so this cannot be used to discover that a notch was
    // silently widened. Compare against minimumNotchWidthHz() for that.
    [[nodiscard]] bool notchAt(int index, double* centerHz, double* widthHz,
                               bool* active) const noexcept;
    // The narrowest notch this channel can actually produce, in Hz.
    //
    // Worth checking before offering a width to the operator, because WDSP
    // enforces this floor SILENTLY: the stage runs with auto-increment on, so a
    // narrower request is widened rather than refused, and the only evidence is
    // that the notch eats more of the band than the UI is drawing. The floor is
    // a function of filter length — 200 Hz at 2048 taps, 50 Hz at 8192 — so it
    // moves when Config::filterTaps does.
    [[nodiscard]] double minimumNotchWidthHz() const noexcept;
    // WDSP meter readout in dBFS-relative units. Meter is the RXA meter type
    // (0 = S peak, 1 = S average, 2 = ADC peak, 3 = ADC average, 4 = AGC gain).
    // Read-only and cheap — safe to call from a timer. Returns a large negative
    // value on a transmit channel, which has no RXA meters.
    enum class Meter { SignalPeak = 0, SignalAverage = 1,
                       AdcPeak = 2, AdcAverage = 3, AgcGain = 4 };
    [[nodiscard]] double meter(Meter which) const noexcept;

    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] std::size_t outputBlockSize() const noexcept;
    [[nodiscard]] int channelIdForTest() const noexcept { return m_channelId; }

    static uint64_t allocationSequenceForTest() noexcept;
    static uint64_t outstandingAllocationsForTest() noexcept;

    // Where open() writes the FFTW wisdom cache. Exposed so a test can assert
    // the file is on disk once open() has RETURNED — the defect it pins is
    // wisdom that only ever reached disk through an atexit handler, which every
    // signal-terminated run skipped. Reading the path instead of hardcoding it
    // keeps the test honest about the env vars wisdomPath() actually consults.
    static std::string wisdomCachePathForTest();

private:
    explicit WdspChannel(int channelId, const Config& config) noexcept;

    static bool validateConfig(const Config& config, std::string* error) noexcept;
    static int wdspMode(Mode mode) noexcept;
    // Pushes the NBP stage's copy of the shift. WDSP keeps the shift stage and
    // the notch database's idea of the shift as two unrelated values, so a
    // caller that sets one without the other gets notches that sit exactly one
    // shift away from where they were placed.
    void applyNotchShift() noexcept;
    static std::size_t computeOutputBlockSize(const Config& config) noexcept;

    void open() noexcept;
    void close() noexcept;
    // Create/destroy the ANB stage alongside the channel. The blanker's id IS
    // the channel id: nob.c keeps its own table of 32, WDSP's channel table is
    // also 32, and this class already owns the allocation and release of that
    // number — so borrowing it gives the two objects one lifetime instead of
    // two that can fall out of step. Nothing else in this process may create an
    // ANB without going through here.
    void openNoiseBlanker() noexcept;
    void closeNoiseBlanker() noexcept;
    bool beginControlOperation() noexcept;
    void endControlOperation() noexcept;

    int m_channelId = -1;
    Config m_config;
    // Fixed for a given Config; cached at open()/reconfigure() so the real-time
    // processIq() buffer-size check does not repeat a divide every block.
    std::size_t m_outputBlockSize = 0;
    double m_shiftHz = 0.0;
    // These two coordinate the real-time processIq() against control-thread
    // operations. The handshake is Dekker-style — each side stores its own flag
    // then reads the other's — which is only correct under sequential
    // consistency, so every access below uses memory_order_seq_cst. Do NOT relax
    // these to acquire/release: acq_rel does not order a store then a load of a
    // different atomic across threads, and both sides could then proceed at once.
    std::atomic<unsigned> m_callbacksInFlight {0};
    std::atomic<bool> m_controlOperation {false};
    bool m_open = false;

    // ── Noise blanker state ───────────────────────────────────────────────
    //
    // m_nbOpen tracks the WDSP-side stage (created for the life of a receive
    // channel whether or not it is running); m_nbActive is what processIq()
    // reads to decide whether to pay for the interleave at all. Separate,
    // because the stage exists while switched off and the real-time path must
    // not consult m_config across a control-thread write.
    bool m_nbOpen = false;
    std::atomic<bool> m_nbActive {false};
    // Set on a transmit edge; processIq() skips the stage while it is true so
    // the running average is FROZEN rather than dragged into the mute path's
    // silence. No companion "flush on release" flag: re-arming the stage on
    // every T->R edge is the defect this hold exists to avoid, not part of it.
    std::atomic<bool> m_nbHold {false};
    // ANB works on WDSP's interleaved-double buffer, in place; processIq()
    // takes separate float planes. These three are the staging buffers for
    // that conversion, sized once in open() so the real-time path never
    // allocates. Blanking has to happen before the channel sees the samples,
    // so there is no way to reuse the caller's buffers — they are const.
    std::vector<double> m_nbInterleaved;
    std::vector<float> m_nbI;
    std::vector<float> m_nbQ;
};

// Mode crosses a thread boundary as a queued Q_ARG (Hl2Backend marshals control
// verbs onto its I/O thread). An unregistered type there does not fail loudly --
// invokeMethod just warns and DROPS the call, which would silently break mode
// switching.
Q_DECLARE_METATYPE(WdspChannel::Mode)
