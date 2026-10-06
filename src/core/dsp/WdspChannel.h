#pragma once

#include <QMetaType>

#include <atomic>
#include "core/WfmReceptionDiagnostics.h"
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
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

    // Opt-in receive FM recipe. Other channel owners retain their existing
    // settings. The FM limiter and unity output panel normalize demodulated
    // audio before it reaches a speaker mixer or an independent receiver tap.
    struct FmReceive
    {
        // Deviation has one owner: Config::fmDeviationHz, including runtime setters.
        bool operator==(const FmReceive&) const = default;
    };

    // Broadcast receive recipe from WDSP Guide section 5.3.10. This opt-in
    // owns an RF FIR before WDSP's 384 -> 192 kHz conversion and normalizes
    // paired audio before any receiver tap. Other channel owners are unchanged.
    struct WbfmReceive
    {
        enum class Deemphasis { Us75, Us50 };
        Deemphasis deemphasis = Deemphasis::Us75;
        double outputGain = 0.25;
        bool forceMono = false;
        static constexpr double kRfTransitionGuardHz = 3000.0;
        bool operator==(const WbfmReceive&) const = default;
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
        std::optional<FmReceive> fmReceive;
        std::optional<WbfmReceive> wbfmReceive;
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
        // FM detector deviation in Hz: the assumed incoming deviation, not a filter.
        // WDSP turns it into audio gain (again = rate / (deviation * TWOPI)); 5000 is
        // RXA.c's default. In Config, not just a setter, because reconfigure() closes
        // and reopens the channel and would revert it. Bounded because a tiny positive
        // value drives the output to inf; one pair shared by setFmDeviation(),
        // validateConfig() and RtlReceiverRegistry::boundedDsp(). Floor is below any
        // NFM service (2.5 kHz in Europe); ceiling above broadcast FM's 75 kHz.
        static constexpr double kMinFmDeviationHz = 100.0;
        static constexpr double kMaxFmDeviationHz = 100000.0;
        double fmDeviationHz = 5000.0;
        // true: fexchange waits for each output block. The r1/r2 buffer phase is
        // still set per channel open (flush_iobuffs() drains with a 1 ms timed
        // wait), so two opens fed the same burst can differ (#5629).
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
        // CW audio peaking filter (setApf()). In Config because reconfigure()
        // frees the peaking stages; defaults are RXA.c's own (off, 600 Hz,
        // 100 Hz, linear gain 2.0).
        bool apfEnabled = false;
        double apfCenterHz = 600.0;
        double apfBandwidthHz = 100.0;
        double apfGain = 2.0;   // LINEAR, not dB
        // Receive squelch — see setSquelch() below. In Config for the same
        // reason as the blanker and the FM deviation: reconfigure() frees all
        // three WDSP squelch stages, so a squelch held only in a runtime setter
        // would silently open on the next sample-rate change. The level is
        // the seam's 0..100 (SliceModel's), and 20 is SliceModel's default.
        bool squelchEnabled = false;
        int squelchLevel = 20;
        // Added to the amsq map; see setSquelch().
        double levelSquelchOffsetDb = 0.0;
        bool operator==(const Config&) const = default;
    };

    // Which WDSP squelch stage a mode uses — see squelchStageFor().
    enum class SquelchStage
    {
        None,   // no squelch in this mode; every stage is held off
        Fm,     // fmsq.c — detector-noise squelch
        Level   // amsq.c — carrier/signal-level squelch (AM and SSB families)
    };

    // What the last squelch application wrote to WDSP: one run flag per stage
    // and the threshold handed to the mode's stage in that stage's units (linear
    // detector noise for Fm, dBFS for Level, 0 for None). `stage` is the mode's
    // stage even when nothing runs (squelch off, or level 0).
    struct AppliedSquelch
    {
        SquelchStage stage = SquelchStage::None;
        bool fmRun = false;
        bool amRun = false;
        double threshold = 0.0;
        // How many times the run flags have been written. Lets a caller tell
        // "re-applied, same answer" from "never applied".
        unsigned applications = 0;
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

    // Control-side, all-or-nothing admission against the SAME pool used by
    // create(). Unconsumed slots return automatically; no FFTW work is done.
    class Reservation final
    {
    public:
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        ~Reservation();
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        [[nodiscard]] std::size_t remaining() const noexcept { return m_count; }

    private:
        friend class WdspChannel;
        Reservation() = default;
        void release() noexcept;
        std::array<int, 32> m_ids {};
        std::size_t m_count = 0;
    };

    [[nodiscard]] static std::optional<Reservation> reserveChannels(std::size_t count);

    static std::unique_ptr<WdspChannel> create(const Config& config,
                                               std::string* error = nullptr) noexcept;
    static std::unique_ptr<WdspChannel> create(const Config& config,
        Reservation& reservation, std::string* error = nullptr) noexcept;

    ~WdspChannel();

    WdspChannel(const WdspChannel&) = delete;
    WdspChannel& operator=(const WdspChannel&) = delete;
    WdspChannel(WdspChannel&&) = delete;
    WdspChannel& operator=(WdspChannel&&) = delete;

    ProcessResult processIq(std::span<const float> inputI,
                            std::span<const float> inputQ,
                            std::span<float> outputLeft,
                            std::span<float> outputRight) noexcept;
    // True when the next processIq() would find a whole output block, i.e. not
    // return Underrun. A non-blocking caller handed several blocks at once must
    // ask first: an underrun leaves the two-slot output ring out of step for
    // good. Same thread and fence as processIq(); false during a control
    // operation. Local WDSP patch 15.
    [[nodiscard]] bool outputReady() noexcept;

    // Start/stop — the T/R call, not teardown. Stop runs the mute envelope down and
    // flushes, keeping FFTW plans, masks, notches, AGC/shift and the blanker; no
    // allocation (allocationSequenceForTest() is unchanged). CloseChannel happens
    // only in close().
    // Non-blocking (dmode 0): the drain happens on the feeding thread, so a
    // blocking stop would time out at 100 ms and abandon the ramp. While stopped,
    // processIq() plays out the ramp, then returns zeros.
    // With vendored patches 4, 7, 8 and 9 (third_party/wdsp/AETHERSDR-PATCHES.md):
    //   * stop/start at any spacing is safe (7 cancels a pending down-ramp; 8
    //     waits out a finished ramp's flush); start may block up to 100 ms
    //     (in practice < 3 ms).
    //   * clocking a stopped channel then destroying it needs no ordering (9; 4
    //     keeps the worker from parking on a drained token).
    // Pinned by runRestartDuringRampTest and runCloseAfterStoppedClockingTest.
    // Guarded like setMode(): returns false if a control op is in flight; never
    // call from processIq(). Setting the current state is a no-op success.
    [[nodiscard]] bool setRunning(bool running) noexcept;
    // TX only: discard queued samples and filter history without clocking a fade.
    // Leaves the channel stopped, retaining its plans/configuration. Control-path
    // operation: uses existing channel locks and refreshes the output semaphore.
    // Refuses while a callback or an asynchronous fade/flush is outstanding.
    [[nodiscard]] bool discardTransmitData() noexcept;
    [[nodiscard]] bool isRunning() const noexcept
    {
        return m_running.load(std::memory_order_relaxed);
    }

    // The caller must stop feeding processIq() before a control operation.
    // Rebuilds the channel, and therefore DESTROYS what setRunning() preserves
    // (the notch database most visibly — see addNotch()'s note). It does
    // restore the running state it found, the way WDSP's own rebuilds do, so
    // reconfiguring a stopped channel does not put it back on the air.
    bool reconfigure(const Config& config, std::string* error = nullptr) noexcept;
    bool setMode(Mode mode) noexcept;
    bool setFilter(double lowHz, double highHz) noexcept;
    bool setWbfmDeemphasis(WbfmReceive::Deemphasis deemphasis) noexcept;
    // Lock-free latest completed output-mode observation, sampled by processIq().
    // Forced Mono is false independently of reception diagnostics.pilotLocked.
    // It is not an exact PCM-ring timestamp. False after stop/open; absent for
    // channels that do not own the broadcast recipe. Caller owns the lifetime.
    [[nodiscard]] std::optional<bool> wbfmStereoDetected() const noexcept;
    // Acquisition thread only, immediately after processIq; control may read
    // after joining/quiescing acquisition. Never reaches into mutable WDSP.
    [[nodiscard]] const std::optional<AetherSDR::WfmReceptionDiagnostics>&
        wbfmReceptionDiagnostics() const noexcept { return m_wbfmReception; }
    // Runtime RX AGC change. agcMode is the WDSP RXA AGC mode (0 off, 1 long,
    // 2 slow, 3 medium, 4 fast); maximumGainDb is the AGC "top", the ceiling on
    // how much gain the AGC may apply. Receive channels only — returns false on
    // a transmit channel, on a non-finite ceiling, or if a control operation is
    // already in flight. Control-path work, guarded exactly like setMode(); it
    // must not be called from the processIq() callback.
    bool setAgc(int agcMode, double maximumGainDb) noexcept;
    // The AGC's gain in mode 0 (off), in dB; xwcpagc() ignores it in every
    // other mode, so it may be set any time. Receive only; refuses a
    // non-finite value or a racing control operation.
    bool setAgcFixedGain(double fixedGainDb) noexcept;
    // CW audio peaking filter (WDSP SPCW, apfshadow.c): centerHz is audio (the
    // CW pitch), gain is linear. Receive only; refuses a non-positive or
    // non-finite parameter. While it runs both outputs come from I alone
    // (xdoublepole mode 2 copies I into Q), so any left/right difference is
    // lost. Stored in Config so open() re-applies it.
    bool setApf(bool enabled, double centerHz, double bandwidthHz, double gain) noexcept;
    [[nodiscard]] static bool apfParametersValid(double centerHz, double bandwidthHz,
                                                 double gain) noexcept;

    // Runtime filter length / phase mode without reconfigure() (which would
    // destroy the notch database).
    // setFilterTaps(): notch width floor 200 Hz at 2048 taps, 50 Hz at 8192
    // (minimumNotchWidthHz()); group delay (taps-1)/2 samples (21.3 / 85.3 ms at
    // 48 kHz). Notches and shift survive (RXASetNC -> setNc_nbp ->
    // calc_nbp_impulse). Stops the channel (outside the lock; see the .cpp),
    // re-plans six FIR cores under the FFTW lock (1.8-28.8 ms), restarts; don't
    // poll it. `taps` must be a power of two in [256, 16384] and a multiple of
    // dspBlockSize (firmin.h), as validateConfig() checks.
    // minimumNotchWidthHz() reads Config::filterTaps unlocked: fine while all
    // callers are on the control thread.
    // setMinimumPhase(): same magnitude response, collapsed group delay; RXASetMP
    // rebuilds all six masks without stopping.
    // Both RX-only, control-path (not from processIq()), idempotent in WDSP.
    bool setFilterTaps(int taps) noexcept;
    bool setMinimumPhase(bool on) noexcept;

    // Runtime FM detector deviation (Hz), RX only. Accepted in any mode but
    // audible only in FM (the fmd stage runs only when selected), so call order
    // with setMode() doesn't matter. Not WBFM: RXA_WBFM uses wbfm.c with a fixed
    // 75 kHz discriminator scale and no deviation setter, so this is inert there.
    // Returns false on a TX channel (TX uses SetTXAFMDeviation), outside
    // Config::kMinFmDeviationHz..kMaxFmDeviationHz, or if a control op is in
    // flight. Guarded like setMode(); never from processIq().
    bool setFmDeviation(double deviationHz) noexcept;

    // Receive squelch: the mode picks one WDSP stage, the other is forced off;
    // level 100 is tightest, and setMode() re-applies it.
    //   level 0                  nothing runs, in every mode ("0 = open")
    //   FM                       fmsq, threshold 10^(-2 * level / 100)
    //   AM, SAM, DSB, LSB, USB   amsq, threshold -140 + 0.7 * level + offset dBFS
    //   CW, DIG, WBFM, other     none
    // amsq gates raw dBFS, so a front-end gain change moves its antenna-referred
    // point; `levelOffsetDb` is how the caller, which owns that gain, refers it
    // back. The channel has no gain of its own to refer to (#6092). SSB is on
    // amsq because ssql never opens at this chain's audio level (#5982).
    // Receive only; false while a control operation is in flight; not from processIq().
    bool setSquelch(bool on, int level, double levelOffsetDb) noexcept;
    [[nodiscard]] static SquelchStage squelchStageFor(Mode mode) noexcept;
    static constexpr double kLevelSquelchBaseDbfs = -140.0;
    static constexpr double kLevelSquelchDbPerStep = 0.7;
    // The two maps above, each clamping level to 0..100, so tests can pin them.
    [[nodiscard]] static double fmSquelchThresholdForLevel(int level) noexcept;
    [[nodiscard]] static double levelSquelchThresholdDbfsForLevel(int level,
                                                                  double offsetDb) noexcept;
    // A snapshot by value, safe from any thread: guarded by its own mutex, not
    // g_setupMutex (the FFTW planner lock, which can be held for a whole plan).
    [[nodiscard]] AppliedSquelch appliedSquelch() const;

    // Impulse noise blanker: WDSP ANB (nob.c) on the RAW IQ ahead of the channel,
    // before the bandpass smears impulses. RX only; TX returns false. `level` is
    // 0..100 in seam units (larger = more aggressive); WDSP's threshold runs the
    // other way, inverted only in noiseBlankerThresholdForLevel(). Guarded like
    // setMode(); never from processIq(). Enabling flushes the blanker first.
    bool setNoiseBlanker(bool on, int level) noexcept;
    // Real-time-safe suspend for backends that clock the channel with silence
    // during TX. The blanker fires on magnitude > threshold * running average, so
    // zeros would decay the average and gate off the start of every RX period.
    // Held, the stage is skipped and its average frozen; release must NOT flush
    // (that leaves it blind ~200 ms; see processIq()). Safe from the processIq()
    // thread: it only stores an atomic.
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

    // Manual notches: the host-side Flex TNF equivalent, and the only notch on a
    // direct-sampling radio (HL2 has no radio-side DSP; oracle addendum 3 §B4).
    // Centres and the notch tune frequency are ABSOLUTE RF Hz; WDSP subtracts the
    // tune frequency, so notches stay on the carrier. Callers must update the
    // tune frequency on every NCO change.
    // No sign correction: centres share an axis with the passband bounds in
    // calc_nbp()/make_nbp(), so HL2's two flips cancel here as for the
    // demodulator (see Hl2RxDsp::onIqBlock); negating would mirror every notch.
    // `index` is positional in a dense array: addNotch() shifts later entries up,
    // removeNotch() down; callers with stable ids must remap after removal.
    // RX only; guarded like setMode(), never from processIq(). reconfigure()
    // destroys the notch database; re-add notches afterwards, like the shift.
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
    // The WDSP channel number this object owns.
    //
    // A PRODUCTION ACCESSOR, despite the alias below. Two log lines read it --
    // Hl2RxDsp::stop and AnanRxDsp's equivalent, both naming the channel in a
    // warning an operator is expected to act on -- and a name ending in
    // ForTest is precisely what a cleanup strips or wraps in an ifdef, which
    // would take those log lines with it. Renamed on #5738 after
    // aethersdr-agent noticed the two non-test callers.
    [[nodiscard]] int channelId() const noexcept { return m_channelId; }

    // Retained so the existing test call sites keep compiling. New code wants
    // channelId(); this spelling says only "a test wrote it first".
    [[nodiscard]] int channelIdForTest() const noexcept { return channelId(); }

    static uint64_t allocationSequenceForTest() noexcept;
    static uint64_t outstandingAllocationsForTest() noexcept;
    // Process-global, test only (#5734, AetherSDR WDSP patch 13): the DSP
    // worker sleeps this long right after it has released a blocked
    // processIq(). That is the window in which, before patch 13, the host
    // overwrote the input block the worker had not yet copied out. 0 restores
    // the shipping path.
    static void setWorkerHandoffPauseForTest(unsigned microseconds) noexcept;
    // Test-only deterministic counterpart: arm the next worker handoff, then
    // acknowledge that one worker is held until explicitly released. Fixtures
    // serialize use, keep other channels idle, and release BEFORE destruction
    // or reconfiguration. The default is inactive; this never holds acquisition.
    static void setWorkerHandoffHoldForTest(bool enabled) noexcept;
    [[nodiscard]] static bool workerHandoffHeldForTest() noexcept;
    // Same serialized-fixture lifetime contract, before output bytes/credit.
    static void setWorkerOutputCopyHoldForTest(bool enabled) noexcept;
    [[nodiscard]] static bool workerOutputCopyHeldForTest() noexcept;
    // Test/control thread only: locks existing exchange/output-count sections. The
    // caller owns this open channel and excludes control/destruction.
    [[nodiscard]] int outputSamplesReadyForTest() const noexcept;
    // After output-count readiness, wait for the last hook and DSP body before
    // arming another hold. Test/control only; NEVER call while a hold is armed.
    void synchronizeWorkerForTest() const noexcept;
    // Test only: the next `count` control operations on THIS channel are
    // refused exactly as a racing processIq() callback would refuse them, so
    // a caller's refused-then-converges path can be driven deterministically.
    // 0 (the default) costs one relaxed load per control operation.
    void refuseControlOperationsForTest(unsigned count) noexcept
    {
        m_refuseControlForTest.store(count, std::memory_order_relaxed);
    }

    // Forwards to AetherSDR::fftwPlannerLock() (core/dsp/FftwPlannerLock.h), which
    // owns the mutex and lists its users. Code outside this class should call
    // fftwPlannerLock() directly. Scope width is per call site; allocations belong
    // inside it (#5424).
    [[nodiscard]] static std::unique_lock<std::mutex> fftwSetupLock();

private:
    explicit WdspChannel(int channelId, const Config& config) noexcept;

    static bool validateConfig(const Config& config, std::string* error) noexcept;
    static int wdspMode(Mode mode) noexcept;
    // Pushes the NBP stage's copy of the shift. WDSP keeps the shift stage and
    // the notch database's idea of the shift as two unrelated values, so a
    // caller that sets one without the other gets notches that sit exactly one
    // shift away from where they were placed.
    void applyNotchShift() noexcept;
    // Writes all three squelch run flags (and the running stage's threshold)
    // for `mode` from m_config's squelch pair. Caller holds g_setupMutex and
    // has checked the channel is a receive channel.
    void applySquelchLocked(Mode mode) noexcept;
    static std::size_t computeOutputBlockSize(const Config& config) noexcept;

    [[nodiscard]] bool open() noexcept;
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

    struct WbfmState;
    std::unique_ptr<WbfmState> m_wbfm;
    std::atomic<int> m_wbfmStereo {-1};
    std::optional<AetherSDR::WfmReceptionDiagnostics> m_wbfmReception;
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
    // WDSP's channel state, mirrored. Atomic because processIq() consults it on
    // the real-time path to decide whether it owns the output buffer this
    // block, the same way it consults m_nbActive — the control handshake
    // already orders the write, this keeps the read from being a data race.
    std::atomic<bool> m_running {false};
    // Written under m_appliedSquelchMutex (with g_setupMutex already held,
    // always in that order); read under it alone — see appliedSquelch().
    mutable std::mutex m_appliedSquelchMutex;
    AppliedSquelch m_appliedSquelch;
    std::atomic<unsigned> m_refuseControlForTest {0};

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
