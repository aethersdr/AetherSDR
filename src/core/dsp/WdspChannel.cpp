#include "core/dsp/WdspChannel.h"
#include "core/dsp/FftwPlannerLock.h"

#include <aether_wdsp.h>
#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// Only ever used to make the wisdom cache's temp filename unique per process.
long long currentProcessId()
{
#ifdef _WIN32
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(::getpid());
#endif
}

constexpr int kWdspChannelCount = 32;
constexpr int kRxChannelType = 0;
constexpr int kTxChannelType = 1;

// The prepared-WBFM geometry. validateConfig() refuses a wbfmReceive Config
// with any other, so open() hands exactly these to WDSP, and
// openWithExchangeDepthForTest() builds its call from the same constants.
constexpr int kWbfmInputSampleRate = 384000;
constexpr int kWbfmDspSampleRate = 192000;
constexpr int kWbfmOutputSampleRate = 48000;
constexpr std::size_t kWbfmInputBlockSize = 2048;
constexpr std::size_t kWbfmDspBlockSize = 1024;
// OpenChannelWithExchangeDepth's contract (aether_wdsp.h): depth 2..8, and
// channel.c refuses anything else before touching the channel.
constexpr int kMinExchangeDepth = 2;
constexpr int kMaxExchangeDepth = 8;
// Seven exchanges can emerge from one approved 8192-sample RTL callback.
// Depth 8 prepares seven output credits plus the existing worker stage:
// 2048 frames / 42.667 ms exchange-stage delay, 32 ms more than depth 2.
// It does not cover arbitrary starvation; every non-OK still withdraws.
constexpr int kPreparedWbfmExchangeDepth = 8;
static_assert(kPreparedWbfmExchangeDepth >= kMinExchangeDepth
              && kPreparedWbfmExchangeDepth <= kMaxExchangeDepth);

std::mutex g_channelMutex;
// Binds the single FFTW planner lock (FftwPlannerLock.h), shared with
// SpectralNR, Hl2Spectrum and AnanPanAnalyzer (#5895). Safe at dynamic init only
// because fftwPlannerMutex() is a function-local static and no WdspChannel
// entry point runs from a static constructor. Every WDSP control call here
// also takes it (RXASetNC/RXASetMP re-plan).
std::mutex& g_setupMutex = AetherSDR::fftwPlannerMutex();
// apfshadow.c's `selection`: 0 double-pole, 1 matched, 2 gaussian, 3 bi-quad.
constexpr int kApfSelectionDoublePole = 0;
std::array<bool, kWdspChannelCount> g_channelsInUse {};

// WDSP plans FFTs with FFTW_PATIENT (~220 planner calls over 11 distinct
// kind/size pairs per RX open, ~a minute cold). Wisdom is global, so prime it
// from a persisted cache; the first run measures and caches. Called under
// g_setupMutex. Uses FFTW's portable wisdom API (WDSPwisdom() is Windows-only).
std::string wisdomPath()
{
    namespace fs = std::filesystem;
    fs::path dir;
    // Test-only override (never set by the app): points the cache at a
    // build-local dir so tests can't touch the operator's cache while keeping their
    // own persistent one. tests/TestWdspWisdomIsolation.cpp sets it before main()
    // so it also holds for test binaries run directly.
    if (const char* override = std::getenv("AETHER_WDSP_WISDOM_DIR");
        override != nullptr && *override != '\0') {
        dir = override;
        std::error_code overrideEc;
        fs::create_directories(dir, overrideEc);   // best-effort
        return (dir / "wdsp-fftw-wisdom").string();
    }
#ifdef _WIN32
    if (const char* la = std::getenv("LOCALAPPDATA")) dir = la;
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) dir = xdg;
    else if (const char* home = std::getenv("HOME")) dir = fs::path(home) / ".cache";
#endif
    if (dir.empty()) {
        std::error_code ec;
        dir = fs::temp_directory_path(ec);
    }
    dir /= "aethersdr";
    std::error_code ec;
    fs::create_directories(dir, ec);   // best-effort
    return (dir / "wdsp-fftw-wisdom").string();
}

// Test/CI only: AETHER_WDSP_FFTW_TIMELIMIT=<seconds> caps FFTW's per-plan
// measuring time via its global setting (cold FFTW_PATIENT planning costs
// 20-190 s per process). Per plan, not total; FFTW can't interrupt one
// measurement. Unset is the shipping path. A bounded process never writes the
// wisdom cache (see open()), since rushed plans would reach the operator's
// radio; that's why this is one knob.
double plannerTimeLimitSeconds()
{
    static const double limit = [] {
        const char* raw = std::getenv("AETHER_WDSP_FFTW_TIMELIMIT");
        if (raw == nullptr || *raw == '\0')
            return -1.0;
        char* end = nullptr;
        const double parsed = std::strtod(raw, &end);
        // A malformed value is ignored rather than treated as 0: silently
        // rushing every plan because someone typed "yes" would be a very
        // quiet way to ship bad plans.
        if (end == raw || !std::isfinite(parsed) || parsed < 0.0)
            return -1.0;
        return parsed;
    }();
    return limit;
}

// True when this process is running with a bounded planner, i.e. its measured
// plans are not good enough to publish to the shared cache.
bool plannerIsBounded()
{
    return plannerTimeLimitSeconds() >= 0.0;
}

void loadWisdomOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        // Before the first plan, so it governs every one of them. FFTW's
        // default is FFTW_NO_TIMELIMIT and we only ever move off it here.
        if (plannerIsBounded())
            fftw_set_timelimit(plannerTimeLimitSeconds());
        fftw_import_wisdom_from_filename(wisdomPath().c_str());
    });
}

// Write the wisdom cache now, at the end of open() under g_setupMutex. Eager
// because at-exit hooks don't run on Hl2EmergencyStop's re-raised signal,
// crashes or force-quit. Per-open cost is ~10-50 KB on the I/O thread.
// Write-then-rename because other processes (other instances, ctest -j8)
// share the file: fftw_export_wisdom_to_filename truncates first and FFTW
// rejects a partial file wholesale. rename(2) is atomic; the temp name carries
// the pid.
void exportWisdomNow()
{
    namespace fs = std::filesystem;
    const fs::path final = wisdomPath();
    const fs::path tmp = fs::path(final).concat(
        ".tmp." + std::to_string(currentProcessId()));
    if (!fftw_export_wisdom_to_filename(tmp.string().c_str())) {
        // It opens with "w", so a failure part way through still leaves a SHORT
        // file sitting next to the real cache. The name is per-process rather
        // than per-call, so an unwritable cache directory keeps exactly one of
        // them rather than accumulating — but a truncated wisdom file is a trap
        // for whoever debugs this next, so do not leave one.
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return;
    }
    std::error_code ec;
    fs::rename(tmp, final, ec);
    if (ec)
        fs::remove(tmp, ec);   // leave no debris behind a failed publish
}

// Persist at process exit as well, so plans measured OUTSIDE an open() —
// SetRXAMode and SetRXABandpassFreqs build their own when the operator changes
// mode or drags a filter edge — are cached too. Those are not worth a file
// write each (a filter drag delivers them at ~30 Hz), so exit is the right
// moment for them, on the runs where exit is reached at all.
void armWisdomExportOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        std::atexit([] {
            // The exit export reads FFTW's process-global wisdom store too.
            auto lock = AetherSDR::fftwPlannerLock();
            exportWisdomNow();
        });
    });
}

void releaseChannelId(int channel)
{
    if (channel < 0 || channel >= kWdspChannelCount) {
        return;
    }
    const std::scoped_lock lock(g_channelMutex);
    g_channelsInUse[static_cast<std::size_t>(channel)] = false;
}

void setError(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
}

// Valid fircore tap counts. fircore splits the filter into nfor = nc / size
// partitions and uses idxmask = nfor - 1 as a POWER-OF-TWO MASK (firmin.c
// xfircore; firmin.h: "nc ... power of two, >= size"). A non-power-of-two
// nfor skips ring partitions (nfor 3: in-band 74 dB down, out-of-band louder);
// a non-multiple of size truncates the impulse (passband intact, stopband
// gone). A power of two in [256, 16384] makes nc / size an exact power of two.
// The 256 floor is WDSP's: min_notch_width() (nbp.c) divides by int(nc / 256),
// and it keeps minimumNotchWidthHz() identical to WDSP's.
constexpr int kMinFilterTaps = 256;
constexpr int kMaxFilterTaps = 16384;

