#include "core/dsp/WdspChannel.h"

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
#include <string>
#include <thread>

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

std::mutex g_channelMutex;
std::mutex g_setupMutex;
std::array<bool, kWdspChannelCount> g_channelsInUse {};

// WDSP builds its FFTs with FFTW_PATIENT (35 plans per channel), which re-runs
// exhaustive measurement on every OpenChannel — ~a minute per channel open.
// FFTW wisdom is global: prime it once from a persisted cache so those plans are
// imported instantly. First run still measures (and caches); later runs import.
// Called under g_setupMutex, so this global FFTW-planner I/O never races a
// concurrent OpenChannel. (WDSPwisdom() itself is Windows-console-only, so we use
// FFTW's portable wisdom API directly.)
std::string wisdomPath()
{
    namespace fs = std::filesystem;
    fs::path dir;
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

void loadWisdomOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] { fftw_import_wisdom_from_filename(wisdomPath().c_str()); });
}

// Write the wisdom cache NOW. Called at the end of open(), under g_setupMutex,
// where the expensive PATIENT plans have just been measured.
//
// EAGER, not only at exit, because at-exit persistence turned out to persist
// almost nothing. The process dies through Hl2EmergencyStop's handler, which
// restores SIG_DFL and re-raises so the kill looks exactly like an unhandled
// one — and an unhandled signal does not run std::atexit. Neither does a crash
// or a Force Quit. Measured: connect (21 s of FFTW planning), SIGTERM the app,
// relaunch, connect again — 21 s a SECOND time, with the cache file never
// created. Every operator who has ever force-quit was paying first-run cost on
// every run, which is what made a genuinely one-time expense feel permanent.
//
// A channel open is rare (connect, add panadapter, rate change) and this writes
// ~10-50 KB, so paying it per open is not a cost worth optimizing. It runs on
// the I/O thread, never the GUI thread.
//
// WRITE-THEN-RENAME, not a direct write, because this file is shared across
// PROCESSES and g_setupMutex only serialises threads within one of them. An
// app instance, a second app instance and the test suite (which ctest runs
// -j8) all export to the same path, and fftw_export_wisdom_to_filename opens
// with "w" — it truncates first, so two concurrent exporters leave a short
// file, and a reader importing mid-write gets a partial one that FFTW rejects
// WHOLESALE. Observed exactly that: a 48 KB cache came back as 13 KB after a
// parallel test run, which is a cache that silently stopped working.
//
// rename(2) is atomic within a filesystem, so a reader sees either the old
// complete file or the new complete file, and concurrent writers merely race
// to be last. The temp name carries the pid so two exporters never share it.
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
    std::call_once(flag, [] { std::atexit([] { exportWisdomNow(); }); });
}

int acquireChannelId()
{
    const std::scoped_lock lock(g_channelMutex);
    for (int channel = 0; channel < kWdspChannelCount; ++channel) {
        if (!g_channelsInUse[static_cast<std::size_t>(channel)]) {
            g_channelsInUse[static_cast<std::size_t>(channel)] = true;
            return channel;
        }
    }
    return -1;
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

} // namespace

namespace {

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
    if (GetWDSPVersion() != 200) {
        setError(error, "The linked WDSP library is not version 2.00");
        return nullptr;
    }

    const int channelId = acquireChannelId();
    if (channelId < 0) {
        setError(error, "All WDSP channel slots are in use");
        return nullptr;
    }

