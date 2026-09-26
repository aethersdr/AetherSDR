#pragma once

#include <QMetaType>

#include <atomic>
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

    // WHICH of WDSP's two impulse blankers runs ahead of the channel. Both are
    // created for the life of a receive channel and at most one runs; see
    // openNoiseBlanker().
    //
    // Values are the seam's (AetherSDR::NoiseBlankerKind) and are asserted equal
    // where the two meet, in core/backends/WdspNoiseBlanker.h. This enum is
    // declared here rather than included from there because core/dsp is a leaf:
    // it depends on Qt and the standard library and nothing else in this tree,
    // and an engine that had to include the seam's vocabulary to blank an
    // impulse would be the wrong dependency direction to open for two enums.
    enum class NoiseBlanker
    {
        Off = 0,
        Impulse = 1,   // ANB, nob.c — zeroes the window
        Advanced = 2,  // NOB, nobII.c — reconstructs it, per NoiseBlankerFill
    };

    // WDSP's own numbering for what fills the blanked window (nobII.c, the
    // switch on a->mode), so pushing it to SetEXTNOBMode is a cast and not a
    // table. Advanced only; Impulse always zeroes.
    enum class NoiseBlankerFill
    {
        Zero = 0,
        SampleHold = 1,
        MeanHold = 2,
        HoldSample = 3,
        Interpolate = 4,
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
        // FM detector deviation in Hz — the receiver's ASSUMPTION about how
        // wide the incoming signal is deviated, not a filter width. WDSP turns
        // it into an inverse audio gain (again = rate / (deviation * TWOPI)),
        // so this scales recovered audio and narrows nothing; see the block
        // above SetRXAFMDeviation in aether_wdsp.h. 5000 is what RXA.c builds
        // the stage with, so the default changes nothing on its own.
        //
        // IN Config RATHER THAN ONLY A SETTER, for the same reason the noise
        // blanker is: reconfigure() closes and reopens the channel, which frees
        // the fmd stage and everything set on it. A deviation held only in a
        // runtime setter would revert to 5 kHz on the next sample-rate or
        // block-size change, silently and with nothing to read that says so.
        //
        // BOUNDED, AND THE BOUND IS THE REFUSAL. A sign check is not enough:
        // 1e-40 is positive and finite, and `again = rate / (deviation *
        // TWOPI)` then runs away until the detector's float output is inf.
        // That was measured on this branch before these two numbers existed,
        // not reasoned about. One pair, shared by setFmDeviation(),
        // validateConfig() and RtlReceiverRegistry::boundedDsp(), so the three
        // doors cannot drift apart. The floor sits below any narrow-FM service
        // in use (2.5 kHz in Europe); the ceiling sits above broadcast FM's
        // 75 kHz, which this detector does not demodulate anyway.
        static constexpr double kMinFmDeviationHz = 100.0;
        static constexpr double kMaxFmDeviationHz = 100000.0;
        double fmDeviationHz = 5000.0;
        bool blockForOutput = false;
        // Impulse noise blanker — see the setNoiseBlanker() block below. Kept
        // in Config, not just as a runtime setter, so that reconfigure() (a
        // sample-rate or block-size change) rebuilds the channel with the
        // operator's blanker rather than silently switching it off, the same
        // way the shift and the AGC are carried.
        NoiseBlanker noiseBlanker = NoiseBlanker::Off;
        // 0..100, the seam's units. Mapped to WDSP's threshold by
        // noiseBlankerThresholdForLevel(). Shared by both blankers, as it is in
        // deskHPSDR — the trigger is the same detector in both stages, so one
        // number keeps a switch between them from also changing sensitivity.
        int noiseBlankerLevel = 50;
        // What Advanced puts in the blanked window. Carried even while Impulse
        // runs, so switching between them does not reset the operator's choice.
        NoiseBlankerFill noiseBlankerFill = NoiseBlankerFill::Zero;
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

    // ── Start and stop, which are NOT teardown ────────────────────────────
    //
    // The T/R call. Stopping runs the Config mute envelope DOWN, flushes the
    // chain, and leaves everything the channel owns in place: the FFTW plans,
    // the filter masks, the notch database, the AGC and shift state, the noise
    // blanker stage. Starting runs the envelope back up. Nothing is allocated
    // or freed either way, which is the whole point — `allocationSequenceForTest()`
    // does not move across a stop/start, and does move across a reconfigure().
    //
    // CloseChannel is not this and must not be used as this. It frees the
    // channel, so a "stop" written as a close throws all of the above away and
    // pays a rebuild to get it back — on this codebase that rebuild is FFTW
    // planning (see loadWisdomOnce() in the .cpp), which is the expensive half
    // of a connect. Closing is teardown, and this class does it in exactly one
    // place: close(), reached from the destructor and from reconfigure().
    //
    // DMODE, and why a running stop cannot use the blocking form. WDSP's stop
    // sets a down-slew flag and a flush flag, and clearing them takes TWO hops,
    // not one: the next fexchange0/fexchange2 calls run the down-slew and
    // release the channel's Sem_Flush when it completes (the ReleaseSemaphore
    // calls at the tail of fexchange0/fexchange2, iobuffs.c), and WDSP's
    // per-channel flushChannel thread wakes on that semaphore, flushes, and
    // clears the flush flag (the tail of flushChannel, channel.c). Either way
    // the drain
    // starts on the thread that is feeding the channel, not inside
    // SetChannelState, and a channel nobody is feeding can never finish it.
    // The blocking form
    // (dmode 1) is therefore correct only once the feed has been fenced off,
    // which is what close() does behind beginControlOperation(). Called with
    // the feed still live — and worse, from the feeding thread itself — it
    // waits out its entire 100 ms timeout, then force-clears the flags and
    // abandons the ramp, which is the click it exists to prevent. This takes
    // the non-blocking form and lets processIq() play the ramp out.
    //
    // While stopped, processIq() still runs (that is what advances the ramp)
    // and returns silence at the caller's normal cadence: once WDSP's exchange
    // bit clears, fexchange2 writes nothing at all, so this class zeroes the
    // output itself rather than letting the last block before the stop repeat.
    //
    // A START HAS NO CLOCKING PRECONDITION, and that took a vendored patch.
    // Upstream's SetChannelState case 1 arms the up-slew but never clears
    // slew.downflag, and the two flags are read independently on opposite sides
    // of fexchange2 — up gates the input, down gates the output. So a start
    // taken before the previous stop's ramp had been clocked out used to leave
    // that ramp pending on a channel WDSP considered running; the next few
    // blocks finished it, and downslew2's completion arm clears
    // ch[].exchange, after which fexchange2 returns having touched nothing at
    // all. The channel was silently dead, isRunning() said true, and only a
    // reconfigure() recovered it. AetherSDR patch 7 (see
    // third_party/wdsp/AETHERSDR-PATCHES.md) makes case 1 cancel a pending
    // down-ramp first.
    //
    // That fixed the ramp that is still PENDING and nothing else, and this
    // header said "safe at any spacing, including none" on the strength of it.
    // THAT WAS TRUE ONLY FOR THE REGIME WE HAD TESTED — restarts inside the
    // ramp — and false just outside it (K5PTB, review of #5628). A ramp that
    // has COMPLETED has already released Sem_Flush, and the flushChannel
    // thread sets exec_bypass whenever it next gets scheduled, which can be
    // after case 1 has cleared it: the worker is then bypassed, so the channel
    // produces nothing, or — in the blocking form — parks the host in
    // fexchange2 forever on a semaphore the bypassed worker will never
    // release. Measured on this tree, restarting with no gap at the spacing
    // where the ramp completes: 42 of 440 non-blocking trials dead, and 20 of
    // 20 blocking trials hung. Patch 8 waits that flush out before arming, and
    // both are 0.
    //
    // AND A STOP FOLLOWED BY CLOCKING USED TO MAKE THE NEXT CLOSE A
    // USE-AFTER-FREE, which is the other half of the same thread's story and
    // took a third vendored patch (ten9876, review of #5628). A completed ramp
    // makes flushChannel runnable; CloseChannel waited for the wdspmain worker
    // and for nothing else, so destroy_main() freed the RXA chain while
    // flushChannel was inside flush_rxa() on it. MEASURED: the shape stop ->
    // clock -> destroy crashed 30 of 30 trials, against clean 15 of 15 for both
    // stop -> destroy and never-stopped -> destroy; a 50 ms gap before the
    // destroy was clean 30 of 30, which is what identifies the flush thread.
    // AetherSDR patch 9 moves upstream's own flushChannel handshake out of
    // destroy_iobuffs() and into pre_main_destroy(), so it runs BEFORE
    // destroy_main() instead of after it. Pinned by
    // runCloseAfterStoppedClockingTest.
    //
    // SO, PLAINLY, WHAT IS SAFE. With patches 7, 8 and 9 together: a stop/start
    // pair is safe at any spacing including none, EXCEPT that the start may
    // block up to WDSP's 100 ms timeout waiting for the flush thread — in
    // practice under 3 ms, and 0 unless the previous stop's ramp was clocked
    // out; and a stopped channel may be clocked for as long as the caller likes
    // and then destroyed, with no ordering obligation on the caller.
    //
    // THAT LAST CLAUSE RESTS ON A PATCH, and it was false before it. Clocking a
    // stopped channel completes the down-ramp, which leaves WDSP's flushChannel
    // thread runnable; its flush_iobuffs() then drained the very token
    // pre_main_destroy() posts to wake the worker, the worker parked forever,
    // and destroy_iobuffs() closed that semaphore under a live waiter -- where
    // glibc's pthread_cond_destroy() blocks and never returns. ten9876 measured
    // it on #5628: 7 hangs in 16 runs under 8-way parallel load. Patch 4's wait
    // loop now re-posts the token on every iteration, which closes it. See
    // third_party/wdsp/AETHERSDR-PATCHES.md, patch 4.
    //
    // What is NOT claimed: none of this has run on hardware, the measurements
    // behind it are synthetic probes rather than a T/R edge, and the hang above
    // does not reproduce on macOS/arm64 at all -- 16 runs clean with the fix and
    // 16 clean without it -- so that platform cannot confirm the fix, only that
    // it causes no regression. Pinned by
    // runRestartDuringRampTest, whose scenarios straddle the ramp, and by
    // runCloseAfterStoppedClockingTest.
    //
    // Control-path work, guarded exactly like setMode(): returns false if a
    // control operation is already in flight, and must not be called from
    // processIq(). Setting the state it is already in is a no-op that succeeds.
    //
    // [[nodiscard]] because beginControlOperation() REFUSES rather than waits:
    // a caller that ignores false has not stopped the channel and will silently
    // pay close()'s 100 ms instead. Every owner in this tree calls this from the
    // thread that drives processIq(), where a callback cannot be in flight, so
    // today it cannot fail — the attribute is there to make it a compile error
    // rather than a mystery if that ownership ever moves off that thread.
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
    // Runtime RX AGC change. agcMode is the WDSP RXA AGC mode (0 off, 1 long,
    // 2 slow, 3 medium, 4 fast); maximumGainDb is the AGC "top", the ceiling on
    // how much gain the AGC may apply. Receive channels only — returns false on
    // a transmit channel, on a non-finite ceiling, or if a control operation is
    // already in flight. Control-path work, guarded exactly like setMode(); it
    // must not be called from the processIq() callback.
    bool setAgc(int agcMode, double maximumGainDb) noexcept;

    // ── Filter length and phase mode, at runtime ──────────────────────────
    //
    // Until these existed, Config::filterTaps and Config::minimumPhase could
    // only be chosen in open(), and the only way to revisit either was
    // reconfigure() -- which closes and reopens the channel and so DESTROYS the
    // notch database. That is the wrong tool for both: the notch database is
    // exactly what a caller changing the filter length is usually trying to
    // keep.
    //
    // setFilterTaps() is what makes the length/selectivity trade a runtime
    // choice rather than a connect-time one. The floor on notch width is a
    // function of the length -- 200 Hz at 2048, 50 Hz at 8192, see
    // minimumNotchWidthHz() -- so a caller that wants a narrow notch can buy
    // the taps when the operator asks for one and give them back afterwards,
    // instead of every receiver paying the group delay of the longest filter
    // anyone might want. The group delay is (taps-1)/2 samples at the DSP
    // rate: 4095.5 samples (85.3 ms at 48 kHz) at 8192, 1023.5 (21.3 ms) at
    // 2048.
    //
    // THE NOTCH DATABASE SURVIVES. RXASetNC reaches nbp0 through
    // RXANBPSetNC -> setNc_nbp -> calc_nbp_impulse, which rebuilds the mask
    // FROM the notch database rather than replacing it, so notches placed
    // before the call are still placed after it -- at the new width floor.
    // This is the whole difference between this call and reconfigure().
    //
    // THE SHIFT SURVIVES TOO, and needs no replay. calc_nbp_impulse() reads the
    // shift from the notch DATABASE (`b->shift`, folded into the passband
    // offset as `b->tunefreq + b->shift`), and setNc_nbp() changes only the tap
    // count before rebuilding from that same database. Nothing on the RXASetNC
    // path writes the shift stage or the database's copy of it, so re-asserting
    // either after this call would be ceremony.
    //
    // COST, AND IT IS NOT FREE. setFilterTaps() stops the channel, re-plans
    // six FIR cores under the process-global FFTW planner lock, and starts it
    // again. MEASURED on this tree (macOS arm64, RelWithDebInfo, 48 kHz):
    // 1.8-28.8 ms held on that lock per call. It is acceptable at a moment
    // the operator initiated and is not acceptable per block, and a caller must
    // not poll it.
    //
    // The stop is taken HERE, outside the lock, rather than left to the one
    // inside RXASetNC -- see the block comment on the implementation. Left to
    // RXASetNC it is a dmode-1 stop whose drain can never complete while the
    // control fence is up, so it spins out its full 100-count Sleep(1) timeout
    // WITH THE PLANNER LOCK HELD. That is 100 ms only where Sleep(1) costs
    // 1 ms; the port routes it to nanosleep(), which on this machine lands
    // nearer 1.6-2.3 ms, and the call measured 155-227 ms. Hl2Spectrum's
    // constructor and destructor take the same lock on the EP2-pacing I/O
    // thread (#5424), so this is not merely someone else's latency.
    //
    // WHAT THE OPERATOR ACTUALLY HEARS is the up-slew and a filter refill, NOT
    // a mute ramp down. The down-ramp is armed and then cancelled unclocked by
    // flush_slews() on the restore (AetherSDR patch 7). An earlier draft of
    // this comment claimed the mute ramp plays; it does not, and it did not
    // before this change either -- RXASetNC's timeout path force-cleared
    // downflag just the same.
    //
    // `taps` MUST BE A POWER OF TWO IN [256, 16384] AND AN EXACT MULTIPLE OF
    // dspBlockSize. nc >= size is only half of WDSP's requirement: fircore
    // partitions into nfor = nc / size and walks the ring with nfor - 1 as a
    // POWER-OF-TWO MASK, so an nfor that is not a power of two silently skips
    // partitions and a non-multiple silently truncates the impulse -- wrong
    // audio, no error. firmin.h says it outright: `int nc; // number of filter
    // coefficients, power of two, >= size`. The same predicate now guards
    // Config::filterTaps in validateConfig(), so create() and reconfigure()
    // cannot reach what this refuses.
    //
    // THREADING, FOR WHOEVER WRITES THE GATE. These setters make
    // Config::filterTaps MUTABLE ON AN OPEN CHANNEL, and minimumNotchWidthHz()
    // reads it without taking anything. There is no cross-thread reader today
    // -- every caller of both is on the control thread -- so this is sound as
    // it stands. It stops being sound the moment the gate publishes a tap
    // count that another thread reads: whoever adds that must either make the
    // field atomic or take the lock in the getter. Recording it here so it is
    // not rediscovered from a torn read.
    //
    // setMinimumPhase() trades linear phase for latency: the same length of
    // filter, its energy front-loaded, so the group delay collapses while the
    // magnitude response (and therefore the notch depth and the width floor)
    // is preserved. Unlike RXASetNC it does NOT stop the channel -- RXASetMP
    // only re-runs the mask through the cepstral conversion -- but it does
    // rebuild all six masks, so it is control-path work for the same reason.
    //
    // Both are receive-only: RXASetNC and RXASetMP have no transmit
    // counterpart and a TX channel has none of the six cores they address.
    // Control-path work, guarded exactly like setMode(); neither may be called
    // from the processIq() callback. Both are idempotent at the WDSP level --
    // RXANBPSetNC and RXANBPSetMP compare against the stored value first -- but
    // RXASetNC still pays the stop/restart, so a caller should not poll them.
    bool setFilterTaps(int taps) noexcept;
    bool setMinimumPhase(bool on) noexcept;

    // Runtime FM detector deviation, in Hz. Receive channels only.
    //
    // Applies in every mode but is only AUDIBLE in FM: RXA builds one fmd stage
    // per channel and runs it only when the mode selects it, so this is
    // accepted and stored on an SSB channel and takes effect if and when the
    // mode becomes FM. That is deliberate — refusing by mode would make the
    // value depend on the order the caller sets mode and deviation in.
    //
    // NOT WBFM. Broadcast FM is a different demodulator, not this one with a
    // wider number: SetRXAMode clears setFMDRun() for every mode and its
    // case RXA_WBFM raises wbfm.p->run WITHOUT putting fmd back, so xfmd's
    // if (a->run) never fires and the value this writes is inert. wbfm.c runs
    // an atan2 quadrature discriminator whose scale is fixed at construction
    // (disc_gain_comp = rate / (TWOPI * 75000.0)), create_wbfm() takes no
    // deviation argument, and WDSP exposes no SetRXAWBFMDeviation to reach it.
    //
    // Returns false on a transmit channel (SetRXAFMDeviation has no TX
    // counterpart; TX deviation is SetTXAFMDeviation on a different stage), on
    // a value outside Config::kMinFmDeviationHz..kMaxFmDeviationHz — WDSP
    // divides by it, so 0 and anything near it drive the audio gain to
    // infinity — or if a control operation is already in flight.
    // Control-path work, guarded exactly like setMode(); it must not be called
    // from the processIq() callback.
    bool setFmDeviation(double deviationHz) noexcept;

    // ── Impulse noise blankers ────────────────────────────────────────────
    //
    // WDSP's ANB (nob.c) and NOB (nobII.c), run on the RAW IQ ahead of the
    // channel. They have to be ahead: an impulse is short in time and wide in
    // frequency, so once the bandpass has smeared it across milliseconds there
    // is no longer a spike to blank. That is also why neither can be an RXA
    // stage — WDSP does not put one there, and every reference client calls
    // them outside the channel too.
    //
    // AT MOST ONE RUNS. They are not complementary stages to be cascaded: both
    // watch the same detector and blank the same window, so running both would
    // have the second one reconstructing what the first one just zeroed. Every
    // reference client treats this as one 3-way choice for the same reason.
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
    // processIq(). Turning a blanker ON flushes it first, so it starts from a
    // known state rather than from whatever the last enabled period left.
    //
    // `fill` is pushed whatever the kind, because it is Advanced's parameter
    // and not its switch: a later switch to Advanced must not need the caller
    // to remember to re-send it.
    bool setNoiseBlanker(NoiseBlanker kind, int level, NoiseBlankerFill fill) noexcept;
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
    // Kept as a bool question because that is what most callers ask — a meter,
    // a readback or a test that wants to know whether anything is blanking.
    [[nodiscard]] bool noiseBlankerEnabled() const noexcept
    {
        return m_config.noiseBlanker != NoiseBlanker::Off;
    }
    [[nodiscard]] NoiseBlanker noiseBlankerKind() const noexcept
    {
        return m_config.noiseBlanker;
    }
    [[nodiscard]] NoiseBlankerFill noiseBlankerFill() const noexcept
    {
        return m_config.noiseBlankerFill;
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

    // Shared FFTW-planner serialization guard. FORWARDS to
    // AetherSDR::fftwPlannerLock() (core/dsp/FftwPlannerLock.h), which owns
    // the mutex; this spelling is kept only so existing call sites and tests
    // need no churn. NEW CODE OUTSIDE THIS CLASS SHOULD TAKE
    // fftwPlannerLock() DIRECTLY -- the lock is a property of FFTW, not of a
    // WDSP channel, and reaching it through this header is the dependency
    // edge that kept SpectralNR on a mutex of its own.
    //
    // THE CENSUS THIS COMMENT USED TO CARRY WAS THE BUG. It said "today:
    // AnanSpectrum", which was already wrong when #5424 added Hl2Spectrum and
    // stayed wrong while SpectralNR guarded the same planner with a private
    // static (#5895). A list that has to be edited by hand goes stale
    // silently, so the authoritative one now lives next to the lock in
    // FftwPlannerLock.h and this comment deliberately keeps none.
    //
    // It also said "Held only around the planner call itself, not the whole
    // construction." That is FALSE and was already false: Hl2Spectrum holds
    // it across its fftw_malloc/fftw_free too, deliberately, because the
    // frames TSan named in #5424 are memalign and free rather than the
    // planner. Width is per call site and the allocations belong inside.
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
    static std::size_t computeOutputBlockSize(const Config& config) noexcept;

    void open() noexcept;
    void close() noexcept;
    // Create/destroy BOTH blanker stages alongside the channel. Each blanker's
    // id IS the channel id: nob.c and nobII.c each keep their own table of 32,
    // WDSP's channel table is also 32, and this class already owns the
    // allocation and release of that number — so borrowing it gives the objects
    // one lifetime instead of three that can fall out of step. Nothing else in
    // this process may create an ANB or a NOB without going through here.
    //
    // Both are created even though at most one runs, and even when the operator
    // has no blanker on at all: create_nobEXT allocates, and a channel that
    // built its stages lazily would be allocating on a live real-time path the
    // first time someone clicked NB2.
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
    // WDSP's channel state, mirrored. Atomic because processIq() consults it on
    // the real-time path to decide whether it owns the output buffer this
    // block, the same way it consults m_nbActive — the control handshake
    // already orders the write, this keeps the read from being a data race.
    std::atomic<bool> m_running {false};

    // ── Noise blanker state ───────────────────────────────────────────────
    //
    // m_nbOpen tracks the WDSP-side stages (created for the life of a receive
    // channel whether or not either is running); m_nbActive is what processIq()
    // reads to decide whether to pay for the interleave at all, and m_nbKind
    // which of the two to feed. Separate from m_config, because the stages exist
    // while switched off and the real-time path must not consult m_config across
    // a control-thread write.
    //
    // TWO atomics rather than one enum atomic the real-time path compares
    // against Off: the gate is read every block and the kind only inside it, and
    // keeping the gate a bool leaves the hot check exactly what it was before
    // NB2 existed.
    bool m_nbOpen = false;
    std::atomic<bool> m_nbActive {false};
    std::atomic<NoiseBlanker> m_nbKind {NoiseBlanker::Off};
    // Set on a transmit edge; processIq() skips the stage while it is true so
    // the running average is FROZEN rather than dragged into the mute path's
    // silence. No companion "flush on release" flag: re-arming the stage on
    // every T->R edge is the defect this hold exists to avoid, not part of it.
    std::atomic<bool> m_nbHold {false};
    // Both blankers work on WDSP's interleaved-double buffer, in place; processIq()
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
// Same reason, for the same reason it matters: both backends marshal the blanker
// verb onto the thread that owns the DSP object, so an unregistered enum here
// would silently drop every NB and NB2 click instead of failing.
Q_DECLARE_METATYPE(WdspChannel::NoiseBlanker)
Q_DECLARE_METATYPE(WdspChannel::NoiseBlankerFill)