bool filterTapsArePartitionable(int taps, std::size_t dspBlockSize) noexcept
{
    return taps >= kMinFilterTaps && taps <= kMaxFilterTaps &&
           (taps & (taps - 1)) == 0 && dspBlockSize != 0 &&
           static_cast<std::size_t>(taps) % dspBlockSize == 0;
}

} // namespace

namespace {

// The one place a Config becomes an OpenChannel call, shared by open() and
// openWithExchangeDepthForTest(). No exchangeDepth is the legacy OpenChannel
// (explicit depth 2, including reused slots). Returns WDSP's verdict: 0 is a
// refusal that left the channel untouched. Caller holds g_setupMutex.
int openWdspChannel(int channelId, const WdspChannel::Config& config,
                    std::optional<int> exchangeDepth) noexcept
{
    const int inputBlock = static_cast<int>(config.inputBlockSize);
    const int dspBlock = static_cast<int>(config.dspBlockSize);
    const int type = config.direction == WdspChannel::Direction::Receive
        ? kRxChannelType : kTxChannelType;
    // Open STOPPED. Every reference client configures mode, filters and AGC
    // after OpenChannel, and opening in state 1 means any samples arriving
    // during that window are demodulated by a default-configured channel --
    // wrong mode, wrong passband, AGC wide open. open() starts it with
    // SetChannelState once it is set up.
    constexpr int state = 0;
    const int blockForOutput = config.blockForOutput ? 1 : 0;
    if (exchangeDepth) {
        return OpenChannelWithExchangeDepth(channelId, inputBlock, dspBlock,
            config.inputSampleRate, config.dspSampleRate, config.outputSampleRate,
            type, state, config.muteDelayUpSec, config.muteSlewUpSec,
            config.muteDelayDownSec, config.muteSlewDownSec, blockForOutput,
            *exchangeDepth);
    }
    OpenChannel(channelId, inputBlock, dspBlock,
        config.inputSampleRate, config.dspSampleRate, config.outputSampleRate,
        type, state, config.muteDelayUpSec, config.muteSlewUpSec,
        config.muteDelayDownSec, config.muteSlewDownSec, blockForOutput);
    return 1;
}

// Apply the full AGC surface, mirroring pihpsdr's set_agc(). SetRXAAGCMode on
// its own leaves slope and the time constants at WDSP's defaults, and the
// default slope of 0 is total compression — it lifts the noise floor to the
// gain ceiling and clips. The per-mode constants match wcpAGC's own presets;
// setting them explicitly keeps the behaviour pinned if those defaults move.
void applyRxAgc(int channel, int mode, double topDb, int slopeDb, double fixedDb)
{
    SetRXAAGCMode(channel, mode);
    SetRXAAGCSlope(channel, slopeDb);
    SetRXAAGCTop(channel, topDb);
    SetRXAAGCFixed(channel, fixedDb);
    switch (mode) {
    case 1:   // long
        SetRXAAGCAttack(channel, 2);
        SetRXAAGCHang(channel, 2000);
        SetRXAAGCDecay(channel, 2000);
        SetRXAAGCHangThreshold(channel, 0);
        break;
    case 2:   // slow
        SetRXAAGCAttack(channel, 2);
        SetRXAAGCHang(channel, 1000);
        SetRXAAGCDecay(channel, 500);
        SetRXAAGCHangThreshold(channel, 0);
        break;
    case 3:   // medium
        SetRXAAGCAttack(channel, 2);
        SetRXAAGCHang(channel, 0);
        SetRXAAGCDecay(channel, 250);
        SetRXAAGCHangThreshold(channel, 100);
        break;
    case 4:   // fast
        SetRXAAGCAttack(channel, 2);
        SetRXAAGCHang(channel, 0);
        SetRXAAGCDecay(channel, 50);
        SetRXAAGCHangThreshold(channel, 100);
        break;
    default:  // off — only the fixed gain matters
        break;
    }
}

}  // namespace

std::unique_ptr<WdspChannel> WdspChannel::create(const Config& config,
                                                 std::string* error) noexcept
{
    if (!validateConfig(config, error)) {
        return nullptr;
    }
    if (GetWDSPVersion() != 210) {
        setError(error, "The linked WDSP library is not version 2.10");
        return nullptr;
    }
    std::optional<Reservation> reservation = reserveChannels(1);
    if (!reservation) {
        setError(error, "All WDSP channel slots are in use");
        return nullptr;
    }
    return create(config, *reservation, error);
}

std::unique_ptr<WdspChannel> WdspChannel::create(const Config& config,
    Reservation& reservation, std::string* error) noexcept
{
    if (!validateConfig(config, error)) {
        return nullptr;
    }
    if (GetWDSPVersion() != 210) {
        setError(error, "The linked WDSP library is not version 2.10");
        return nullptr;
    }
    if (reservation.m_count == 0) {
        setError(error, "No reserved WDSP channel slots remain");
        return nullptr;
    }
    const int channelId = reservation.m_ids[reservation.m_count - 1];

    std::unique_ptr<WdspChannel> channel(new (std::nothrow) WdspChannel(channelId, config));
    if (!channel) {
        setError(error, "Could not allocate the WDSP channel owner");
        return nullptr;
    }
    --reservation.m_count;
    if (!channel->open()) {
        setError(error, "WDSP refused the prepared exchange depth");
        return nullptr;
    }
    return channel;
}

std::optional<WdspChannel::Reservation> WdspChannel::reserveChannels(std::size_t count)
{
    if (count == 0 || count > kWdspChannelCount) {
        return std::nullopt;
    }
    Reservation reservation;
    const std::scoped_lock lock(g_channelMutex);
    for (int id = 0; id < kWdspChannelCount && reservation.m_count < count; ++id) {
        if (!g_channelsInUse[static_cast<std::size_t>(id)]) {
            reservation.m_ids[reservation.m_count++] = id;
        }
    }
    if (reservation.m_count != count) {
        // Nothing was acquired: failed batches cannot consume a partial pool.
        reservation.m_count = 0;
        return std::nullopt;
    }
    for (std::size_t index = 0; index < count; ++index) {
        g_channelsInUse[static_cast<std::size_t>(reservation.m_ids[index])] = true;
    }
    return reservation;
}

WdspChannel::Reservation::Reservation(Reservation&& other) noexcept
    : m_ids(other.m_ids), m_count(std::exchange(other.m_count, 0))
{
}

WdspChannel::Reservation& WdspChannel::Reservation::operator=(Reservation&& other) noexcept
{
    if (this != &other) {
        release();
        m_ids = other.m_ids;
        m_count = std::exchange(other.m_count, 0);
    }
    return *this;
}

WdspChannel::Reservation::~Reservation()
{
    release();
}

void WdspChannel::Reservation::release() noexcept
{
    while (m_count != 0) {
        releaseChannelId(m_ids[--m_count]);
    }
}

struct WdspChannel::WbfmState
{
    explicit WbfmState(const Config& config)
        : input(2 * config.inputBlockSize), output(4 * config.inputBlockSize),
          i(config.inputBlockSize), q(config.inputBlockSize), gain(config.wbfmReceive->outputGain),
          filter(create_bps(1, 0, static_cast<int>(config.inputBlockSize), input.data(),
              output.data(), -config.filterHighHz, -config.filterLowHz,
              config.inputSampleRate, 0, 1.0), destroy_bps)
    {
        // WDSP fir_bandpass uses the negative complex-frequency convention.
        // Negate/swap edges so this prefilter follows mathematical RTL IQ.
        // 2048-frame blocks give 2049 BH4 taps and a 4096-point overlap-save
        // FFT. Unlike fircore, xbps has no control-update mutex on this path.
        // PATIENT planning can overwrite its scratch input; begin with a
        // defined overlap history after the plans have been constructed.
        flush_bps(filter.get());
    }
    std::vector<double> input;
    std::vector<double> output;
    std::vector<float> i;
    std::vector<float> q;
    double gain;
    std::unique_ptr<_bps, decltype(&destroy_bps)> filter;
};

WdspChannel::WdspChannel(int channelId, const Config& config) noexcept
    : m_channelId(channelId)
    , m_config(config)
    , m_outputBlockSize(computeOutputBlockSize(config))
{
}

