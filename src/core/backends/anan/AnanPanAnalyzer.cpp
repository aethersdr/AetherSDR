#include "core/backends/anan/AnanPanAnalyzer.h"

#include "core/dsp/WdspChannel.h"

#include <aether_wdsp.h>

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

// One line per settings change (FFT AVG, FPS, rate). Off by default -- it fires
// on every slider step; enable with QT_LOGGING_RULES="aether.anan.pan.info=true".
Q_LOGGING_CATEGORY(lcAnanPan, "aether.anan.pan", QtWarningMsg)

namespace AetherSDR::anan {

namespace {

// WDSP analyzer constants (see aether_wdsp.h's analyzer block).
constexpr int kWindowKaiser = 5;
constexpr double kKaiserPiAlpha = 14.0;
constexpr int kDetectorAverage = 2;
constexpr int kAverageNone = 0;
constexpr int kAverageLinearRecursive = 1;
constexpr int kAverageLogRecursive = 3;
constexpr int kMaxDisplays = 72;   // dMAX_DISPLAYS
constexpr int kMaxAverage = 60;    // dMAX_AVERAGE

int nextPowerOfTwo(int n) noexcept
{
    int p = 1;
    while (p < n && p < AnanPanAnalyzer::kMaxFftSize)
        p <<= 1;
    return p;
}

}  // namespace

AnanPanAnalyzer::Derived AnanPanAnalyzer::derive(const Settings& s) noexcept
{
    Derived d;
    const int fps = std::max(1, s.framesPerSecond);
    const double rate = static_cast<double>(std::max(1, s.sampleRateHz));

    d.fftSize = std::max(kMinFftSize, nextPowerOfTwo(std::max(1, s.numPoints)));

    // One full FFT per display frame: each FFT advances by rate / fps fresh
    // samples and re-uses the rest of its window from the previous one. At a
    // high rate the advance exceeds the FFT, the overlap is zero, and the FFTs
    // simply tile the stream.
    d.overlap = static_cast<int>(
        std::max(0.0, std::ceil(static_cast<double>(d.fftSize) - rate / fps)));

    // How far the input may run ahead of the FFTs before the analyzer skips
    // samples: one FFT plus a tenth of a second of either input or FFT work,
    // whichever is less. Capped so it stays inside the 2 * kMaxFftSize ring.
    const double ahead = std::min(0.1 * rate, 0.1 * d.fftSize * fps);
    d.maxWriteahead = std::min(d.fftSize + static_cast<int>(ahead),
                               2 * kMaxFftSize - kBlockSize);

    // Recursive average with time constant averageTimeMs. WDSP applies the
    // weight once per FFT, not once per display frame, so the weight is
    // derived from the analyzer's real FFT rate, rate / (fftSize - overlap):
    // equal to fps while one FFT fits per frame (every rate up to 384 ksps at
    // 25 fps, where this reduces to deskHPSDR's exp(-1 / (fps * t))), and
    // higher once the overlap saturates at zero (768 and 1536 ksps), where
    // deskHPSDR's per-frame weight would deliver 1.9x / 3.75x less averaging
    // time than the control says. 0 = none.
    if (s.averageTimeMs > 0) {
        const double fftsPerSecond =
            rate / static_cast<double>(std::max(1, d.fftSize - d.overlap));
        const double framesPerTau = fftsPerSecond * (s.averageTimeMs / 1000.0);
        d.avBackmult = std::exp(-1.0 / framesPerTau);
        // Only WDSP's window-averaging mode (2) reads this, and this class never
        // selects it; set so the value is well-defined for the log line.
        d.numAverage = std::clamp(static_cast<int>(framesPerTau), 2, kMaxAverage);
    } else {
        d.avBackmult = 0.0;
        d.numAverage = 2;
    }
    return d;
}

std::unique_ptr<AnanPanAnalyzer> AnanPanAnalyzer::create(int disp, const Settings& settings,
                                                         std::string* error)
{
    if (disp < 0 || disp >= kMaxDisplays) {
        if (error) *error = "analyzer slot out of range";
        return nullptr;
    }
    if (settings.numPoints < 2 || settings.numPoints > AnanPanAnalyzer::kMaxPoints) {
        if (error) *error = "analyzer point count out of range";
        return nullptr;
    }

    int ok = -1;
    {
        auto lock = WdspChannel::fftwSetupLock();
        XCreateAnalyzer(disp, &ok, kMaxFftSize, 1, 1, nullptr);
    }
    if (ok != 0) {
        if (error) *error = "XCreateAnalyzer failed";
        return nullptr;
    }

    std::unique_ptr<AnanPanAnalyzer> a(new AnanPanAnalyzer(disp, settings));
    a->applySettings();
    return a;
}

AnanPanAnalyzer::AnanPanAnalyzer(int disp, const Settings& settings)
    : m_disp(disp), m_settings(settings)
{
    m_staged.assign(static_cast<std::size_t>(2 * kBlockSize), 0.0);
    m_scratch.assign(static_cast<std::size_t>(settings.numPoints), 0.0f);
}

AnanPanAnalyzer::~AnanPanAnalyzer()
{
    // DestroyAnalyzer() waits for the dispatcher thread but NOT for FFT worker
    // threads still in flight, then frees the buffers they use. SetAnalyzer()
    // does wait for them (and stops the dispatcher), so run it once as a drain
    // barrier. Same FFT size, so nothing is re-planned; no further Spectrum0()
    // call follows, so nothing restarts the dispatcher.
    const Derived d = derive(m_settings);
    int highSideLo = 0;
    auto lock = WdspChannel::fftwSetupLock();   // DestroyAnalyzer destroys FFTW plans
    SetAnalyzer(m_disp, 1, 1, 1, &highSideLo, d.fftSize, kBlockSize, kWindowKaiser,
                kKaiserPiAlpha, d.overlap, 0, 0.0, 0.0, m_settings.numPoints, 1, 0, 0.0,
                0.0, d.maxWriteahead);
    DestroyAnalyzer(m_disp);
}

void AnanPanAnalyzer::applySettings()
{
    const Derived d = derive(m_settings);
    m_fftSize = d.fftSize;
    int highSideLo = 0;
    {
        // Plans FFTW_PATIENT when the size changes, which it does only on the
        // first call after create().
        auto lock = WdspChannel::fftwSetupLock();
        SetAnalyzer(m_disp,
                    1,                  // one pixel output
                    1,                  // one FFT (no spur elimination)
                    1,                  // complex input
                    &highSideLo,
                    d.fftSize,
                    kBlockSize,
                    kWindowKaiser,
                    kKaiserPiAlpha,
                    d.overlap,
                    0,                  // no per-sub-span clip
                    0.0, 0.0,           // no whole-span clip -- the crop is the GUI's
                    m_settings.numPoints,
                    1,                  // one sub-span
                    0,                  // calibration set
                    0.0, 0.0,           // no frequency calibration
                    d.maxWriteahead);
    }
    SetDisplayDetectorMode(m_disp, 0, kDetectorAverage);
    applyAveraging();
    // Normalise to the bandwidth of one output point: tell the analyzer the
    // "sample rate" is the point count, so its one-hertz normalisation lands
    // on one point instead.
    SetDisplayNormOneHz(m_disp, 0, 1);
    SetDisplaySampleRate(m_disp, m_settings.numPoints);
}

void AnanPanAnalyzer::applyAveraging()
{
    const Derived d = derive(m_settings);
    m_avBackmult = d.avBackmult;
    // One line per settings change (FPS or the operator's averaging controls),
    // stating exactly what WDSP is given -- the only way to see from a bench
    // session what the analyzer is actually doing.
    qCInfo(lcAnanPan).nospace()
        << "analyzer slot " << m_disp << ": " << m_settings.sampleRateHz << " Hz, "
        << m_settings.numPoints << " points, FFT " << d.fftSize << ", overlap "
        << d.overlap << ", " << m_settings.framesPerSecond << " fps, average "
        << m_settings.averageTimeMs << " ms "
        << (m_settings.averageTimeMs <= 0 ? "(none)"
                                          : (m_settings.logAverage ? "(log)" : "(linear)"))
        << ", history weight " << d.avBackmult << (m_seeded ? "" : " (seeding)");
    SetDisplayNumAverage(m_disp, 0, d.numAverage);
    if (m_settings.averageTimeMs <= 0) {
        SetDisplayAverageMode(m_disp, 0, kAverageNone);
        return;
    }
    // Seed: with no weight on history the first frame IS the average. The
    // real weight goes in once that frame has been taken (takeFrame()) -- so
    // a frame the worker finished BEFORE this switch, still waiting in
    // GetPixels because takeFrame() only polls when a display frame is due,
    // must not be the one that counts: it was computed under the old mode,
    // and seeding from it would leave the new mode's fresh -160 dB history
    // in place and fade the panadapter in from black. Discard it first. (A
    // worker mid-FFT at this instant can still land one; that window is the
    // FFT's compute time, not a display interval.)
    if (!m_seeded) {
        int stale = 0;
        GetPixels(m_disp, 0, m_scratch.data(), &stale);
    }
    SetDisplayAvBackmult(m_disp, 0, m_seeded ? m_avBackmult : 0.0);
    SetDisplayAverageMode(m_disp, 0,
                          m_settings.logAverage ? kAverageLogRecursive : kAverageLinearRecursive);
}

void AnanPanAnalyzer::setAverageTimeMs(int ms)
{
    ms = std::max(0, ms);
    if (ms == m_settings.averageTimeMs)
        return;
    // Entering log-recursive mode resets WDSP's history to -160 dB, so switching
    // averaging on from none must re-seed; a change of time within the mode
    // keeps the history and only moves the weight.
    if (m_settings.averageTimeMs <= 0)
        m_seeded = false;
    m_settings.averageTimeMs = ms;
    applyAveraging();
}

void AnanPanAnalyzer::setLogAverage(bool on)
{
    if (on == m_settings.logAverage)
        return;
    m_settings.logAverage = on;
    // Switching WDSP's average mode resets its history (to -160 dB for log,
    // to ~0 power for linear), so the next frame must seed it.
    if (m_settings.averageTimeMs > 0)
        m_seeded = false;
    applyAveraging();
}

void AnanPanAnalyzer::setFramesPerSecond(int fps)
{
    if (fps <= 0 || fps == m_settings.framesPerSecond)
        return;
    m_settings.framesPerSecond = fps;
    // SetAnalyzer() resets the analyzer's input ring; drop our partial block
    // with it so the next block starts clean.
    m_stagedCount = 0;
    applySettings();
}

void AnanPanAnalyzer::setNumPoints(int points)
{
    points = std::clamp(points, 2, kMaxPoints);
    if (points == m_settings.numPoints)
        return;
    m_settings.numPoints = points;
    m_scratch.assign(static_cast<std::size_t>(points), 0.0f);
    // The averaging history is kept per point, in the old layout: seed from
    // the next frame instead of blending two different point grids.
    m_seeded = false;
    // SetAnalyzer() resets the input ring, as in setFramesPerSecond().
    m_stagedCount = 0;
    applySettings();
}

void AnanPanAnalyzer::feed(std::span<const std::complex<float>> iq)
{
    for (const auto& s : iq) {
        m_staged[2 * m_stagedCount] = static_cast<double>(s.real());
        m_staged[2 * m_stagedCount + 1] = static_cast<double>(s.imag());
        if (++m_stagedCount == static_cast<std::size_t>(kBlockSize)) {
            Spectrum0(1, m_disp, 0, 0, m_staged.data());
            m_stagedCount = 0;
        }
    }
}

std::size_t AnanPanAnalyzer::dropStagedPartial() noexcept
{
    const std::size_t dropped = m_stagedCount;
    m_stagedCount = 0;
    return dropped;
}

bool AnanPanAnalyzer::takeFrame(std::vector<float>& pointsDb)
{
    // GetPixels copies straight into its argument, so read into a scratch
    // buffer and leave `pointsDb` untouched when there is no new frame.
    int flag = 0;
    GetPixels(m_disp, 0, m_scratch.data(), &flag);
    if (flag == 0)
        return false;
    if (!m_seeded) {
        m_seeded = true;
        if (m_settings.averageTimeMs > 0)
            SetDisplayAvBackmult(m_disp, 0, m_avBackmult);
    }
    pointsDb.assign(m_scratch.begin(), m_scratch.end());
    return true;
}

}  // namespace AetherSDR::anan
