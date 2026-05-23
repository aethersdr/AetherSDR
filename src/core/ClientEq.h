#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace MasterSDR {

// Client-side parametric EQ. Runs inside AudioEngine for both the RX and
// TX audio paths — independent of the radio-side EQ applet, which sends
// commands to the radio's own DSP. Supports up to 16 simultaneous bands
// of peak, low/high shelf, low/high pass filters.
//
// Thread model: the UI thread writes parameters via set*() / setBand();
// the audio thread reads them via process(). Cross-thread synchronisation
// is via std::atomic — there are no locks. Coefficient recompute happens
// once per block on the audio thread; a per-band version counter lets the
// audio thread skip recompute when nothing has changed.
//
// Parameters are smoothed with a one-pole smoother per-block so slider
// motion during playback doesn't produce zipper noise. Smoothing time
// constant is fixed at ~15ms.
class ClientEq {
public:
    enum class FilterType : int {
        Peak       = 0,
        LowShelf   = 1,
        HighShelf  = 2,
        LowPass    = 3,
        HighPass   = 4,
    };

    // Global filter family — applies to the cascade math for HP/LP bands
    // (shelves and peaks always use their native 2nd-order topology).
    enum class FilterFamily : int {
        Butterworth = 0,   // maximally flat passband
        Chebyshev   = 1,   // steeper rolloff with 1 dB passband ripple
        Bessel      = 2,   // flat group delay (gentler rolloff)
        Elliptic    = 3,   // steepest rolloff, ripple in both bands
    };

    static constexpr int kMaxBands    = 16;
    static constexpr int kMaxSections = 4;  // up to 48 dB/oct HP/LP

    struct BandParams {
        float      freqHz{1000.0f};
        float      gainDb{0.0f};
        float      q{0.707f};
        FilterType type{FilterType::Peak};
        bool       enabled{true};
        // Slope in dB/octave for HP / LP — 12 / 24 / 36 / 48.  Ignored
        // for peak and shelf bands (they always use their native 2nd-
        // order topology).
        int        slopeDbPerOct{12};
    };

    ClientEq();
    ~ClientEq() = default;

    ClientEq(const ClientEq&)            = delete;
    ClientEq& operator=(const ClientEq&) = delete;

    // Main thread — call before first process() and on sample-rate change.
    void prepare(double sampleRate);

    // Main thread — global enable (bypass when false). Lock-free.
    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept;

    // Main thread — master output gain applied after all bands.  Linear
    // scale (1.0 = 0 dB).  Clamped to [0.0, 4.0] = [-inf, +12 dB].
    void setMasterGain(float linear) noexcept;
    float masterGain() const noexcept;

    // Global filter family for HP/LP cascade math.  Lock-free.
    void setFilterFamily(FilterFamily f) noexcept;
    FilterFamily filterFamily() const noexcept;

    // Main thread — set a single band's parameters. Triggers a recompute
    // on the audio thread at its next block. Lock-free.
    void setBand(int idx, const BandParams& p) noexcept;
    BandParams band(int idx) const noexcept;

    // Main thread — set how many of the kMaxBands slots are active.
    // Bands beyond this count are fully bypassed.
    void setActiveBandCount(int n) noexcept;
    int  activeBandCount() const noexcept;

    // Audio thread — process an interleaved float32 buffer in-place.
    // `channels` must be 1 or 2. `frames` is samples-per-channel.
    // RT-safe: no allocations, no locks, no exceptions.
    void process(float* interleaved, int frames, int channels) noexcept;

    // Audio thread — clear filter state (z1/z2) between discontinuous
    // bursts (e.g. transmit on/off). Leaves coefficients alone.
    void reset() noexcept;

    // Sample rate this EQ was prepared at. UI uses this when computing
    // the displayed magnitude response.
    double sampleRate() const noexcept { return m_sampleRate; }

    // Compute the analytic magnitude of one band at a probe frequency,
    // in dB, from its target parameters. Pure function, stateless, safe
    // to call from any thread. Used by the editor and applet to draw
    // the response curve without having to read the audio thread's
    // coefficients. A disabled band returns 0.0 (transparent).
    // The family-less overload assumes Butterworth for HP/LP cascades.
    static float bandMagnitudeDb(const BandParams& p,
                                 float probeFreqHz,
                                 double sampleRate) noexcept;
    static float bandMagnitudeDb(const BandParams& p,
                                 float probeFreqHz,
                                 double sampleRate,
                                 FilterFamily family) noexcept;

    // Default 10-band Logic-Pro-style layout: HP / LS / Peak×6 / HS / LP,
    // evenly log-spaced across 40 Hz .. 12 kHz.  All bands return with
    // enabled=false — the editor UI auto-enables a band the first time
    // the user interacts with it (drag handle, click icon, right-click).
    // Handles sit on the 0 dB line so the dots appear as a horizontal
    // row until the user shapes them.
    static constexpr int kDefaultBandCount = 10;
    static BandParams defaultBand(int idx);

private:
    struct Coeff {
        float b0{1.0f}, b1{0.0f}, b2{0.0f};
        float a1{0.0f}, a2{0.0f};
    };

    struct Smoothed {
        float freqHz{1000.0f};
        float gainDb{0.0f};
        float q{0.707f};
    };

    struct Section {
        Coeff coeff;
        float z1L{0.0f}, z2L{0.0f};
        float z1R{0.0f}, z2R{0.0f};
    };

    struct Runtime {
        std::array<Section, kMaxSections> sections;
        int        activeSections{1};
        Smoothed   current;      // post-smoothing values used for coeff
        uint64_t   lastVersion{0};
        FilterType cachedType{FilterType::Peak};
        bool       cachedEnabled{true};
        int        cachedSlopeDbPerOct{12};
        FilterFamily cachedFamily{FilterFamily::Butterworth};
    };

    struct AtomicBand {
        std::atomic<float>    freqHz{1000.0f};
        std::atomic<float>    gainDb{0.0f};
        std::atomic<float>    q{0.707f};
        std::atomic<int>      type{static_cast<int>(FilterType::Peak)};
        std::atomic<bool>     enabled{true};
        std::atomic<int>      slopeDbPerOct{12};
        std::atomic<uint64_t> version{0};
    };

    // Audio-thread helper: recompute `runtime.coeff` from the smoothed
    // `runtime.current` values, given `runtime.cachedType` and fs.
    void computeCoefficients(Runtime& runtime) noexcept;

    // Audio-thread helper: apply one-pole smoothing toward the target
    // parameters read from the atomic band. Returns true if anything
    // actually changed enough to warrant a coefficient recompute.
    bool smoothTowardTarget(int idx, Runtime& runtime,
                            const AtomicBand& target,
                            float smoothCoeff) noexcept;

    double             m_sampleRate{24000.0};
    float              m_smoothCoeff{0.0f};   // recomputed in prepare()
    std::atomic<bool>  m_enabled{false};
    std::atomic<int>   m_activeBandCount{0};
    std::atomic<float> m_masterGain{1.0f};    // post-band master output
    std::atomic<int>   m_filterFamily{static_cast<int>(FilterFamily::Butterworth)};

    std::array<AtomicBand, kMaxBands> m_bands;
    std::array<Runtime,    kMaxBands> m_runtime;
};

} // namespace MasterSDR