WdspChannel::~WdspChannel()
{
    m_controlOperation.store(true, std::memory_order_seq_cst);
    while (m_callbacksInFlight.load(std::memory_order_seq_cst) != 0) {
        // Yield to the real-time thread we are draining rather than burning a
        // core; avoids priority inversion if it was preempted mid-fexchange2.
        std::this_thread::yield();
    }
    close();
    releaseChannelId(m_channelId);
}

bool WdspChannel::outputReady() noexcept
{
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        return false;
    }
    m_callbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    const bool ready = GetChannelOutputReady(m_channelId) != 0;
    m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);
    return ready;
}

WdspChannel::ProcessResult WdspChannel::processIq(std::span<const float> inputI,
                                                  std::span<const float> inputQ,
                                                  std::span<float> outputLeft,
                                                  std::span<float> outputRight) noexcept
{
    if (inputI.size() != m_config.inputBlockSize || inputQ.size() != inputI.size() ||
        outputLeft.size() != m_outputBlockSize || outputRight.size() != outputLeft.size()) {
        return ProcessResult::InvalidBuffer;
    }
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        return ProcessResult::Busy;
    }

    m_callbacksInFlight.fetch_add(1, std::memory_order_seq_cst);
    if (m_controlOperation.load(std::memory_order_seq_cst)) {
        m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);
        return ProcessResult::Busy;
    }

    const uint64_t allocationsBefore = wdspPortThreadAllocationSequence();
    int wdspError = 0;

    // ── Noise blanker, ahead of the channel ───────────────────────────────
    //
    // Inside the allocation-guarded window deliberately: neither xanb() nor
    // xnob() allocates today, and if a future WDSP snapshot changes that, this
    // reports AllocationViolation rather than letting a malloc land on the
    // real-time path unnoticed.
    const float* channelI = inputI.data();
    const float* channelQ = inputQ.data();
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
            // Transmitting. Skip the stage entirely — see setNoiseBlankerHold.
        } else {
            // Not flushed on leaving a hold: the stage was skipped, not fed, so its
            // running average still holds the pre-TX level and it's armed for the first
            // RX sample. A flush resets it to full scale and leaves the blanker blind for
            // ~200 ms at backtau 0.05 s (aether_wdsp.h ARMING DELAY). The delay line
            // carries only trans_count + adv_count = 8 samples (0.17 ms) across.
            const std::size_t n = inputI.size();
            for (std::size_t k = 0; k < n; ++k) {
                m_nbInterleaved[2 * k] = static_cast<double>(inputI[k]);
                m_nbInterleaved[2 * k + 1] = static_cast<double>(inputQ[k]);
            }
            // In place: both blankers read and write the same buffer. Which one
            // is a relaxed load of the kind, inside the gate that already told
            // us one of them is running — the control path stores both, gate
            // last, so a kind seen here is a stage that is running.
            if (m_nbKind.load(std::memory_order_relaxed) == NoiseBlanker::Advanced) {
                xnobEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
            } else {
                xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
            }
            for (std::size_t k = 0; k < n; ++k) {
                m_nbI[k] = static_cast<float>(m_nbInterleaved[2 * k]);
                m_nbQ[k] = static_cast<float>(m_nbInterleaved[2 * k + 1]);
            }
            channelI = m_nbI.data();
            channelQ = m_nbQ.data();
        }
    }

    if (m_wbfm) {
        for (std::size_t sample = 0; sample < inputI.size(); ++sample) {
            m_wbfm->input[2 * sample] = channelI[sample];
            m_wbfm->input[2 * sample + 1] = channelQ[sample];
        }
        xbps(m_wbfm->filter.get(), 0);
        for (std::size_t sample = 0; sample < inputI.size(); ++sample) {
            m_wbfm->i[sample] = static_cast<float>(m_wbfm->output[2 * sample]);
            m_wbfm->q[sample] = static_cast<float>(m_wbfm->output[2 * sample + 1]);
        }
        channelI = m_wbfm->i.data();
        channelQ = m_wbfm->q.data();
    }

    if (!m_running.load(std::memory_order_relaxed)) {
        // Once the down-slew finishes, fexchange2 neither writes nor zeroes the
        // output, so making "stopped" mean silence is our job. (WDSP's downslew2
        // currently leaves zeros anyway, but that isn't a contract.) Zero first, then
        // still call it: while the ramp runs it overwrites this with the slewed tail,
        // and the call is what advances the ramp.
        std::ranges::fill(outputLeft, 0.0f);
        std::ranges::fill(outputRight, 0.0f);
    }
    fexchange2(m_channelId,
               const_cast<float*>(channelI),
               const_cast<float*>(channelQ),
               outputLeft.data(), outputRight.data(), &wdspError);
    if (m_wbfm) {
        if (wdspError == 0) {
            for (std::size_t sample = 0; sample < outputLeft.size(); ++sample) {
                outputLeft[sample] *= static_cast<float>(m_wbfm->gain);
                outputRight[sample] *= static_cast<float>(m_wbfm->gain);
            }
        }
        m_wbfmReception.reset();
        AetherWdspWbfmObservation observation{};
        if (wdspError == 0 && m_running.load(std::memory_order_relaxed)
            && GetRXAWBFMReception(m_channelId, &observation) && observation.valid) {
            m_wbfmReception = AetherSDR::WfmReceptionDiagnostics{
                true, observation.pilotMagnitude, observation.pilotLocked != 0,
                observation.lockDurationMs, observation.lockLossCount, observation.reacquisitionCount,
                observation.observationDurationMs, observation.stableDurationMs,
                observation.pilotEngageThreshold, observation.pilotReleaseThreshold,
                observation.consecutiveHighBlocks, observation.consecutiveLowBlocks,
                observation.engageBlocks, observation.releaseBlocks, observation.observationSequence};
        }
        const bool stereo = m_wbfmReception && m_wbfmReception->pilotLocked
            && !m_config.wbfmReceive->forceMono;
        m_wbfmStereo.store(stereo ? 1 : 0, std::memory_order_relaxed);
    }
    const uint64_t allocationsAfter = wdspPortThreadAllocationSequence();
    m_callbacksInFlight.fetch_sub(1, std::memory_order_seq_cst);

    if (allocationsAfter != allocationsBefore) {
        return ProcessResult::AllocationViolation;
    }
    if (wdspError == -2) {
        return ProcessResult::Underrun;
    }
    if (wdspError != 0) {
        return ProcessResult::EngineError;
    }
    return ProcessResult::Ok;
}

bool WdspChannel::discardTransmitData() noexcept
{
    if (m_config.direction != Direction::Transmit || !beginControlOperation()) {
        return false;
    }
    const bool discarded = DiscardTXAChannelData(m_channelId) != 0;
    if (discarded) {
        m_running.store(false, std::memory_order_relaxed);
    }
    endControlOperation();
    return discarded;
}

bool WdspChannel::setRunning(bool running) noexcept
{
    // Idempotent, and deliberately BEFORE the handshake: WDSP's own
    // SetChannelState already no-ops when the state matches, so taking the
    // control fence here would make a redundant T/R edge able to fail purely
    // because a block was in flight.
    if (m_running.load(std::memory_order_relaxed) == running) {
        return true;
    }
    if (!beginControlOperation()) {
        return false;
    }
    // dmode 0 (see the header): the drain is processIq()'s job. Outside
    // g_setupMutex, which only serialises the FFTW planner. The one
    // SetChannelState inside the lock is open()'s start, part of a planner-bound
    // build; patch 8's Sleep(1) wait can't fire there because pre_main_build
    // clears flushflag before OpenChannel starts the channel (channel.c), an
    // invariant of vendored code.
    if (m_wbfm) {
        if (running) { flush_bps(m_wbfm->filter.get()); }
        m_wbfmStereo.store(0, std::memory_order_relaxed);
        m_wbfmReception.reset();
    }
    SetChannelState(m_channelId, running ? 1 : 0, 0);
    m_running.store(running, std::memory_order_relaxed);
    endControlOperation();
    return true;
}

bool WdspChannel::reconfigure(const Config& config, std::string* error) noexcept
{
    if (!validateConfig(config, error) || !beginControlOperation()) {
        if (error != nullptr && error->empty()) {
            *error = "WDSP channel is processing audio";
        }
        return false;
    }

    const bool wasRunning = m_running.load(std::memory_order_relaxed);
    close();
    m_config = config;
    m_outputBlockSize = computeOutputBlockSize(m_config);
    if (!open()) {
        setError(error, "WDSP refused the prepared exchange depth");
        endControlOperation();
        return false;
    }
    if (!wasRunning) {
        // Restore the state found, like WDSP's own rebuilds (channel.c
        // SetDSPBuffsize); open() always starts, so a stopped channel must be stopped
        // again. Leaving a down-ramp unclocked is safe: patch 7 makes case 1 cancel
        // it. Outside g_setupMutex, like setRunning() and close().
        SetChannelState(m_channelId, 0, 0);
        m_running.store(false, std::memory_order_relaxed);
    }
    endControlOperation();
    return true;
}

