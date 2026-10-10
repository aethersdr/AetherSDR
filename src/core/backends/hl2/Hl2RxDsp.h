#pragma once

#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

#include "core/backends/hl2/Hl2AdcPairing.h"
#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"
#include "core/dsp/WdspProcessTally.h"

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
    // HL2 IQ rate and the audio rate — see the note in buildChannel().
    static constexpr int kWdspDspSampleRateHz = 48000;

    // RX filter length. This is also the manual-notch resolution: WDSP's
    // narrowest notch is 1600 / (taps/256) Hz at kWdspDspSampleRateHz, and it
    // WIDENS a narrower request instead of rejecting it. 8192 taps buys a 50 Hz
    // floor (pihpsdr's value); WDSP's own default of 2048 would floor it at
    // 200 Hz, wide enough to swallow a CW signal next to the carrier being
    // notched. Keep kMinNotchWidthHz in step if this changes — the UI offers
    // widths from it.
    // This is the LONG length (rxFilterTapsFor): whenever a notch exists it is
    // in force, so the advertised notch floor derives from it.
    static constexpr int kRxFilterTaps = 8192;
    static constexpr double kMinNotchWidthHz =
        1600.0 / (static_cast<double>(kRxFilterTaps) / 256.0)
        * (static_cast<double>(kWdspDspSampleRateHz) / 48000.0);
    static_assert(kMinNotchWidthHz <= 50.0,
                  "RX filter taps no longer allow a 50 Hz notch; the width "
                  "presets in the TNF menu assume one.");

    // RX filter phase per mode (#5498): minimum phase preserves the notch
    // floor and cuts post-unmute return from 128 to 44 ms (AGC off). CW stays
    // linear by the ruling linked in the WDSP patch ledger. Minimum
    // phase costs ~14 ms per filter edit on hl2-io; patch 14 frees its design
    // scratch. See third_party/wdsp/AETHERSDR-PATCHES.md for the measurements.
    // CW's latency is cut by length instead: rxFilterTapsFor().
    [[nodiscard]] static constexpr bool rxMinimumPhaseFor(WdspChannel::Mode mode) noexcept
    {
        return mode != WdspChannel::Mode::Cwl && mode != WdspChannel::Mode::Cwu;
    }

    // RXA panel gain, applied after the AGC: unity removes WDSP's x4, which put
    // every HL2 mode 12.04 dB past the AGC's level (#5942). Bandpass, speak and
    // mpeak still run between the AGC and the panel, so this bounds no peak.
    static constexpr double kRxPanelGain = 1.0;

    // RX filter length (#5578). Outside CW minimum phase already removed the
    // delay, so length buys nothing there. In CW the length is the latency, so
    // it runs kRxShortFilterTaps unless a notch needs the long filter's floor or
    // the passband is under kRxShortTapsMinWidthHz; 2048 would leave a -33 dB
    // skirt 50 Hz out (hl2_rxdsp_adaptive_taps_test measures each length).
    // Shortening needs kRxTapsHysteresisHz of margin; currentTaps <= 0 = none.
    static constexpr int kRxShortFilterTaps = 4096;
    static constexpr double kRxShortTapsMinWidthHz = 100.0;
    static constexpr double kRxTapsHysteresisHz = 20.0;
    [[nodiscard]] static constexpr int rxFilterTapsFor(WdspChannel::Mode mode, double lowHz,
                                                       double highHz, int notchCount,
                                                       int currentTaps = 0) noexcept
    {
        if (rxMinimumPhaseFor(mode) || notchCount > 0)
            return kRxFilterTaps;
        const double width = highHz >= lowHz ? highHz - lowHz : lowHz - highHz;
        const bool shortening = currentTaps <= 0 || currentTaps > kRxShortFilterTaps;
        const double needed = kRxShortTapsMinWidthHz
                              + (shortening && currentTaps > 0 ? kRxTapsHysteresisHz : 0.0);
        return width >= needed ? kRxShortFilterTaps : kRxFilterTaps;
    }

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
        // waits for each output block. That does not make a burst feed
        // reproducible: flush_iobuffs() drains with a 1 ms timed wait, so the
        // r1/r2 phase is fixed per channel open, not per run (#5629).
        bool blockForOutput = false;
    };

    // (Re)build the WdspChannel + Hl2Spectrum for this config, SYNCHRONOUSLY on
    // this object's own thread. Returns false (and sets error, if given) when the
    // WDSP channel cannot be created.
    //
    // Connect uses this before streaming starts. Live rate changes and added
    // receivers use beginRebuild/buildChannel/installRebuiltChannel so WDSP
    // setup does not hold the I/O pacer (docs/HERMES.md §22.4).
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);

    // Asynchronous rebuild: build off-thread, swap on-thread. MetisClient's EP2
    // pacing, EP6 drain and every Hl2RxDsp share one I/O thread, so a build on
    // it would stall every receiver and stop EP2, which the gateware watchdog
    // answers by halting the stream (docs/HERMES.md §20.8). Same trio as
    // AnanRxDsp: build first, disturb the session second.

    // What buildChannel() produced. Move-only (owns two unique_ptrs) and free
    // of any reference to the Hl2RxDsp it will be installed into, which is what
    // makes it safe to carry between threads.
    struct RebuildResult {
        std::unique_ptr<WdspChannel> channel;
        std::unique_ptr<Hl2Spectrum> spectrum;
        std::size_t outputBlockSize = 0;
        // The Config this was actually built for. installRebuiltChannel() takes
        // the GEOMETRY from here (rate, block size, FFT size) because that is
        // what the new channel physically is, and leaves the operator-facing
        // fields to m_config, which may have moved while the build ran.
        Config built;
        // The noise-blanker request the channel was OPENED with, so
        // installRebuiltChannel() can tell whether the operator moved it
        // mid-build and needs a live push after the swap.
        WdspChannel::NoiseBlanker builtNbKind = WdspChannel::NoiseBlanker::Off;
        int builtNbLevel = 50;
        WdspChannel::NoiseBlankerFill builtNbFill =
            WdspChannel::NoiseBlankerFill::Zero;
        std::string error;   // set iff channel == nullptr
    };

    // The slow half of configure() (OpenChannel, FFTW planning) with nothing of
    // `this` in it, so it may run on another thread while the installed channel
    // keeps producing audio. The blanker triple is passed in because the channel
    // is opened with it; the caller snapshots it on this object's thread inside
    // beginRebuild()'s turn.
    [[nodiscard]] static RebuildResult buildChannel(
        const Config& config, WdspChannel::NoiseBlanker noiseBlanker,
        int noiseBlankerLevel, WdspChannel::NoiseBlankerFill noiseBlankerFill);

    // Marks a rebuild in flight and seeds the operator-facing half of m_config
    // from the build's snapshot. Call on this object's thread BEFORE the build.
    // While a rebuild is in flight, control verbs update mirrors only: pushing
    // to WDSP would block this I/O thread on WdspChannel.cpp's process-wide
    // g_setupMutex, which the build holds. installRebuiltChannel() re-applies
    // them. Geometry fields (rates, block and FFT size) are left alone because
    // they describe the still-running channel until the swap. Counted, not a
    // flag: two rebuilds can be outstanding at once.
    Q_INVOKABLE void beginRebuild(const Config& config);

    // Give up on a rebuild that will never be installed — it failed, or a newer
    // one superseded it. Balances beginRebuild(); touches nothing else, so the
    // running channel and every mirror are exactly as they were.
    Q_INVOKABLE void abandonRebuild();

    // Swap a built RebuildResult in as the active channel, on this object's
    // thread. Not Q_INVOKABLE: RebuildResult is move-only and moc's dispatch
    // copy-constructs by-value arguments. Returns false, leaving the current
    // channel untouched, if result.channel is null. Always balances
    // beginRebuild().
    bool installRebuiltChannel(RebuildResult result);

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
    // Cap panadapter frames per second; fps <= 0 removes the cap. The FFT is
    // skipped when a frame is not due: the natural rate is IQ rate / FFT size
    // (47 fps at 48 kHz, 375 fps at 384 kHz), so limiting here rather than
    // downstream saves the FFT work. Wall-clock based, so it holds across a
    // sample-rate change.
    Q_INVOKABLE void setSpectrumRateFps(int fps);

    // Panadapter averaging: the operator's FFT AVG as a time constant in ms
    // (0 = none) and the weighted toggle as the averaging domain (true =
    // log-recursive, false = power); see Hl2Spectrum. Held here, not in Config,
    // and re-applied in installChannel(): every zoom builds a fresh Hl2Spectrum.
    Q_INVOKABLE void setSpectrumAverageMs(int ms);
    Q_INVOKABLE void setSpectrumLogAverage(bool on);
    // The NCO moved: forget the running average and the held partial window, so
    // old-axis IQ does not ghost across the new axis. Not a transport gap, which
    // keeps the average (see Hl2Spectrum::reset()).
    Q_INVOKABLE void dropSpectrumAverage();
    // What the installed spectrum is actually running, for tests. DSP thread.
    [[nodiscard]] double spectrumAverageMsApplied() const noexcept
    {
        return m_spectrum ? m_spectrum->averageTimeMs() : -1.0;
    }
    [[nodiscard]] bool spectrumLogAverageApplied() const noexcept
    {
        return m_spectrum && m_spectrum->logAverage();
    }

    // Impulse noise blankers, the HL2's only ones (no firmware DSP), so NB is
    // shown even with hasRadioSideDsp = false; at most one of the two runs. Runs
    // in WdspChannel::processIq() on the wire samples ahead of fexchange2, before
    // the bandpass smears the impulse. Audio path only: the panadapter shows
    // unblanked IQ (a Flex blanks both). `level` 0..100, larger is more
    // aggressive. Held outside Config because configure() replaces m_config; it
    // is re-applied after every rebuild.
    Q_INVOKABLE void setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                     WdspChannel::NoiseBlankerFill fill);
    // Receive squelch; `level` is the slice model's 0..100. WdspChannel owns the
    // per-mode routing and re-applies it on setMode(). Held outside Config, like
    // the blanker, and re-applied by installChannel(). A change the channel
    // refuses (a control operation in flight) is marked pending and retried at
    // the top of each processIqBlock() until it is taken; see squelchPending().
    // `levelOffsetDb` refers the level map to the LNA:
    // Hl2DbReference::levelSquelchOffsetDb().
    Q_INVOKABLE void setSquelch(bool on, int level, double levelOffsetDb);
    [[nodiscard]] bool squelchPending() const noexcept { return m_squelchPending; }
    [[nodiscard]] bool squelchEnabled() const noexcept { return m_squelchOn; }
    [[nodiscard]] int squelchLevel() const noexcept { return m_squelchLevel; }
    [[nodiscard]] double squelchLevelOffsetDb() const noexcept { return m_squelchOffsetDb; }
    // What the channel last WROTE to WDSP (stage, run flags, threshold), or
    // nullopt before configure(). Forwarded, not mirrored, for the reason
    // channelConfig() gives below. The record itself is a by-value snapshot
    // safe from any thread; m_channel is not, so call this on this object's
    // thread.
    [[nodiscard]] std::optional<WdspChannel::AppliedSquelch> appliedSquelch() const
    {
        if (!m_channel)
            return std::nullopt;
        return m_channel->appliedSquelch();
    }

    // What the operator ASKED for. Survives configure() and is what a rebuild
    // re-applies.
    [[nodiscard]] bool noiseBlankerEnabled() const
    {
        return m_nbKind != WdspChannel::NoiseBlanker::Off;
    }
    [[nodiscard]] WdspChannel::NoiseBlanker noiseBlankerKind() const { return m_nbKind; }
    [[nodiscard]] int noiseBlankerLevel() const { return m_nbLevel; }
    [[nodiscard]] WdspChannel::NoiseBlankerFill noiseBlankerFill() const
    {
        return m_nbFill;
    }
    // What the WDSP stage actually applied: the request crosses a queued
    // connection and WdspChannel can refuse it. Atomic (relaxed) because
    // Hl2Backend reads these from the GUI thread for `hl2 nb.get`.
    [[nodiscard]] bool appliedNoiseBlankerEnabled() const
    {
        return appliedNoiseBlankerKind() != WdspChannel::NoiseBlanker::Off;
    }
    [[nodiscard]] WdspChannel::NoiseBlanker appliedNoiseBlankerKind() const
    {
        return m_nbAppliedKind.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int appliedNoiseBlankerLevel() const
    {
        return m_nbAppliedLevel.load(std::memory_order_relaxed);
    }
    [[nodiscard]] WdspChannel::NoiseBlankerFill appliedNoiseBlankerFill() const
    {
        return m_nbAppliedFill.load(std::memory_order_relaxed);
    }

    // AGC-off level and CW APF: held outside Config (configure() replaces it)
    // and re-applied by installChannel(), pushed only when canPushToChannel().
    //
    // AGC-off level 0..100 -> WDSP fixed gain: 10 + 0.6 * (level - 10) dB, so
    // the default level 10 is exactly WDSP's 10 dB channel default, and the
    // slope matches the AGC-T threshold's 0.6 dB/unit. Not referred to the LNA
    // gain: with AGC off the operator rides RF gain against overload.
    static constexpr double kAgcFixedGainDbPerUnit = 0.6;
    static constexpr int kDefaultAgcOffLevel = 10;           // SliceModel::m_agcOffLevel
    static constexpr double kDefaultAgcFixedGainDb = 10.0;   // WdspChannel::Config
    [[nodiscard]] static double agcFixedGainDbForOffLevel(int level) noexcept
    {
        return kDefaultAgcFixedGainDb
               + static_cast<double>(std::clamp(level, 0, 100) - kDefaultAgcOffLevel)
                     * kAgcFixedGainDbPerUnit;
    }
    // WDSP applies the fixed gain only in AGC mode 0 (wcpAGC.c xwcpagc), so
    // this is safe in any mode and audible exactly when AGC is Off.
    Q_INVOKABLE void setAgcOffLevel(int level);
    [[nodiscard]] int agcOffLevel() const noexcept { return m_agcOffLevel; }

    // APF level 0..100 -> the peak's bandwidth (the UI labels it "APF
    // bandwidth", higher = narrower): 200 Hz at 0 halving every 50 units, so
    // the default 50 is RXA.c's own 100 Hz. Gain stays at RXA.c's linear 2.0,
    // so the slider changes selectivity, not loudness.
    static constexpr double kApfWidestBandwidthHz = 200.0;
    static constexpr double kApfGain = 2.0;          // RXA.c create_apfshadow
    static constexpr int kDefaultApfLevel = 50;      // SliceModel::m_apfLevel
    [[nodiscard]] static double apfBandwidthHzForLevel(int level) noexcept
    {
        const double units = static_cast<double>(std::clamp(level, 0, 100));
        return kApfWidestBandwidthHz * std::pow(2.0, -units / 50.0);
    }
    [[nodiscard]] static constexpr bool isCwMode(WdspChannel::Mode mode) noexcept
    {
        return mode == WdspChannel::Mode::Cwl || mode == WdspChannel::Mode::Cwu;
    }
    // centerHz is the CW pitch in audio Hz (the backend owns it). The stage
    // runs only in CWL/CWU; the request is held through other modes and
    // setMode() re-evaluates it.
    Q_INVOKABLE void setApf(bool on, int level, double centerHz);
    [[nodiscard]] bool apfRequested() const noexcept { return m_apfOn; }
    [[nodiscard]] int apfLevel() const noexcept { return m_apfLevel; }
    [[nodiscard]] double apfCenterHz() const noexcept { return m_apfCenterHz; }
    [[nodiscard]] bool apfInCircuit() const noexcept
    {
        return m_apfOn && isCwMode(m_config.mode);
    }

    // Post-DDC half of the ADC pairing (docs/HERMES.md §13 item 16;
    // Hl2AdcPairing.h). WDSP's RXA_ADC_PK in dB re wire full scale, not
    // calibrated to the antenna. adcmeter runs first in xrxa, after the shift
    // and input resampler, so it measures this one slice at
    // kWdspDspSampleRateHz before filtering, demod or AGC. Sampled on the DSP
    // thread after each processed block; atomic (relaxed) for the GUI-thread
    // healthSnapshot(). nullopt until a block has been processed.
    [[nodiscard]] std::optional<double> adcPeakDbfs() const
    {
        const float v = m_adcPeakDbfs.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        return static_cast<double>(v);
    }
    // Raw steady_clock stamp of the adcPeakDbfs() reading, for
    // SliceSamplingGate, which must compare it against its resume request.
    // Stored only on the unmuted path of processIqBlock(), so a stamp later than
    // the request proves the unmute was applied. 0 = never processed.
    [[nodiscard]] std::int64_t adcPeakObservedAtNs() const noexcept
    {
        return m_adcPeakAtNs.load(std::memory_order_relaxed);
    }
    // Age of the adcPeakDbfs() reading; it stops advancing when IQ stops or the
    // chain is muted for TX. Feeds Hl2AdcPairing.h's freshness input
    // (kSliceStaleMs) so a held slice peak isn't paired with a live flag.
    [[nodiscard]] std::optional<std::int64_t> adcPeakObservedAgoMs() const
    {
        const std::int64_t at = m_adcPeakAtNs.load(std::memory_order_relaxed);
        return slicePeakAgeMs(at, steadyNowNs());
    }

    // Every processIq() outcome, counted since construction (WdspProcessTally.h).
    // `Underrun` is separate from the faults: it is normal while the async output
    // side fills. Monotonic across rebuilds, since a rebuild is when a fault is
    // most likely. Safe to call from the GUI thread.
    [[nodiscard]] WdspProcessTally::Counts processTally() const noexcept
    {
        return m_processTally.snapshot();
    }

    // Partial FFT windows discarded at a transport discontinuity (including
    // accepted rewinds and duplicates; empty windows don't count). Monotonic for
    // this object's lifetime, across configure(). Independent of droppedPackets.
    // Written on the I/O thread, read relaxed on the GUI thread.
    [[nodiscard]] quint64 spectrumGapDiscards() const noexcept
    {
        return m_spectrumGapDiscards.load(std::memory_order_relaxed);
    }

    // Manual notches. `index` is WDSP's positional handle; Hl2Backend maps stable
    // ids onto it. Centres are absolute RF Hz, so setNotchTuneFrequency() must
    // follow the NCO. The set is mirrored here because a rebuild destroys WDSP's
    // notch database.
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
    // The RX filter length and notch-width floor the live channel is running
    // (0 without one). Read back from the channel, not from the policy, so a
    // test of the wiring cannot agree with itself.
    [[nodiscard]] int rxFilterTapsInForce() const;
    [[nodiscard]] double minimumNotchWidthInForceHz() const;
    // TEST ONLY: treat every filter-length change as refused, as setFilterTaps()
    // refuses one that loses beginControlOperation(). That race cannot be built
    // on one thread, and refusing at WdspChannel would refuse the notch too.
    void setRefuseFilterTapsChangesForTest(bool on) noexcept { m_refuseFilterTapsForTest = on; }

    // Mute the demodulator while transmitting. The spectrum keeps running on
    // real IQ, but the audio channel is clocked with silence so WDSP's buffers
    // never fill with our own transmission and drain it to the speakers at
    // unkey.
    Q_INVOKABLE void setAudioMuted(bool muted);
    // The applied mute state, not the request: Hl2Backend posts setAudioMuted()
    // across threads, so its own m_rxAudioMuted is only a request (#5497). Read
    // on the DSP thread, where setAudioMuted() runs.
    [[nodiscard]] bool isAudioMuted() const noexcept { return m_audioMuted; }
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

    // What the WDSP channel was actually OPENED WITH, for the read-back verb.
    //
    // Forwarded from WdspChannel rather than mirrored here, for the same reason
    // appliedNoiseBlankerEnabled() reads the applied value: a read-back that
    // returned this class's own copy of the request would be certifying its own
    // input. Null when no channel exists, which the caller must report as
    // "not configured" rather than as zeros.
    [[nodiscard]] const WdspChannel::Config* channelConfig() const noexcept
    {
        return m_channel ? &m_channel->config() : nullptr;
    }
    // Test only: forwards WdspChannel::refuseControlOperationsForTest() to the
    // current channel, so the squelch retry path can be driven offline.
    void refuseChannelControlForTest(unsigned count) noexcept
    {
        if (m_channel)
            m_channel->refuseControlOperationsForTest(count);
    }
    [[nodiscard]] std::size_t channelOutputBlockSize() const noexcept
    {
        return m_channel ? m_channel->outputBlockSize() : 0;
    }

    // The WDSP channel id this chain was actually given, or -1 before configure().
    //
    // Ids come from a PROCESS-WIDE pool of 32 shared with the transmit chain and
    // every other backend, so this is whatever was free — NOT the receiver's own
    // index, and not stable across a reconnect. Hl2Backend records it in the
    // index-space map (Hl2Receivers.h) precisely so nothing has to derive it.
    [[nodiscard]] int wdspChannelId() const noexcept
    {
        return m_channel ? m_channel->channelId() : -1;
    }

    // Demodulated-audio DC blocker, one pole per channel. WDSP's AM/SAM
    // detector (amd.c) emits sqrt(I^2 + Q^2), so the carrier arrives as a DC
    // pedestal; `levelfade` holds it rather than removing it, and the symmetric
    // AM passband puts 0 Hz mid-band. It runs on WdspChannel's output, so wcpAGC
    // inside RXA still rides the pedestal. Applied to every mode (a no-op on
    // zero-mean audio). A future WDSP RX consumer should move this into
    // WdspChannel. Public for hl2_am_dcblock_test.
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

    // An EP6 sequence gap precedes the next block. Called by DirectConnection on
    // this object's I/O thread. Discards the partial panadapter frame so no FFT
    // spans the discontinuity (see Hl2Spectrum::reset()). The audio path is left
    // alone: dropping buffered samples would lengthen the hole, and resetting
    // WDSP would restart filter and AGC state on one lost datagram.
    void onSequenceGap();

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    void meterUpdate(float dbfs);                           // audio-RMS S-meter

private:
    // Pushes rxMinimumPhaseFor(m_config.mode) to the live channel. Only
    // called where the control verbs may reach it (setMode, installChannel).
    void applyMinimumPhaseForMode();
    // Pushes rxFilterTapsFor(mode, passband, notchCount) to the live channel;
    // notchCount is passed so addNotch() can raise for the notch it is about to
    // add. hysteresisFromTaps (> 0) is the length the hysteresis is measured
    // from, for a fresh channel replacing one; 0 means the length in force.
    // Returns whether the wanted length is in force afterwards.
    bool applyFilterTaps(int notchCount, int hysteresisFromTaps = 0);
    // One attempt to put the squelch request on the channel; marks it pending
    // on refusal. Caller has checked canPushToChannel().
    void pushSquelchToChannel();
    // Push the held APF request / AGC-off level at the live channel. Callers
    // check canPushToChannel() first. Refusals are logged and the request is
    // kept, so the next install re-applies it.
    void applyApf();
    void applyAgcOffLevel();
    // True when the next panadapter frame may be computed. Stays true until one
    // actually completes, since a frame spans several EP6 blocks.
    bool spectrumFrameDue();

    // The shared install step: resize the scratch buffers, recompute the DC
    // blocker, re-apply everything Config does not carry (shift, the notch set,
    // the noise blanker, the blanker hold, the squelch, the AGC-off level, the
    // APF) and take ownership of the new channel/spectrum. configure() and
    // installRebuiltChannel() both end here so a second copy of that list
    // cannot drift and lose one of them.
    void installChannel(RebuildResult result);
    // Arm m_meterTap from the current geometry. One site for the arithmetic,
    // called on the mute's release edge and on a channel install so the two
    // cannot drift apart. DSP thread only.
    void armMeterSettle();

    // May a control verb push at m_channel now? False while a rebuild is
    // outstanding (see beginRebuild()). True inside installChannel() even then,
    // because the install's re-application goes through these same verbs.
    [[nodiscard]] bool canPushToChannel() const noexcept
    {
        return m_channel && (m_installing || m_rebuildsInFlight == 0);
    }
    int m_rebuildsInFlight = 0;
    bool m_installing = false;

    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<Hl2Spectrum> m_spectrum;
    double m_shiftHz = 0.0;   // current slice offset from the NCO, Hz
    // The operator's panadapter averaging; see setSpectrumAverageMs().
    int m_spectrumAverageMs = 0;
    bool m_spectrumLogAverage = false;
    // Noise-blanker state, kept out of m_config so configure() cannot clear it.
    // m_nbKind/m_nbLevel/m_nbFill are the REQUEST; m_nbApplied* are what the
    // WDSP stage took. They diverge exactly when something went wrong, which is
    // the whole reason the bridge readback reports the applied set.
    WdspChannel::NoiseBlanker m_nbKind = WdspChannel::NoiseBlanker::Off;
    int  m_nbLevel = 50;      // 0..100, the slice model's units
    // Squelch request — see setSquelch(). Defaults mirror SliceModel's.
    bool m_squelchOn = false;
    int  m_squelchLevel = 20;
    double m_squelchOffsetDb = 0.0;
    // True while the channel has refused the current request; see setSquelch().
    bool m_squelchPending = false;
    WdspChannel::NoiseBlankerFill m_nbFill = WdspChannel::NoiseBlankerFill::Zero;
    std::atomic<WdspChannel::NoiseBlanker> m_nbAppliedKind
        {WdspChannel::NoiseBlanker::Off};
    std::atomic<int>  m_nbAppliedLevel {50};
    std::atomic<WdspChannel::NoiseBlankerFill> m_nbAppliedFill
        {WdspChannel::NoiseBlankerFill::Zero};
    // AGC-off level and APF, kept out of m_config for the same reason; see
    // setAgcOffLevel()/setApf(). Requests, in the slice model's units.
    int m_agcOffLevel = kDefaultAgcOffLevel;
    bool m_apfOn = false;
    int m_apfLevel = kDefaultApfLevel;
    double m_apfCenterHz = 600.0;
    // Latest RXA_ADC_PK and when it was taken; see adcPeakDbfs() above. NaN and
    // 0 are the "never observed" sentinels, which is why neither is a value the
    // accessors can return. A steady_clock stamp rather than a QElapsedTimer
    // because a QElapsedTimer's members are not atomic and this is read from
    // another thread.
    std::atomic<float> m_adcPeakDbfs {std::numeric_limits<float>::quiet_NaN()};
    std::atomic<std::int64_t> m_adcPeakAtNs {0};
    // See spectrumGapDiscards(). Written on the I/O thread by onSequenceGap(),
    // read from the GUI thread by Hl2Backend::healthSnapshot().
    std::atomic<quint64> m_spectrumGapDiscards {0};
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
    bool m_refuseFilterTapsForTest = false;
    bool m_notchesEnabled = true;
    double m_notchTuneHz = 0.0;

    // Per-outcome counters for m_channel->processIq(); see processTally().
    // Written on the DSP thread in processIqBlock(), read from the GUI thread.
    WdspProcessTally m_processTally;

    bool m_audioMuted = false;
    // The S-meter tap's gate — the settle window after the channel starts
    // being fed real IQ again, and the read cadence — see the settle note in
    // processIqBlock() and WdspSMeter.h. Armed on the mute's release edge and
    // on a channel swap, ticked once per block that WDSP actually completes,
    // and only on the unmuted path, so it measures the same clock the meter
    // itself integrates on. DSP thread only, like m_audioMuted.
    WdspSMeterTap m_meterTap;
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