    std::unique_ptr<WdspChannel> channel(new (std::nothrow) WdspChannel(channelId, config));
    if (!channel) {
        releaseChannelId(channelId);
        setError(error, "Could not allocate the WDSP channel owner");
        return nullptr;
    }
    channel->open();
    return channel;
}

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

    const uint64_t allocationsBefore = wdspPortAllocationSequence();
    int wdspError = 0;

    // ── Noise blanker, ahead of the channel ───────────────────────────────
    //
    // Inside the allocation-guarded window deliberately: xanb() allocates
    // nothing today, and if a future WDSP snapshot changes that, this reports
    // AllocationViolation rather than letting a malloc land on the real-time
    // path unnoticed.
    const float* channelI = inputI.data();
    const float* channelQ = inputQ.data();
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
            // Transmitting. Skip the stage entirely — see setNoiseBlankerHold.
        } else {
            // DELIBERATELY NOT FLUSHED on the way out of a hold, and this is
            // the whole point of holding rather than muting. The stage was
            // SKIPPED while held, not fed, so its running average still holds
            // the pre-transmit signal level and it is already armed for the
            // first receive sample.
            //
            // Flushing here would reset that average to full scale, and at
            // backtau = 0.05 s nothing could exceed threshold * average for
            // ~200 ms — an unarmed blanker at the start of every receive
            // period, which is exactly the failure aether_wdsp.h's ARMING
            // DELAY note warns about and exactly the window this hold exists
            // to protect. Measured: with a flush here the blanked/unblanked
            // impulse peak ratio was 1.000 for the first 200 ms after each
            // transmit — bit-identical to the blanker being switched off —
            // against 0.43 once settled. Without it, 0.52 immediately.
            //
            // What the delay line carries across the hold is trans_count +
            // adv_count samples: 8 at tau/advtime = 0.0001 s, or 0.17 ms of
            // pre-transmit audio. That is not worth de-arming the stage for.
            const std::size_t n = inputI.size();
            for (std::size_t k = 0; k < n; ++k) {
                m_nbInterleaved[2 * k] = static_cast<double>(inputI[k]);
                m_nbInterleaved[2 * k + 1] = static_cast<double>(inputQ[k]);
            }
            // In place: ANB reads and writes the same buffer.
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
            for (std::size_t k = 0; k < n; ++k) {
                m_nbI[k] = static_cast<float>(m_nbInterleaved[2 * k]);
                m_nbQ[k] = static_cast<float>(m_nbInterleaved[2 * k + 1]);
            }
            channelI = m_nbI.data();
            channelQ = m_nbQ.data();
        }
    }

    fexchange2(m_channelId,
               const_cast<float*>(channelI),
               const_cast<float*>(channelQ),
               outputLeft.data(), outputRight.data(), &wdspError);
    const uint64_t allocationsAfter = wdspPortAllocationSequence();
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

bool WdspChannel::reconfigure(const Config& config, std::string* error) noexcept
{
    if (!validateConfig(config, error) || !beginControlOperation()) {
        if (error != nullptr && error->empty()) {
            *error = "WDSP channel is processing audio";
        }
        return false;
    }

    close();
    m_config = config;
    m_outputBlockSize = computeOutputBlockSize(m_config);
    open();
    endControlOperation();
    return true;
}