bool WdspChannel::setMode(Mode mode) noexcept
{
    // An opt-in FM channel changes family through a complete reconfiguration;
    // otherwise its panel normalization would leak into another demodulator.
    if ((m_config.fmReceive && mode != Mode::Fm)
        || (m_config.wbfmReceive && mode != Mode::Wbfm)) { return false; }
    if (!beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_config.direction == Direction::Receive) {
            SetRXAMode(m_channelId, wdspMode(mode));
            // The squelch belongs to a mode family, so a mode change moves it
            // to the new family's stage and stops the old one — see
            // setSquelch() in the header.
            applySquelchLocked(mode);
        } else {
            SetTXAMode(m_channelId, wdspMode(mode));
        }
    }
    m_config.mode = mode;
    endControlOperation();
    return true;
}

bool WdspChannel::setFilter(double lowHz, double highHz) noexcept
{
    if (!std::isfinite(lowHz) || !std::isfinite(highHz) || lowHz >= highHz ||
        (m_config.wbfmReceive && (lowHz < -0.45 * m_config.inputSampleRate
            || highHz > 0.45 * m_config.inputSampleRate)) || !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_wbfm) {
            setFreqs_bps(m_wbfm->filter.get(), -highHz, -lowHz);
            flush_bps(m_wbfm->filter.get());
        } else if (m_config.direction == Direction::Receive) {
            // RXASetPassband, not SetRXABandpassFreqs: the latter sets only the
            // bandpass and leaves the NBP stage — the filter actually in
            // circuit — untouched, so nothing selects a sideband. Both
            // reference clients use the composite call.
            SetRXABandpassFreqs(m_channelId, lowHz, highHz);
            RXANBPSetFreqs(m_channelId, lowHz, highHz);
        } else {
            SetTXABandpassFreqs(m_channelId, lowHz, highHz);
        }
    }
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    endControlOperation();
    return true;
}

bool WdspChannel::setWbfmDeemphasis(WbfmReceive::Deemphasis deemphasis) noexcept
{
    if (!m_config.wbfmReceive
        || (deemphasis != WbfmReceive::Deemphasis::Us75 && deemphasis != WbfmReceive::Deemphasis::Us50)
        || !beginControlOperation()) { return false; }
    SetRXAWBFMdmph(m_channelId, 1, deemphasis == WbfmReceive::Deemphasis::Us50 ? 1 : 0);
    m_config.wbfmReceive->deemphasis = deemphasis;
    endControlOperation();
    return true;
}

std::optional<bool> WdspChannel::wbfmStereoDetected() const noexcept
{
    const int observed = m_wbfmStereo.load(std::memory_order_relaxed);
    if (observed < 0) { return std::nullopt; }
    return observed != 0;
}

bool WdspChannel::setFmDeviation(double deviationHz) noexcept
{
    // Out of range is refused rather than clamped, and RANGE is the word:
    // WDSP computes again = rate / (deviation * TWOPI), so zero divides by
    // zero, a negative value inverts the recovered audio — and a tiny positive
    // value, which a sign check waves through, sends again to infinity and the
    // detector emits inf. See Config::kMinFmDeviationHz.
    if (m_config.direction != Direction::Receive || !std::isfinite(deviationHz) ||
        deviationHz < Config::kMinFmDeviationHz ||
        deviationHz > Config::kMaxFmDeviationHz ||
        (m_config.fmReceive && deviationHz >= m_config.dspSampleRate / 2.0) ||
        !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        SetRXAFMDeviation(m_channelId, deviationHz);
    }
    // Stored so open() can re-push it: reconfigure() frees the fmd stage.
    m_config.fmDeviationHz = deviationHz;
    endControlOperation();
    return true;
}

bool WdspChannel::setSquelch(bool on, int level, double levelOffsetDb) noexcept
{
    if (m_config.direction != Direction::Receive || !std::isfinite(levelOffsetDb)
        || !beginControlOperation()) {
        return false;
    }
    m_config.squelchEnabled = on;
    m_config.squelchLevel = std::clamp(level, 0, 100);
    m_config.levelSquelchOffsetDb = levelOffsetDb;
    {
        const std::scoped_lock setupLock(g_setupMutex);
        applySquelchLocked(m_config.mode);
    }
    endControlOperation();
    return true;
}

WdspChannel::SquelchStage WdspChannel::squelchStageFor(Mode mode) noexcept
{
    // See the header for why SSB is on amsq and CW on nothing.
    switch (mode) {
    case Mode::Fm:
        return SquelchStage::Fm;
    case Mode::Am:
    case Mode::Sam:
    case Mode::Dsb:
    case Mode::Lsb:
    case Mode::Usb:
        return SquelchStage::Level;
    case Mode::Cwl:
    case Mode::Cwu:
    case Mode::Digu:
    case Mode::Digl:
    case Mode::Spec:
    case Mode::Drm:
    case Mode::Wbfm:
        break;
    }
    return SquelchStage::None;
}

double WdspChannel::fmSquelchThresholdForLevel(int level) noexcept
{
    // pihpsdr's map (dl1ycf/pihpsdr src/receiver.c rx_set_squelch: "FM
    // squelch: 1.0 ... 0.01 expon. interpolation"). fmsq mutes when averaged
    // detector noise EXCEEDS the threshold, so 1.0 lets almost anything
    // through and 0.01 needs a near-quieting signal.
    const double clamped = std::clamp(static_cast<double>(level), 0.0, 100.0);
    return std::pow(10.0, -2.0 * clamped / 100.0);
}

double WdspChannel::levelSquelchThresholdDbfsForLevel(int level, double offsetDb) noexcept
{
    // Fitted to HL2 measurements at the offset-0 gain (#5982, #6092): level 50
    // sits between the no-signal floor and a strong broadcast carrier.
    const double clamped = std::clamp(static_cast<double>(level), 0.0, 100.0);
    return kLevelSquelchBaseDbfs + kLevelSquelchDbPerStep * clamped + offsetDb;
}

void WdspChannel::applySquelchLocked(Mode mode) noexcept
{
    // EVERY RUN FLAG, EVERY TIME. Writing only the stage being turned on
    // would leave the previous mode's stage running after a mode change: amsq
    // left on across AM -> FM gates FM on carrier level, and fmsq left on
    // outside FM gates on a trigger buffer nothing refreshes any more.
    AppliedSquelch applied;
    applied.stage = squelchStageFor(mode);
    {
        const std::scoped_lock recordLock(m_appliedSquelchMutex);
        applied.applications = m_appliedSquelch.applications + 1;
    }
    // Level 0 runs nothing: "open" by construction, whatever the stage would
    // have made of its bottom threshold.
    const bool run = m_config.squelchEnabled && m_config.squelchLevel > 0;
    const int level = m_config.squelchLevel;
    switch (applied.stage) {
    case SquelchStage::Fm:
        applied.threshold = fmSquelchThresholdForLevel(level);
        SetRXAFMSQThreshold(m_channelId, applied.threshold);
        applied.fmRun = run;
        break;
    case SquelchStage::Level:
        applied.threshold =
            levelSquelchThresholdDbfsForLevel(level, m_config.levelSquelchOffsetDb);
        SetRXAAMSQThreshold(m_channelId, applied.threshold);
        applied.amRun = run;
        break;
    case SquelchStage::None:
        break;
    }
    SetRXAFMSQRun(m_channelId, applied.fmRun ? 1 : 0);
    SetRXAAMSQRun(m_channelId, applied.amRun ? 1 : 0);
    const std::scoped_lock recordLock(m_appliedSquelchMutex);
    m_appliedSquelch = applied;
}

WdspChannel::AppliedSquelch WdspChannel::appliedSquelch() const
{
    const std::scoped_lock recordLock(m_appliedSquelchMutex);
    return m_appliedSquelch;
}

bool WdspChannel::setAgc(int agcMode, double maximumGainDb) noexcept
{
    // RX-only: SetRXAAGC* has no transmit counterpart, and a TX channel has no
    // AGC stage to configure.
    if (m_config.direction != Direction::Receive || !std::isfinite(maximumGainDb) ||
        !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        applyRxAgc(m_channelId, agcMode, maximumGainDb,
                   m_config.agcSlopeDb, m_config.agcFixedGainDb);
    }
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    endControlOperation();
    return true;
}

bool WdspChannel::setAgcFixedGain(double fixedGainDb) noexcept
{
    if (m_config.direction != Direction::Receive || !std::isfinite(fixedGainDb) ||
        !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        SetRXAAGCFixed(m_channelId, fixedGainDb);
    }
    // Stored so setAgc() and open() push the operator's value, not the
    // construction default: applyRxAgc() re-sends it on every mode change.
    m_config.agcFixedGainDb = fixedGainDb;
    endControlOperation();
    return true;
}

bool WdspChannel::apfParametersValid(double centerHz, double bandwidthHz,
                                     double gain) noexcept
{
    // The double-pole design divides by the centre (H(f) = (bw/fc) / ...), and
    // calc_dpole_nc sizes its FIR from the bandwidth; a zero or non-finite
    // value in either is a filter WDSP cannot build. A zero gain is not a
    // filter at all — it is a mute that looks like an APF.
    return std::isfinite(centerHz) && std::isfinite(bandwidthHz) && std::isfinite(gain)
           && centerHz > 0.0 && bandwidthHz > 0.0 && gain > 0.0;
}

bool WdspChannel::setApf(bool enabled, double centerHz, double bandwidthHz,
                         double gain) noexcept
{
    if (m_config.direction != Direction::Receive ||
        !apfParametersValid(centerHz, bandwidthHz, gain)) {
        return false;
    }
    // Unchanged: return before the handshake and g_setupMutex (the global FFTW
    // planner lock, held on the EP2-pacing I/O thread), like setMinimumPhase().
    // applyApf() runs on every mode change, mostly to keep an off stage off.
    // Exact equality holds: m_config is only what an accepted setApf() or
    // open() gave WDSP, and nothing else writes SPCW.
    if (enabled == m_config.apfEnabled && centerHz == m_config.apfCenterHz
        && bandwidthHz == m_config.apfBandwidthHz && gain == m_config.apfGain) {
        return true;
    }
    if (!beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        // Shape before run: switching on first would run one block through
        // whatever design the stage last held.
        SetRXASPCWFreq(m_channelId, centerHz);
        SetRXASPCWBandwidth(m_channelId, bandwidthHz);
        SetRXASPCWGain(m_channelId, gain);
        SetRXASPCWRun(m_channelId, enabled ? 1 : 0);
    }
    m_config.apfEnabled = enabled;
    m_config.apfCenterHz = centerHz;
    m_config.apfBandwidthHz = bandwidthHz;
    m_config.apfGain = gain;
    endControlOperation();
    return true;
}

bool WdspChannel::setFilterTaps(int taps) noexcept
{
    // The SAME predicate validateConfig() applies, so the setter and open()
    // cannot disagree about what fircore can run. nc >= size is only half of
    // it; see filterTapsArePartitionable() for the other half and for what it
    // costs to be missing.
    if (m_config.direction != Direction::Receive ||
        !filterTapsArePartitionable(taps, m_config.dspBlockSize)) {
        return false;
    }
    if (taps == m_config.filterTaps) {
        // Already there. Returning early rather than paying the stop/re-plan
        // keeps this cheap enough for a caller that recomputes a desired length
        // on every notch edit.
        return true;
    }
    if (!beginControlOperation()) {
        return false;
    }

    // Stop the channel ourselves, outside g_setupMutex, before RXASetNC. Its own
    // SetChannelState(ch, 0, 1) waits for flushflag, which only fexchange2's
    // down-ramp clears, and processIq() now returns Busy, so the wait always times
    // out: 155-227 ms per call (nanosleep port) under the global FFTW lock, which
    // Hl2Spectrum takes on the EP2-pacing I/O thread (#5424). Pre-stopping makes
    // both of RXASetNC's SetChannelState calls no-ops, so the lock covers only
    // the six FIR re-plans. Same convention as setRunning(), reconfigure() and
    // close(): dmode 0, outside the lock. An unclocked down-ramp is safe (patch 7
    // cancels it), and the restore doesn't wait (patch 8 waits only on exchange
    // CLEAR; an unclocked ramp leaves it SET).
    const bool wasRunning = m_running.load(std::memory_order_relaxed);
    if (wasRunning) {
        SetChannelState(m_channelId, 0, 0);
    }
    {
        // setNc_fircore re-plans six FIR cores and FFTW's planner is process
        // global. This, and only this, is what the lock is for.
        const std::scoped_lock setupLock(g_setupMutex);
        RXASetNC(m_channelId, taps);
    }
    if (wasRunning) {
        SetChannelState(m_channelId, 1, 0);
    }
    m_config.filterTaps = taps;
    endControlOperation();
    return true;
}

bool WdspChannel::setMinimumPhase(bool on) noexcept
{
    if (m_config.direction != Direction::Receive) {
        return false;
    }
    if (on == m_config.minimumPhase) {
        return true;
    }
    if (!beginControlOperation()) {
        return false;
    }
    {
        // RXASetMP does not stop the channel, but it does run every mask
        // through mp_imp_exec, which plans and executes FFTs at nc * pfactor.
        // Same planner, same lock.
        const std::scoped_lock setupLock(g_setupMutex);
        RXASetMP(m_channelId, on ? 1 : 0);
    }
    m_config.minimumPhase = on;
    endControlOperation();
    return true;
}

bool WdspChannel::setShift(double shiftHz) noexcept
{
    if (m_config.direction != Direction::Receive || !std::isfinite(shiftHz)
        || !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        SetRXAShiftFreq(m_channelId, shiftHz);
        // Running the stage at 0 Hz costs a pointless rotate per sample, so
        // switch it off when there is no offset to apply.
        SetRXAShiftRun(m_channelId, shiftHz != 0.0 ? 1 : 0);
        m_shiftHz = shiftHz;
        // The notch database tracks the shift separately and WDSP never links
        // the two itself. Both reference clients set them together (Thetis
        // radio.cs RXOsc sets SetRXAShiftFreq and RXANBPSetShiftFrequency on
        // the same line); setting only the first leaves every notch offset by
        // the shift, which stays invisible until someone tunes off centre.
        applyNotchShift();
    }
    endControlOperation();
    return true;
}

void WdspChannel::applyNotchShift() noexcept
{
    // Caller holds g_setupMutex.
    //
    // The SAME value the shift stage got, not a negation of it. Thetis sets
    // both on one line from one variable (radio.cs RXOsc) for the same reason:
    // the notch database is not a second opinion about the shift, it is the
    // copy the filter-mask rebuild reads.
    RXANBPSetShiftFrequency(m_channelId, m_shiftHz);
}

bool WdspChannel::addNotch(int index, double centerHz, double widthHz,
                           bool active) noexcept
{
    if (m_config.direction != Direction::Receive || index < 0
        || !std::isfinite(centerHz) || !std::isfinite(widthHz) || widthHz <= 0.0
        || !beginControlOperation()) {
        return false;
    }
    int result = -1;
    {
        const std::scoped_lock setupLock(g_setupMutex);
        result = RXANBPAddNotch(m_channelId, index, centerHz, widthHz,
                                active ? 1 : 0);
    }
    endControlOperation();
    return result == 0;
}

bool WdspChannel::editNotch(int index, double centerHz, double widthHz,
                            bool active) noexcept
{
    if (m_config.direction != Direction::Receive || index < 0
        || !std::isfinite(centerHz) || !std::isfinite(widthHz) || widthHz <= 0.0
        || !beginControlOperation()) {
        return false;
    }
    int result = -1;
    {
        const std::scoped_lock setupLock(g_setupMutex);
        result = RXANBPEditNotch(m_channelId, index, centerHz, widthHz,
                                 active ? 1 : 0);
    }
    endControlOperation();
    return result == 0;
}

bool WdspChannel::removeNotch(int index) noexcept
{
    if (m_config.direction != Direction::Receive || index < 0
        || !beginControlOperation()) {
        return false;
    }
    int result = -1;
    {
        const std::scoped_lock setupLock(g_setupMutex);
        result = RXANBPDeleteNotch(m_channelId, index);
    }
    endControlOperation();
    return result == 0;
}