bool WdspChannel::setMode(Mode mode) noexcept
{
    if (!beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_config.direction == Direction::Receive) {
            SetRXAMode(m_channelId, wdspMode(mode));
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
        !beginControlOperation()) {
        return false;
    }
    {
        const std::scoped_lock setupLock(g_setupMutex);
        if (m_config.direction == Direction::Receive) {
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
    // WDSP's own min_notch_width() (nbp.c) for the window type RXA.c creates the
    // stage with (wintype 0). Mirrored here rather than exposed from WDSP
    // because the value is needed to *offer* widths, before any notch exists.
    if (m_config.direction != Direction::Receive || m_config.filterTaps <= 0
        || m_config.dspSampleRate <= 0) {
        return 0.0;
    }
    return 1600.0 / (static_cast<double>(m_config.filterTaps) / 256.0)
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

std::string WdspChannel::wisdomCachePathForTest()
{
    return wisdomPath();
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
    if (config.direction == Direction::Transmit && config.mode == Mode::Wbfm) {
        setError(error, "WDSP TX does not define a WBFM mode");
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

    // Created whether or not the operator has it switched on, so that enabling
    // it later is a run-flag store rather than an allocation on a live channel.
    // Times are pihpsdr's (receiver.c create_anbEXT); only the threshold is
    // ours to move, because only the threshold has a control above the seam.
    create_anbEXT(m_channelId,
                  m_config.noiseBlankerEnabled ? 1 : 0,
                  static_cast<int>(m_config.inputBlockSize),
                  static_cast<double>(m_config.inputSampleRate),
                  0.0001,   // tau       — signal<->zero transition time
                  0.0001,   // hangtime  — hold at zero after the impulse
                  0.0001,   // advtime   — blank this far ahead of it
                  0.05,     // backtau   — averaging time for the trigger level
                  noiseBlankerThresholdForLevel(m_config.noiseBlankerLevel));
    m_nbOpen = true;
    m_nbActive.store(m_config.noiseBlankerEnabled, std::memory_order_relaxed);
}

void WdspChannel::closeNoiseBlanker() noexcept
{
    if (!m_nbOpen) {
        return;
    }
    m_nbActive.store(false, std::memory_order_relaxed);
    destroy_anbEXT(m_channelId);
    m_nbOpen = false;
}

bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_config.direction != Direction::Receive) {
        return false;
    }
    if (!beginControlOperation()) {
        return false;
    }
    m_config.noiseBlankerEnabled = on;
    m_config.noiseBlankerLevel = std::clamp(level, 0, 100);
    if (m_nbOpen) {
        SetEXTANBThreshold(m_channelId,
                           noiseBlankerThresholdForLevel(m_config.noiseBlankerLevel));
        if (on) {
            // Flush BEFORE running. Whatever the delay line holds is a fragment
            // of the last enabled period, and on the HL2 that can be a
            // transmit-era gap; playing it out is an audible tick at the exact
            // moment the operator asked for less noise.
            flush_anbEXT(m_channelId);
        }
        SetEXTANBRun(m_channelId, on ? 1 : 0);
    }
    m_nbActive.store(on && m_nbOpen, std::memory_order_relaxed);
    endControlOperation();
    return true;
}

void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    // No beginControlOperation(): this is called from the same thread that
    // drives processIq() on a transmit edge, and taking the control handshake
    // there would deadlock against a callback in flight. Both stores are
    // atomic and the flush they schedule happens inside processIq() itself.
    m_nbHold.store(hold, std::memory_order_relaxed);
}

void WdspChannel::open() noexcept
{
    const std::scoped_lock setupLock(g_setupMutex);
    loadWisdomOnce();   // import cached FFTW wisdom so PATIENT plans don't re-measure
    OpenChannel(m_channelId,
                static_cast<int>(m_config.inputBlockSize),
                static_cast<int>(m_config.dspBlockSize),
                m_config.inputSampleRate,
                m_config.dspSampleRate,
                m_config.outputSampleRate,
                m_config.direction == Direction::Receive ? kRxChannelType : kTxChannelType,
                // Open STOPPED. Every reference client configures mode, filters
                // and AGC after OpenChannel, and opening in state 1 means any
                // samples arriving during that window are demodulated by a
                // default-configured channel -- wrong mode, wrong passband, AGC
                // wide open. SetChannelState below starts it once it is set up.
                0,
                m_config.muteDelayUpSec, m_config.muteSlewUpSec,
                m_config.muteDelayDownSec, m_config.muteSlewDownSec,
                m_config.blockForOutput ? 1 : 0);
    if (m_config.direction == Direction::Receive) {
        SetRXAMode(m_channelId, wdspMode(m_config.mode));
        SetRXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
        RXANBPSetFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
        applyRxAgc(m_channelId, m_config.agcMode, m_config.maximumAgcGainDb,
                   m_config.agcSlopeDb, m_config.agcFixedGainDb);
        // Filter length / phase mode. RXASetNC internally stops and restarts
        // the channel (SetChannelState 0 then restore), so it is control-path
        // work — safe here inside open(), never from processIq().
        RXASetNC(m_channelId, m_config.filterTaps);
        RXASetMP(m_channelId, m_config.minimumPhase ? 1 : 0);
    } else {
        SetTXAMode(m_channelId, wdspMode(m_config.mode));
        SetTXABandpassFreqs(m_channelId, m_config.filterLowHz, m_config.filterHighHz);
    }
    // Cache what this open measured, right now, while we still hold the setup
    // lock -- a kill or a crash before exit must not throw the measurement away.
    exportWisdomNow();
    armWisdomExportOnce();   // and again at exit, for later setMode/setFilter plans
    // Before the channel starts, so the first block through processIq() finds
    // its staging buffers sized and the stage already created.
    openNoiseBlanker();
    // Fully configured -- now run. dmode 0: nothing to flush on the way up.
    SetChannelState(m_channelId, 1, 0);
    m_open = true;
}

void WdspChannel::close() noexcept
{
    if (!m_open) {
        return;
    }
    const std::scoped_lock setupLock(g_setupMutex);
    // Stop and FLUSH before teardown (dmode 1 blocks until the channel has
    // drained, bounded by WDSP's own 100 ms timeout). CloseChannel on a running
    // channel frees buffers out from under the mute ramp and skips the flush
    // entirely, which is both a click on the way out and a race.
    SetChannelState(m_channelId, 0, 1);
    CloseChannel(m_channelId);
    // After the channel has stopped and drained: while it is still running a
    // callback can be inside processIq(), and destroying the stage under one
    // frees the delay line out from under xanb().
    closeNoiseBlanker();
    m_open = false;
}

bool WdspChannel::beginControlOperation() noexcept
{
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