bool WdspChannel::setNotchesEnabled(bool on) noexcept
{
    if (m_config.direction != Direction::Receive || !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        RXANBPSetNotchesRun(m_channelId, on ? 1 : 0);
    }
    endControlOperation();
    return true;
}

bool WdspChannel::setNotchTuneFrequency(double tuneHz) noexcept
{
    if (m_config.direction != Direction::Receive || !std::isfinite(tuneHz)
        || !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        RXANBPSetTuneFrequency(m_channelId, tuneHz);
    }
    endControlOperation();
    return true;
}

int WdspChannel::notchCount() const noexcept
{
    if (m_config.direction != Direction::Receive)
        return 0;
    // RXANBPGetNumNotches takes the channel's own csDSP, so it is safe against
    // the DSP thread — but not against close() on another thread, which frees
    // the NOTCHDB under g_setupMutex. Every other accessor on this class either
    // goes through beginControlOperation() or takes that lock; these two are
    // read-only and cheap, so the lock is all they need to match the contract.
    const std::scoped_lock setupLock(g_setupMutex);
    if (!m_open)
        return 0;
    int count = 0;
    RXANBPGetNumNotches(m_channelId, &count);
    return count;
}

bool WdspChannel::notchAt(int index, double* centerHz, double* widthHz,
                          bool* active) const noexcept
{
    if (m_config.direction != Direction::Receive || index < 0)
        return false;
    // See notchCount() — same interlock, same reason.
    const std::scoped_lock setupLock(g_setupMutex);
    if (!m_open)
        return false;
    double center = 0.0;
    double width = 0.0;
    int isActive = 0;
    if (RXANBPGetNotch(m_channelId, index, &center, &width, &isActive) != 0)
        return false;
    if (centerHz != nullptr) *centerHz = center;
    if (widthHz != nullptr) *widthHz = width;
    if (active != nullptr) *active = isActive != 0;
    return true;
}

double WdspChannel::minimumNotchWidthHz() const noexcept
{
    // WDSP's min_notch_width() (nbp.c) for wintype 0, mirrored so widths can be
    // offered before a notch exists. nc / 256 is INTEGER division in WDSP (NBP::nc
    // is int); keep it so if the power-of-two tap guard is ever widened. NBP::rate
    // is a double, so the rate division stays real.
    if (m_config.direction != Direction::Receive
        || m_config.filterTaps < kMinFilterTaps || m_config.dspSampleRate <= 0) {
        return 0.0;
    }
    return 1600.0 / static_cast<double>(m_config.filterTaps / 256)
           * (static_cast<double>(m_config.dspSampleRate) / 48000.0);
}

double WdspChannel::meter(Meter which) const noexcept
{
    if (m_config.direction != Direction::Receive)
        return -300.0;
    return GetRXAMeter(m_channelId, static_cast<int>(which));
}

std::size_t WdspChannel::outputBlockSize() const noexcept
{
    return m_outputBlockSize;
}

std::size_t WdspChannel::computeOutputBlockSize(const Config& config) noexcept
{
    // Exact: validateConfig() guarantees inputSampleRate > 0 and that
    // inputBlockSize * outputSampleRate is a whole multiple of inputSampleRate.
    return config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate) /
           static_cast<std::size_t>(config.inputSampleRate);
}

uint64_t WdspChannel::allocationSequenceForTest() noexcept
{
    return wdspPortAllocationSequence();
}

uint64_t WdspChannel::outstandingAllocationsForTest() noexcept
{
    return wdspPortOutstandingAllocations();
}

uint64_t WdspChannel::threadAllocationSequenceForTest() noexcept
{
    return wdspPortThreadAllocationSequence();
}

int WdspChannel::openWithExchangeDepthForTest(int channelId, int exchangeDepth) noexcept
{
    // A refusal probe only. A call WDSP would accept would open a channel
    // nothing owns or closes, or rebuild one a live WdspChannel owns, so it is
    // refused here and never reaches WDSP.
    if (channelId >= 0 && channelId < kWdspChannelCount
        && exchangeDepth >= kMinExchangeDepth && exchangeDepth <= kMaxExchangeDepth) {
        return kOpenProbeWouldOpen;
    }
    Config config;
    config.mode = Mode::Wbfm;
    config.wbfmReceive.emplace();
    config.blockForOutput = false;
    config.inputSampleRate = kWbfmInputSampleRate;
    config.dspSampleRate = kWbfmDspSampleRate;
    config.outputSampleRate = kWbfmOutputSampleRate;
    config.inputBlockSize = kWbfmInputBlockSize;
    config.dspBlockSize = kWbfmDspBlockSize;
    // The same lock and setup order as open().
    const std::scoped_lock setupLock(g_setupMutex);
    loadWisdomOnce();
    return openWdspChannel(channelId, config, exchangeDepth);
}

void WdspChannel::setWorkerHandoffPauseForTest(unsigned microseconds) noexcept
{
    wdspPortSetHandoffPauseForTest(microseconds);
}

void WdspChannel::setWorkerHandoffHoldForTest(bool enabled) noexcept
{
    wdspPortSetHandoffHoldForTest(enabled ? 1 : 0);
}

bool WdspChannel::workerHandoffHeldForTest() noexcept
{
    return wdspPortHandoffHeldForTest() != 0;
}

void WdspChannel::setWorkerOutputCopyHoldForTest(bool enabled) noexcept
{
    wdspPortSetOutputCopyHoldForTest(enabled ? 1 : 0);
}

bool WdspChannel::workerOutputCopyHeldForTest() noexcept
{
    return wdspPortOutputCopyHeldForTest() != 0;
}

int WdspChannel::outputSamplesReadyForTest() const noexcept
{
    return GetChannelOutputSamplesForTest(m_channelId);
}

void WdspChannel::synchronizeWorkerForTest() const noexcept
{
    SynchronizeChannelWorkerForTest(m_channelId);
}

void WdspChannel::restorePureSignalCorrectionForTest(const char* path) const noexcept
{
    // PSRestoreCorr only copies the name; WDSP's signature is just not const.
    PSRestoreCorr(m_channelId, const_cast<char*>(path));
}

std::unique_lock<std::mutex> WdspChannel::fftwSetupLock()
{
    // Forwards, and keeps its name so Hl2Spectrum, AnanPanAnalyzer and
    // wdsp_channel_test need no churn. The lock itself lives in
    // FftwPlannerLock.h; new code outside this class should take
    // fftwPlannerLock() directly rather than reaching through WDSP.
    return AetherSDR::fftwPlannerLock();
}

bool WdspChannel::validateConfig(const Config& config, std::string* error) noexcept
{
    if (config.inputBlockSize == 0 || config.dspBlockSize == 0 ||
        config.inputBlockSize > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        config.dspBlockSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        setError(error, "WDSP block sizes must be positive 32-bit values");
        return false;
    }
    if (config.inputSampleRate <= 0 || config.dspSampleRate <= 0 ||
        config.outputSampleRate <= 0) {
        setError(error, "WDSP sample rates must be positive");
        return false;
    }
    if ((config.inputSampleRate % config.dspSampleRate != 0 &&
         config.dspSampleRate % config.inputSampleRate != 0) ||
        (config.outputSampleRate % config.dspSampleRate != 0 &&
         config.dspSampleRate % config.outputSampleRate != 0) ||
        (config.inputBlockSize * static_cast<std::size_t>(config.outputSampleRate)) %
            static_cast<std::size_t>(config.inputSampleRate) != 0) {
        setError(error, "WDSP rates and block sizes must have integral ratios");
        return false;
    }
    if (!std::isfinite(config.filterLowHz) || !std::isfinite(config.filterHighHz) ||
        config.filterLowHz >= config.filterHighHz) {
        setError(error, "WDSP filter edges are invalid");
        return false;
    }
    if (config.fmReceive && (config.direction != Direction::Receive || config.mode != Mode::Fm
        || !std::isfinite(config.fmDeviationHz) || config.fmDeviationHz <= 0.0
        || config.fmDeviationHz >= config.dspSampleRate / 2.0)) {
        setError(error, "WDSP receive FM deviation is invalid for this channel");
        return false;
    }
    if (config.wbfmReceive && (config.fmReceive || config.direction != Direction::Receive
        || config.mode != Mode::Wbfm || config.inputSampleRate != kWbfmInputSampleRate
        || config.dspSampleRate != kWbfmDspSampleRate
        || config.outputSampleRate != kWbfmOutputSampleRate
        || config.inputBlockSize != kWbfmInputBlockSize
        || config.dspBlockSize != kWbfmDspBlockSize
        || config.filterLowHz < -0.45 * config.inputSampleRate
        || config.filterHighHz > 0.45 * config.inputSampleRate
        || !std::isfinite(config.wbfmReceive->outputGain)
        || config.wbfmReceive->outputGain < 0.0 || config.wbfmReceive->outputGain > 1.0
        || (config.wbfmReceive->deemphasis != WbfmReceive::Deemphasis::Us75
            && config.wbfmReceive->deemphasis != WbfmReceive::Deemphasis::Us50))) {
        setError(error, "Invalid WDSP broadcast receive profile");
        return false;
    }
    if (config.direction == Direction::Transmit && config.mode == Mode::Wbfm) {
        setError(error, "WDSP TX does not define a WBFM mode");
        return false;
    }
    // The TXA modulator settings open() pushes; a receive Config never uses them.
    if (config.direction == Direction::Transmit &&
        (!std::isfinite(config.txAmCarrierLevel) || config.txAmCarrierLevel < 0.0 ||
         config.txAmCarrierLevel > 1.0)) {
        setError(error, "WDSP TX AM carrier level must be within [0, 1]");
        return false;
    }
    if (config.direction == Direction::Transmit &&
        (!std::isfinite(config.txFmDeviationHz) ||
         config.txFmDeviationHz < Config::kMinFmDeviationHz ||
         config.txFmDeviationHz > Config::kMaxFmDeviationHz)) {
        setError(error,
            "WDSP TX FM deviation is outside Config::kMinFmDeviationHz..kMaxFmDeviationHz");
        return false;
    }
    // Receive only: filterTaps reaches WDSP solely through open()'s RXASetNC
    // and is read only by minimumNotchWidthHz(), both of which are RX-side. A
    // transmit channel has none of the six cores RXASetNC addresses, so
    // constraining it there would refuse geometries nothing can be hurt by.
    //
    // THIS IS THE SAME CLAUSE setFilterTaps() APPLIES, deliberately. Without it
    // here, create() and reconfigure() were a second door into exactly the
    // corruption the setter refuses -- see filterTapsArePartitionable() above
    // for the measured cost of walking through it.
    if (config.direction == Direction::Receive &&
        !filterTapsArePartitionable(config.filterTaps, config.dspBlockSize)) {
        setError(error,
                 "WDSP filter taps must be a power of two in [256, 16384] and "
                 "an exact multiple of the DSP block size");
        return false;
    }
    // Refused here as well as in setFmDeviation(), because open() pushes the
    // Config value straight into SetRXAFMDeviation, which divides by it — and
    // refused by RANGE, not by sign, for the reason on kMinFmDeviationHz.
    // Direction-gated like the WBFM refusal above it: open() pushes this on
    // the RXA path only, so a transmit Config never reaches the call and has
    // no business being failed by it.
    if (config.direction == Direction::Receive &&
        (!std::isfinite(config.fmDeviationHz) ||
         config.fmDeviationHz < Config::kMinFmDeviationHz ||
         config.fmDeviationHz > Config::kMaxFmDeviationHz)) {
        setError(error,
            "WDSP FM deviation is outside Config::kMinFmDeviationHz..kMaxFmDeviationHz");
        return false;
    }
    // Same door as setApf(), for the same reason as the deviation above: open()
    // pushes these straight into the peaking-filter design.
    if (config.direction == Direction::Receive &&
        !apfParametersValid(config.apfCenterHz, config.apfBandwidthHz, config.apfGain)) {
        setError(error, "WDSP APF centre, bandwidth and gain must be positive and finite");
        return false;
    }
    if (config.direction == Direction::Receive && !std::isfinite(config.agcFixedGainDb)) {
        setError(error, "WDSP AGC fixed gain must be finite");
        return false;
    }
    if (config.rxPanelGain && (config.direction != Direction::Receive
        || !std::isfinite(*config.rxPanelGain) || *config.rxPanelGain < 0.0)) {
        setError(error, "WDSP RX panel gain must be finite, non-negative and receive-only");
        return false;
    }
    return true;
}

int WdspChannel::wdspMode(Mode mode) noexcept
{
    return static_cast<int>(mode);
}

double WdspChannel::noiseBlankerThresholdForLevel(int level) noexcept
{
    const double clamped = std::clamp(static_cast<double>(level), 0.0, 100.0);
    // Geometric from 100 down to 4 — see the header. 0.04 is 4/100, so the
    // exponent form makes the endpoints readable: 100 * 0.04^0 = 100 and
    // 100 * 0.04^1 = 4, with the midpoint at 100 * 0.2 = 20.
    return 100.0 * std::pow(0.04, clamped / 100.0);
}

void WdspChannel::openNoiseBlanker() noexcept
{
    if (m_nbOpen || m_config.direction != Direction::Receive) {
        return;
    }
    // Staging buffers first: processIq() may run the moment the channel starts,
    // and it must never be the thing that sizes them.
    m_nbInterleaved.assign(2 * m_config.inputBlockSize, 0.0);
    m_nbI.assign(m_config.inputBlockSize, 0.0f);
    m_nbQ.assign(m_config.inputBlockSize, 0.0f);

    // Created whether or not the operator has one switched on, so that enabling
    // one later is a run-flag store rather than an allocation on a live channel.
    // Times are pihpsdr's and deskHPSDR's (receiver.c create_anbEXT /
    // create_nobEXT, identical in both); only the threshold is ours to move,
    // because only the threshold has a control above the seam.
    const int runAnb = m_config.noiseBlanker == NoiseBlanker::Impulse ? 1 : 0;
    const int runNob = m_config.noiseBlanker == NoiseBlanker::Advanced ? 1 : 0;
    const double threshold = noiseBlankerThresholdForLevel(m_config.noiseBlankerLevel);
    create_anbEXT(m_channelId,
                  runAnb,
                  static_cast<int>(m_config.inputBlockSize),
                  static_cast<double>(m_config.inputSampleRate),
                  0.0001,   // tau       — signal<->zero transition time
                  0.0001,   // hangtime  — hold at zero after the impulse
                  0.0001,   // advtime   — blank this far ahead of it
                  0.05,     // backtau   — averaging time for the trigger level
                  threshold);
    // Same numbers in the same order, with the fill mode NOB adds. Deliberately
    // the same: a switch between the two blankers is meant to change what fills
    // the window, not how the impulse is detected, so the detector's four times
    // and its trigger stay put.
    create_nobEXT(m_channelId,
                  runNob,
                  static_cast<int>(m_config.noiseBlankerFill),
                  static_cast<int>(m_config.inputBlockSize),
                  static_cast<double>(m_config.inputSampleRate),
                  0.0001,   // slewtime — nobII names tau this way
                  0.0001,   // hangtime
                  0.0001,   // advtime
                  0.05,     // backtau
                  threshold);
    m_nbOpen = true;
    m_nbKind.store(m_config.noiseBlanker, std::memory_order_relaxed);
    m_nbActive.store(m_config.noiseBlanker != NoiseBlanker::Off,
                     std::memory_order_relaxed);
}

void WdspChannel::closeNoiseBlanker() noexcept
{
    if (!m_nbOpen) {
        return;
    }
    m_nbActive.store(false, std::memory_order_relaxed);
    m_nbKind.store(NoiseBlanker::Off, std::memory_order_relaxed);
    destroy_anbEXT(m_channelId);
    destroy_nobEXT(m_channelId);
    m_nbOpen = false;
}

bool WdspChannel::setNoiseBlanker(NoiseBlanker kind, int level,
                                  NoiseBlankerFill fill) noexcept
{
    if (m_config.direction != Direction::Receive) {
        return false;
    }
    if (!beginControlOperation()) {
        return false;
    }
    m_config.noiseBlanker = kind;
    m_config.noiseBlankerLevel = std::clamp(level, 0, 100);
    m_config.noiseBlankerFill = fill;
    if (m_nbOpen) {
        const double threshold =
            noiseBlankerThresholdForLevel(m_config.noiseBlankerLevel);
        SetEXTANBThreshold(m_channelId, threshold);
        SetEXTNOBThreshold(m_channelId, threshold);
        SetEXTNOBMode(m_channelId, static_cast<int>(fill));
        // Flush BEFORE running, and only the stage being started. Whatever its
        // delay line holds is a fragment of the last enabled period, and on the
        // HL2 that can be a transmit-era gap; playing it out is an audible tick
        // at the exact moment the operator asked for less noise. The stage being
        // STOPPED is not flushed: it is about to stop being read, and flushing it
        // would only cost time on the control path.
        if (kind == NoiseBlanker::Impulse) {
            flush_anbEXT(m_channelId);
        } else if (kind == NoiseBlanker::Advanced) {
            flush_nobEXT(m_channelId);
        }
        // processIq feeds only the chosen stage, which is what keeps the two
        // exclusive; the run flags only follow the kind.
        SetEXTANBRun(m_channelId, kind == NoiseBlanker::Impulse ? 1 : 0);
        SetEXTNOBRun(m_channelId, kind == NoiseBlanker::Advanced ? 1 : 0);
    }
    m_nbKind.store(m_nbOpen ? kind : NoiseBlanker::Off, std::memory_order_relaxed);
    m_nbActive.store(kind != NoiseBlanker::Off && m_nbOpen,
                     std::memory_order_relaxed);
    endControlOperation();
    return true;
}

void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    // No beginControlOperation(): called from the processIq() thread on a TX
    // edge, where the handshake would deadlock. The atomic is read on the next
    // block. No flush is scheduled; the hold makes processIq skip the blanker so
    // its average survives TX. The only flush_anbEXT is in setNoiseBlanker, on
    // enable (#5499).
    m_nbHold.store(hold, std::memory_order_relaxed);
}

bool WdspChannel::open() noexcept
{
    const std::scoped_lock setupLock(g_setupMutex);
    loadWisdomOnce();   // import cached FFTW wisdom so PATIENT plans don't re-measure
    const bool preparedWbfm = m_config.wbfmReceive && !m_config.blockForOutput;
    if (openWdspChannel(m_channelId, m_config,
                        preparedWbfm ? std::optional<int>(kPreparedWbfmExchangeDepth)
                                     : std::nullopt) == 0) {
        return false;
    }
    if (m_config.direction == Direction::Receive) {
        SetRXAMode(m_channelId, wdspMode(m_config.mode));
        if (m_config.fmReceive) {
            // Upstream RX panel gain defaults to 4 and the FM limiter is off.
            // Together with FM de-emphasis this clips valid modulation at
            // unity monitor gain. Normalize inside the FM chain, including
            // pre-monitor taps. FM bypasses the main RX AGC.
            SetRXAPanelGain1(m_channelId, 1.0);
            SetRXAFMLimGain(m_channelId, 0.0);
            SetRXAFMLimRun(m_channelId, 1);
        }
        if (m_config.rxPanelGain) {  // a set field wins over the FM value above
            SetRXAPanelGain1(m_channelId, *m_config.rxPanelGain);
        }
        if (m_config.wbfmReceive) {
            m_wbfm = std::make_unique<WbfmState>(m_config);
            SetRXAWBFMDiscriminatorCompensation(m_channelId, 1);
            SetRXAWBFMForceMono(m_channelId, m_config.wbfmReceive->forceMono ? 1 : 0);
            SetRXAWBFMdmph(m_channelId, 1,
                m_config.wbfmReceive->deemphasis == WbfmReceive::Deemphasis::Us50 ? 1 : 0);
            m_wbfmStereo.store(0, std::memory_order_relaxed);
            m_wbfmReception.reset();
        } else {
            SetRXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
            RXANBPSetFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
        }
        applyRxAgc(m_channelId, m_config.agcMode, m_config.maximumAgcGainDb,
                   m_config.agcSlopeDb, m_config.agcFixedGainDb);
        // Filter length / phase mode. RXASetNC internally stops and restarts
        // the channel (SetChannelState 0 then restore), so it is control-path
        // work — safe here inside open(), never from processIq().
        RXASetNC(m_channelId, m_config.filterTaps);
        RXASetMP(m_channelId, m_config.minimumPhase ? 1 : 0);
        // The fmd stage is built by create_rxa with a hard 5000.0 and freed
        // again by close(), so this has to be re-pushed on every open or a
        // reconfigure() silently returns the operator to a 5 kHz assumption.
        SetRXAFMDeviation(m_channelId, m_config.fmDeviationHz);
        // Same reason again: create_rxa builds all three squelch stages with
        // run = 0 and close() frees them, so a reconfigure() would otherwise
        // open the operator's squelch without anything saying so.
        applySquelchLocked(m_config.mode);
        // close() frees the peaking stages, so the APF is re-pushed on every
        // open. The selection is stated, not inherited: everything here assumes
        // the double-pole (its centre divide, calc_dpole_nc sizing, the mode-2
        // I-into-Q copy that lets one positive centre serve CWL and CWU).
        SetRXASPCWSelection(m_channelId, kApfSelectionDoublePole);
        SetRXASPCWFreq(m_channelId, m_config.apfCenterHz);
        SetRXASPCWBandwidth(m_channelId, m_config.apfBandwidthHz);
        SetRXASPCWGain(m_channelId, m_config.apfGain);
        SetRXASPCWRun(m_channelId, m_config.apfEnabled ? 1 : 0);
    } else {
        SetTXAMode(m_channelId, wdspMode(m_config.mode));
        SetTXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
        // Pushed in every transmit mode: a later setMode() into AM or FM must
        // find them set, and SetTXAMode touches neither.
        SetTXAAMCarrierLevel(m_channelId, m_config.txAmCarrierLevel);
        SetTXAFMDeviation(m_channelId, m_config.txFmDeviationHz);
        // CTCSS encode off, unconditionally: create_txa() builds fmmod with
        // ctcss_run = 1 (100 Hz, level 0.10), and no host-modulated radio here
        // has a tone-encode control to carry a value in Config.
        SetTXACTCSSRun(m_channelId, 0);
    }
    // Cache what this open measured, right now, while we still hold the setup
    // lock -- a kill or a crash before exit must not throw the measurement away.
    // Bounded planner => rushed plans => do not publish them. See
    // plannerTimeLimitSeconds(): this cache is shared with the running app, so
    // a test run that exported would degrade the operator's next connect.
    if (!plannerIsBounded()) {
        exportWisdomNow();
        armWisdomExportOnce();   // and again at exit, for later setMode/setFilter plans
    }
    // Before the channel starts, so the first block through processIq() finds
    // its staging buffers sized and the stage already created.
    openNoiseBlanker();
    // Fully configured -- now run. dmode 0: nothing to flush on the way up.
    SetChannelState(m_channelId, 1, 0);
    m_running.store(true, std::memory_order_relaxed);
    m_open = true;
    return true;
}

void WdspChannel::close() noexcept
{
    if (!m_open) {
        return;
    }
    // Teardown, the only CloseChannel in the process. Stop first: closing a
    // running channel frees buffers under the mute ramp and skips the flush.
    // dmode 1 here: nothing clocks the channel behind the control fence, so the
    // wait hits WDSP's 100 ms timeout, whose branch force-clears exchange,
    // flushflag and slew.downflag as CloseChannel wants (dmode 0 skips that).
    // Already-stopped is benign: pre_main_destroy/build and destroy_iobuffs reset
    // them. Patch 9 runs the flushChannel handshake in pre_main_destroy
    // (runCloseAfterStoppedClockingTest). Outside g_setupMutex (per-channel state
    // only), so N closes don't serialise N timeouts.
    SetChannelState(m_channelId, 0, 1);
    m_running.store(false, std::memory_order_relaxed);
    {
        const std::scoped_lock setupLock(g_setupMutex);
        CloseChannel(m_channelId);
        // After the channel has stopped and drained: while it is still running
        // a callback can be inside processIq(), and destroying the stage under
        // one frees the delay line out from under xanb().
        closeNoiseBlanker();
        m_wbfm.reset();
        m_wbfmStereo.store(-1, std::memory_order_relaxed);
        m_wbfmReception.reset();
    }
    m_open = false;
}

bool WdspChannel::beginControlOperation() noexcept
{
    // Test hook — see refuseControlOperationsForTest().
    if (m_refuseControlForTest.load(std::memory_order_relaxed) != 0) {
        m_refuseControlForTest.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    bool expected = false;
    if (!m_controlOperation.compare_exchange_strong(expected, true,
                                                    std::memory_order_seq_cst)) {
        return false;
    }
    if (m_callbacksInFlight.load(std::memory_order_seq_cst) != 0) {
        m_controlOperation.store(false, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

void WdspChannel::endControlOperation() noexcept
{
    m_controlOperation.store(false, std::memory_order_seq_cst);
}
