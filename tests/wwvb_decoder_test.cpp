// WWVB legacy-AM decoder test (AetherClock WS-1).
//
// Self-contained: an in-file WWVB PWM synthesizer (per the pinned PRD-A §8
// signal levels), a deterministic AWGN helper, a BPSK-phase-flip variant, and
// a tiny std-only WAV writer. Exercises the frozen WwvbDecoder + TimeFrameVoter
// contract against the NIST WWVB legacy-AM field map (NIST SP 250-67, MSB-first
// weights, verified vs the frozen WS-1 spec table).
//
// Plain int main() + CHECK macro (NOT QtTest). No Qt. Only the two engine
// headers + std.

#include "core/WwvbDecoder.h"
#include "core/TimeFrameVoter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// ------------------------------------------------------------------ constants
constexpr int    kSR          = 24000;   // 24 kHz per the input contract
constexpr double kCarrierHz   = 1000.0;  // 60 kHz -> ~1000 Hz audio tone
constexpr double kCarrierAmp  = 0.5;     // full carrier amplitude (~ -6 dBFS)
constexpr double kPi          = 3.14159265358979323846;
constexpr double kWwvbDropDb  = 17.0;    // pinned power drop for a low-power bit
constexpr double kWwvbAgcDb   = 12.0;    // AGC-compression robustness variant
constexpr double kEnvSnrFloor = 16.0;    // pinned WWVB envelope-SNR floor (dB)

// Symbol codes used by the synth timeline (mirror ClockSymbol Zero/One/Marker).
constexpr int kSymZero = 0, kSymOne = 1, kSymMarker = 2;

// ------------------------------------------------------------- frame encoding
struct FrameTruth {
    int  min, hr, doy, yr;
    int  dut1Tenths;   // signed tenths of a second
    bool lyi;          // leap-year indicator (s55)
    bool lsw;          // leap-second warning (s56)
    int  dstCode;      // 2-bit DST code: bit "2" = s57 (dst1), bit "1" = s58 (dst2)
};

// Encode one 60 s WWVB frame to per-second symbols (0/1/marker). MSB-first BCD
// per the NIST WWVB time-code table; unused seconds stay binary 0.
static std::array<int, 60> encodeFrame(const FrameTruth& t) {
    std::array<int, 60> s;
    s.fill(kSymZero);
    for (int m : {0, 9, 19, 29, 39, 49, 59}) s[m] = kSymMarker;

    // Set a decimal digit (0..9) into the seconds carrying binary weights 8,4,2,1
    // (a second index of -1 means that weight is absent from the field).
    auto setBits = [&](int value, const std::array<int, 4>& secs) {
        const int w[4] = {8, 4, 2, 1};
        for (int i = 0; i < 4; ++i)
            if (secs[i] >= 0) s[secs[i]] = (value & w[i]) ? kSymOne : kSymZero;
    };

    // MIN: (1,40)(2,20)(3,10) tens; (5,8)(6,4)(7,2)(8,1) units
    setBits(t.min / 10, {-1, 1, 2, 3});
    setBits(t.min % 10, {5, 6, 7, 8});
    // HR: (12,20)(13,10) tens; (15,8)(16,4)(17,2)(18,1) units
    setBits(t.hr / 10, {-1, -1, 12, 13});
    setBits(t.hr % 10, {15, 16, 17, 18});
    // DOY: (22,200)(23,100) hundreds; (25,80)(26,40)(27,20)(28,10) tens; (30..33) units
    setBits(t.doy / 100, {-1, -1, 22, 23});
    setBits((t.doy / 10) % 10, {25, 26, 27, 28});
    setBits(t.doy % 10, {30, 31, 32, 33});
    // DUT1 sign: s36=+ s37=- s38=+  (positive -> 36 AND 38; negative -> 37)
    if (t.dut1Tenths >= 0) { s[36] = kSymOne; s[37] = kSymZero; s[38] = kSymOne; }
    else                   { s[36] = kSymZero; s[37] = kSymOne; s[38] = kSymZero; }
    // DUT1 magnitude: (40,0.8)(41,0.4)(42,0.2)(43,0.1)
    setBits(std::abs(t.dut1Tenths), {40, 41, 42, 43});
    // YR: (45,80)(46,40)(47,20)(48,10) tens; (50,8)(51,4)(52,2)(53,1) units
    setBits(t.yr / 10, {45, 46, 47, 48});
    setBits(t.yr % 10, {50, 51, 52, 53});
    // LYI s55, LSW s56, DST code s57/s58
    s[55] = t.lyi ? kSymOne : kSymZero;
    s[56] = t.lsw ? kSymOne : kSymZero;
    s[57] = (t.dstCode >> 1) & 1 ? kSymOne : kSymZero;
    s[58] = (t.dstCode & 1) ? kSymOne : kSymZero;
    return s;
}

// Build a run of `nFrames` consecutive minutes, with `leadTail` seconds of the
// previous minute in front (acquisition runway + the s59->s0 double marker at
// the first frame boundary) and `leadOut` seconds of the following minute after
// (so the last full frame's closing double marker exists and it can emit).
static std::vector<int> buildTimeline(const FrameTruth& base, int nFrames,
                                      int leadTail, int leadOut) {
    std::vector<int> secs;
    FrameTruth prev = base; prev.min = base.min - 1;
    auto psym = encodeFrame(prev);
    for (int s = 60 - leadTail; s < 60; ++s) secs.push_back(psym[s]);
    for (int fi = 0; fi < nFrames; ++fi) {
        FrameTruth ft = base; ft.min = base.min + fi;
        auto sm = encodeFrame(ft);
        for (int s = 0; s < 60; ++s) secs.push_back(sm[s]);
    }
    if (leadOut > 0) {
        FrameTruth nxt = base; nxt.min = base.min + nFrames;
        auto nsym = encodeFrame(nxt);
        for (int s = 0; s < leadOut; ++s) secs.push_back(nsym[s]);
    }
    return secs;
}

// ------------------------------------------------------------------ synthesis
// Low-power window duration (seconds) for a symbol: 0.2 s = 0, 0.5 s = 1, 0.8 s = marker.
static double symbolLowSeconds(int sym) {
    return sym == kSymZero ? 0.2 : sym == kSymOne ? 0.5 : 0.8;
}

// Render the symbol timeline to 24 kHz float mono. The carrier is a continuous
// 1000 Hz tone at kCarrierAmp; amplitude drops `dropDb` at each second start for
// the encoded duration, then returns to full (dropDb == 0 => pure carrier tone,
// no PWM). `bpsk` adds a 180-degree carrier phase flip starting +100 ms after
// each 1-bit second edge, WITHOUT touching the amplitude envelope (INV-8).
// `sigma` > 0 adds deterministic AWGN.
static std::vector<float> synth(const std::vector<int>& secs, double dropDb,
                                bool bpsk, double sigma, std::uint64_t seed) {
    const double drop = std::pow(10.0, -dropDb / 20.0);
    const int flipStart = static_cast<int>(std::lround(0.100 * kSR));  // +100 ms
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> noise(0.0, sigma > 0.0 ? sigma : 1.0);

    std::vector<float> out;
    out.reserve(secs.size() * static_cast<std::size_t>(kSR));
    long long n = 0;
    for (int sym : secs) {
        const int lowSamples = static_cast<int>(std::lround(symbolLowSeconds(sym) * kSR));
        for (int j = 0; j < kSR; ++j) {
            const double amp = (j < lowSamples) ? drop : 1.0;
            const double t = static_cast<double>(n) / kSR;
            double carrier = std::sin(2.0 * kPi * kCarrierHz * t);
            if (bpsk && sym == kSymOne && j >= flipStart) carrier = -carrier;
            double v = amp * kCarrierAmp * carrier;
            if (sigma > 0.0) v += noise(rng);
            out.push_back(static_cast<float>(v));
            ++n;
        }
    }
    return out;
}

// Pure AWGN, no carrier.
static std::vector<float> noiseVector(std::size_t nSamples, double sigma, std::uint64_t seed) {
    std::vector<float> out(nSamples);
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0.0, sigma);
    for (auto& v : out) v = static_cast<float>(nd(rng));
    return out;
}

// Envelope-SNR (dB) definition consistent with the pinned 16 dB floor: full
// carrier RMS over the time-domain noise RMS.
static double envelopeSnrDb(double carrierAmp, double sigma) {
    const double carrierRms = carrierAmp / std::sqrt(2.0);
    return 20.0 * std::log10(carrierRms / sigma);
}

// ------------------------------------------------------------------ WAV writer
// Minimal std-only WAV: 16-bit PCM, stereo (duplicated mono), 24 kHz.
static void writeWavStereo16(const std::string& path, const std::vector<float>& mono, int sr) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const std::uint16_t channels = 2, bits = 16;
    const std::uint16_t blockAlign = channels * (bits / 8);
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sr) * blockAlign;
    const std::uint32_t nFrames = static_cast<std::uint32_t>(mono.size());
    const std::uint32_t dataBytes = nFrames * blockAlign;
    const std::uint32_t riff = 36 + dataBytes;
    auto w32 = [&](std::uint32_t v) {
        char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        f.write(b, 4);
    };
    auto w16 = [&](std::uint16_t v) {
        char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        f.write(b, 2);
    };
    f.write("RIFF", 4); w32(riff); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(channels);
    w32(static_cast<std::uint32_t>(sr)); w32(byteRate); w16(blockAlign); w16(bits);
    f.write("data", 4); w32(dataBytes);
    for (float s : mono) {
        double c = std::max(-1.0, std::min(1.0, static_cast<double>(s)));
        const std::int16_t v = static_cast<std::int16_t>(std::lround(c * 32767.0));
        w16(static_cast<std::uint16_t>(v));  // L
        w16(static_cast<std::uint16_t>(v));  // R (duplicated mono)
    }
}

// ---------------------------------------------------------------- capture/feed
struct Capture {
    std::vector<ClockFrameInfo> frames;
    std::vector<ClockTimeInfo>  times;
    std::vector<ClockLockState> states;
    long secondCount = 0;
    ClockLockState finalState = ClockLockState::NoSignal;
    ClockStation   finalStation = ClockStation::Unknown;
};

static void wire(WwvbDecoder& d, Capture& c) {
    d.onSecond       = [&](const ClockSecondInfo&) { ++c.secondCount; };
    d.onFrame        = [&](const ClockFrameInfo& f) { c.frames.push_back(f); };
    d.onTime         = [&](const ClockTimeInfo& t) { c.times.push_back(t); };
    d.onStateChanged = [&](ClockLockState s) { c.states.push_back(s); };
}

static void feedFixed(WwvbDecoder& d, const std::vector<float>& x, std::size_t chunk) {
    for (std::size_t i = 0; i < x.size(); i += chunk) {
        const std::size_t c = std::min(chunk, x.size() - i);
        d.process(x.data() + i, c);
    }
}

// Deliberately irregular chunk sizes to exercise the streaming accumulator.
static void feedIrregular(WwvbDecoder& d, const std::vector<float>& x) {
    static const std::size_t sizes[] = {1, 7, 64, 999, 4096, 24000, 333, 2, 17000};
    const std::size_t nSizes = sizeof(sizes) / sizeof(sizes[0]);
    std::size_t i = 0, k = 0;
    while (i < x.size()) {
        std::size_t c = sizes[k % nSizes];
        if (i + c > x.size()) c = x.size() - i;
        d.process(x.data() + i, c);
        i += c; ++k;
    }
}

static Capture runFixed(const std::vector<float>& x) {
    WwvbDecoder d; Capture c; wire(d, c);
    feedFixed(d, x, 8192);
    c.finalState = d.state(); c.finalStation = d.station();
    return c;
}

static Capture runIrregular(const std::vector<float>& x) {
    WwvbDecoder d; Capture c; wire(d, c);
    feedIrregular(d, x);
    c.finalState = d.state(); c.finalStation = d.station();
    return c;
}

// --------------------------------------------------------------- shared checks
static void checkVoted(const Capture& c, int min, int hr, int doy, int yr, const char* tag) {
    CHECK(!c.times.empty());
    if (c.times.empty()) { (void)tag; return; }
    const ClockTimeInfo& tv = c.times.back();
    CHECK(tv.minute == min);
    CHECK(tv.hour   == hr);
    CHECK(tv.doy    == doy);
    CHECK(tv.year2  == yr);
    CHECK(tv.station == ClockStation::Wwvb);
    (void)tag;
}

// Per-frame auxiliary fields (not voted): DUT1 sign+magnitude, DST code, LYI, LSW.
static void checkAuxFields(const Capture& c, const FrameTruth& truth, int nFrames) {
    int matched = 0;
    for (const auto& f : c.frames) {
        if (f.minute < truth.min || f.minute > truth.min + nFrames - 1) continue;
        ++matched;
        CHECK(f.station == ClockStation::Wwvb);
        CHECK(f.dut1Tenths == truth.dut1Tenths);
        CHECK(f.dst1 == (((truth.dstCode >> 1) & 1) != 0));
        CHECK(f.dst2 == ((truth.dstCode & 1) != 0));
        CHECK(f.leapYear == truth.lyi);
        CHECK(f.leapPending == truth.lsw);
    }
    CHECK(matched >= nFrames);
}

// ---------------------------------------------- direct TimeFrameVoter rollover
// Drive TimeFrameVoter directly with the NIST WWVB legacy-AM field map so the
// whole-timestamp value vote is exercised across hour/day/year boundaries — the
// case the golden audio vectors never reached (all stayed within one hour).

using VMap = std::vector<std::pair<int, int>>;  // (second, weight)
// WWVB field maps derived from encodeFrame's setBits placement (NIST SP 250-67).
static const VMap kVbMin{{1,40},{2,20},{3,10},{5,8},{6,4},{7,2},{8,1}};
static const VMap kVbHr {{12,20},{13,10},{15,8},{16,4},{17,2},{18,1}};
static const VMap kVbDoy{{22,200},{23,100},{25,80},{26,40},{27,20},{28,10},
                         {30,8},{31,4},{32,2},{33,1}};
static const VMap kVbYr {{45,80},{46,40},{47,20},{48,10},{50,8},{51,4},{52,2},{53,1}};

static TimeFrameVoter::Config wwvbVoterConfig() {
    auto toFieldMap = [](const VMap& m) {
        ClockFieldMap fm;
        for (const auto& pr : m) fm.push_back(ClockBitWeight{pr.first, pr.second});
        return fm;
    };
    TimeFrameVoter::Config cfg;
    cfg.fields[TimeFrameVoter::FieldMinutes] = toFieldMap(kVbMin);
    cfg.fields[TimeFrameVoter::FieldHours]   = toFieldMap(kVbHr);
    cfg.fields[TimeFrameVoter::FieldDoy]     = toFieldMap(kVbDoy);
    cfg.fields[TimeFrameVoter::FieldYear]    = toFieldMap(kVbYr);
    cfg.markerSeconds = {0, 9, 19, 29, 39, 49, 59};
    cfg.window = 8;
    cfg.minFramesForLock = 2;
    cfg.agingFactor = 0.9f;
    return cfg;
}

static std::array<ClockSymbol, 60> wwvbVoterFrame(int mn, int hr, int doy, int yr) {
    const FrameTruth t{mn, hr, doy, yr, /*dut1*/0, /*lyi*/false, /*lsw*/false, /*dst*/0};
    const std::array<int, 60> s = encodeFrame(t);
    std::array<ClockSymbol, 60> out{};
    for (int i = 0; i < 60; ++i) {
        out[i] = (s[i] == kSymOne)    ? ClockSymbol::One
               : (s[i] == kSymMarker) ? ClockSymbol::Marker
                                      : ClockSymbol::Zero;
    }
    return out;
}

// [wwvb.voter.rollover] Whole-timestamp value vote survives hour/day/year
// rollovers; a mixed-hour window never votes an off-air hour.
static void sectionVoterRollover() {
    std::array<float, 60> conf; conf.fill(1.0f);

    // (a) advanceMinutes helper is calendar-correct in isolation.
    CHECK(advanceMinutes(TimeFields{59, 21, 200, 26}, 1) == (TimeFields{0, 22, 200, 26}));
    CHECK(advanceMinutes(TimeFields{59, 23, 200, 26}, 1) == (TimeFields{0, 0, 201, 26}));
    CHECK(advanceMinutes(TimeFields{59, 23, 365, 26}, 1) == (TimeFields{0, 0, 1, 27}));
    CHECK(advanceMinutes(TimeFields{59, 23, 366, 28}, 1) == (TimeFields{0, 0, 1, 29}));

    // (b) hour rollover 21:57 -> 22:02.
    {
        TimeFrameVoter v(wwvbVoterConfig());
        struct MH { int mn, hr; };
        for (const MH e : {MH{57,21}, {58,21}, {59,21}, {0,22}, {1,22}, {2,22}}) {
            v.addFrame(wwvbVoterFrame(e.mn, e.hr, 200, 26), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == e.mn);
            CHECK(v.votedField(TimeFrameVoter::FieldHours)   == e.hr);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
            CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);
        }
    }

    // (c) day rollover 23:58 -> 00:01, doy 200 -> 201.
    {
        TimeFrameVoter v(wwvbVoterConfig());
        struct MHD { int mn, hr, doy; };
        for (const MHD e : {MHD{58,23,200}, {59,23,200}, {0,0,201}, {1,0,201}}) {
            v.addFrame(wwvbVoterFrame(e.mn, e.hr, e.doy, 26), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == e.mn);
            CHECK(v.votedField(TimeFrameVoter::FieldHours)   == e.hr);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == e.doy);
        }
    }

    // (d) year rollover on doy 365 -> 1, yr 26 -> 27 (non-leap year length).
    {
        TimeFrameVoter v(wwvbVoterConfig());
        struct T { int mn, hr, doy, yr; };
        for (const T e : {T{58,23,365,26}, {59,23,365,26}, {0,0,1,27}, {1,0,1,27}}) {
            v.addFrame(wwvbVoterFrame(e.mn, e.hr, e.doy, e.yr), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)  == e.doy);
            CHECK(v.votedField(TimeFrameVoter::FieldYear) == e.yr);
        }
    }

    // (e) bizarre-blend regression: 7 consecutive hour-21 frames then hour-22 —
    //     voted hour must only ever be 21 or 22, never a per-bit-mixed value.
    {
        TimeFrameVoter v(wwvbVoterConfig());
        struct MH { int mn, hr; };
        for (const MH e : {MH{53,21}, {54,21}, {55,21}, {56,21}, {57,21}, {58,21},
                           {59,21}, {0,22}, {1,22}, {2,22}}) {
            v.addFrame(wwvbVoterFrame(e.mn, e.hr, 200, 26), conf);
            const int h = v.votedField(TimeFrameVoter::FieldHours);
            CHECK(h == 21 || h == 22);
            CHECK(h == e.hr);
        }
    }
}

// One voter frame, then per-second corruptions injected at chosen confidences:
// each Corrupt overrides a second's symbol and its matched-filter margin, so the
// vector can shape single-bit fades (low margin) and confident misreads (high
// margin) the way the live receiver saw them.
struct Corrupt { int second; ClockSymbol symbol; float confidence; };
static std::pair<std::array<ClockSymbol, 60>, std::array<float, 60>>
wwvbNoisyFrame(int mn, int hr, int doy, int yr, float baseConf,
               const std::vector<Corrupt>& corruptions) {
    std::array<ClockSymbol, 60> syms = wwvbVoterFrame(mn, hr, doy, yr);
    std::array<float, 60> conf;
    conf.fill(baseConf);
    for (const Corrupt& c : corruptions) {
        syms[c.second] = c.symbol;
        conf[c.second] = c.confidence;
    }
    return {syms, conf};
}

// [wwvb.voter.noisycorpus] Mirror of the WWV WS-3 exact-tuple degeneracy on the
// WWVB legacy-AM map: an 8-frame window inside one hour (06:18..06:25, doy 200,
// yr 26) where every frame is corrupt in a different field, so no whole-tuple
// key repeats. The old whole-tuple vote degenerates to the newest frame's
// corrupt tuple (yr=66, the year 25 off) at a high, honest-looking static
// quality. Normalize-then-per-bit recovers the true timestamp from the per-field
// majorities and reports a quality that finally reflects the contested vote.
static void sectionVoterNoisyCorpus() {
    constexpr float C  = 0.35f;   // clean-bit margin (the live corpus ran ~0.35..0.41)
    constexpr float LO = 0.05f;   // faded minute bit — a low-margin miss
    constexpr float HI = 0.90f;   // a confident misread — high margin, wrong bit

    TimeFrameVoter v(wwvbVoterConfig());

    // Frames oldest -> newest. The three oldest fade a minutes tens bit at LOW
    // margin; the five newest are minutes-clean but each carries one confident
    // wrong static bit — so NO frame decodes a fully-correct tuple.
    struct NF { int mn, hr, doy, yr; std::vector<Corrupt> corr; };
    const NF frames[] = {
        {18, 6, 200, 26, {{3,  ClockSymbol::Zero, LO}}}, // drop min 10-bit -> 08
        {19, 6, 200, 26, {{3,  ClockSymbol::Zero, LO}}}, // -> 09
        {20, 6, 200, 26, {{2,  ClockSymbol::Zero, LO}}}, // drop min 20-bit -> 00
        {21, 6, 200, 26, {{32, ClockSymbol::One,  HI}}}, // set doy 2-bit -> 202
        {22, 6, 200, 26, {{33, ClockSymbol::One,  HI}}}, // set doy 1-bit -> 201
        {23, 6, 200, 26, {{47, ClockSymbol::Zero, HI}}}, // drop yr 20-bit -> 06
        {24, 6, 200, 26, {{16, ClockSymbol::Zero, HI}}}, // drop hr 4-bit -> 02
        {25, 6, 200, 26, {{46, ClockSymbol::One,  HI}}}, // set yr 40-bit -> 66 (newest)
    };
    for (const NF& f : frames) {
        const auto [syms, conf] = wwvbNoisyFrame(f.mn, f.hr, f.doy, f.yr, C, f.corr);
        v.addFrame(syms, conf);
    }

    // Normalize-then-per-bit recovers the TRUE timestamp as of the newest frame,
    // even though the newest frame's own decode carries yr=66 and no tuple repeats.
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);
    CHECK(v.lastFrameMinute() == 25);   // newest raw frame really is minute 25/yr 66

    // Quality is coupled to the voted space: lockConfidence is the MINIMUM
    // per-bit margin, so this window's several contested bits collapse it far out
    // of the clean-lock band — measured ~0.292 — while the value is recovered.
    const float q = v.lockConfidence();
    CHECK(q > 0.0f && q <= 1.0f);
    CHECK(q < 0.35f);

    // A clean 8-frame window over the same minutes votes the same timestamp at
    // strictly higher quality — the dip tracks the contention, not luck.
    TimeFrameVoter clean(wwvbVoterConfig());
    std::array<float, 60> cleanConf;
    cleanConf.fill(C);
    for (int mn = 18; mn <= 25; ++mn)
        clean.addFrame(wwvbVoterFrame(mn, 6, 200, 26), cleanConf);
    CHECK(clean.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(clean.votedField(TimeFrameVoter::FieldYear)    == 26);
    CHECK(clean.lockConfidence() > q);
}

// [wwvb.voter.minutefade] Mirror guard for the re-encoded-field weighting: five
// newest frames fade the minutes 20 bit (sec 2 here) over three older clean
// frames. Field-MEAN weighting of the re-encoded minute votes the wrong minute
// (05); field-MIN weighting lets the three clean frames carry the 20 bit so the
// true minute (25) survives.
static void sectionVoterMinuteFadeWeighting() {
    constexpr float C  = 0.40f;   // clean-bit margin
    constexpr float LO = 0.05f;   // faded minutes-20 bit — a low-margin miss

    TimeFrameVoter v(wwvbVoterConfig());
    struct NF { int mn; std::vector<Corrupt> corr; };
    const NF frames[] = {
        {18, {}}, {19, {}}, {20, {}},
        {21, {{2, ClockSymbol::Zero, LO}}}, // 21 has the 20 bit -> raw 01
        {22, {{2, ClockSymbol::Zero, LO}}}, // -> raw 02
        {23, {{2, ClockSymbol::Zero, LO}}}, // -> raw 03
        {24, {{2, ClockSymbol::Zero, LO}}}, // -> raw 04
        {25, {{2, ClockSymbol::Zero, LO}}}, // newest -> raw 05
    };
    for (const NF& f : frames) {
        const auto [syms, conf] = wwvbNoisyFrame(f.mn, 6, 200, 26, C, f.corr);
        v.addFrame(syms, conf);
    }
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);
}

// [wwvb.voter.contested-bit-quality] Mirror: the year-40 bit is flipped in the
// four newest frames (raw year 26 -> 66); votedField votes 66 on recency, but a
// single bit decided the field, so lockConfidence must collapse to that bit's
// normalized margin (~0.208) rather than average it into the clean-lock band
// (~0.974 under a mean over 31 bits).
static void sectionVoterContestedBitQuality() {
    std::array<float, 60> conf;
    conf.fill(0.40f);

    TimeFrameVoter v(wwvbVoterConfig());
    for (int mn = 18; mn <= 21; ++mn) v.addFrame(wwvbVoterFrame(mn, 6, 200, 26), conf);
    for (int mn = 22; mn <= 25; ++mn) v.addFrame(wwvbVoterFrame(mn, 6, 200, 66), conf);

    CHECK(v.locked());
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 66);
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.35f);
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
}

// [wwvb.voter.doy366gate] Mirror: a confident newest frame decodes doy 366 in yr
// 27 (2027 is not a leap year). advanceMinutes would wrap doy 366 into Jan 1 of
// yr 28 (even at age 0), so the gate excludes it and the two clean frames hold
// the vote at doy 200 / yr 27; without the gate the vote becomes doy 1 / yr 28.
static void sectionVoterDoy366Gate() {
    constexpr float C  = 0.35f;
    constexpr float HI = 0.90f;
    std::array<float, 60> cc;  cc.fill(C);
    std::array<float, 60> chi; chi.fill(HI);

    TimeFrameVoter v(wwvbVoterConfig());
    v.addFrame(wwvbVoterFrame(23, 6, 200, 27), cc);
    v.addFrame(wwvbVoterFrame(24, 6, 200, 27), cc);
    v.addFrame(wwvbVoterFrame(25, 6, 366, 27), chi);  // doy 366 in a non-leap year

    CHECK(v.votedField(TimeFrameVoter::FieldDoy)  == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 27);
}

// [wwvb.voter.frankenstein] Mirror: three clean hour-2 frames with the 4-bit
// (sec 16) faded (sec17 One@0.90, sec16 Zero@0.05) and one confident hour-4
// newest (sec17 Zero@0.05, sec16 One@0.90). Per-bit would compose 2+4 = 6;
// coherence gates sec16 (confidence-winner One vs count-majority Zero) so the
// hour falls back to the held majority 2, at the weak held-value margin (~0.42).
static void sectionVoterFrankenstein() {
    constexpr float LO = 0.05f;
    constexpr float HI = 0.90f;
    constexpr float C  = 0.40f;

    TimeFrameVoter v(wwvbVoterConfig());
    for (int mn = 18; mn <= 20; ++mn) {
        const auto [syms, conf] = wwvbNoisyFrame(
            mn, 2, 200, 26, C,
            {{17, ClockSymbol::One, HI}, {16, ClockSymbol::Zero, LO}});
        v.addFrame(syms, conf);
    }
    {
        const auto [syms, conf] = wwvbNoisyFrame(
            21, 4, 200, 26, C,
            {{17, ClockSymbol::Zero, LO}, {16, ClockSymbol::One, HI}});
        v.addFrame(syms, conf);
    }

    CHECK(v.locked());
    CHECK(v.votedField(TimeFrameVoter::FieldHours) == 2);   // held majority, NOT 6
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 21);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)   == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)  == 26);
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.45f);
}

// [wwvb.voter.lone-voter-participation] Mirror: sec 45 (year-80 bit) is Unknown
// in three of four otherwise-clean frames; only the newest classifies it. Its
// margin is ~1.0 but participation ~0.291 caps its trust, pulling quality down
// while the value stays year 26.
static void sectionVoterLoneVoterParticipation() {
    constexpr float C = 0.40f;

    TimeFrameVoter v(wwvbVoterConfig());
    for (int mn = 18; mn <= 20; ++mn) {
        const auto [syms, conf] =
            wwvbNoisyFrame(mn, 6, 200, 26, C, {{45, ClockSymbol::Unknown, C}});
        v.addFrame(syms, conf);
    }
    std::array<float, 60> newestConf;
    newestConf.fill(C);
    v.addFrame(wwvbVoterFrame(21, 6, 200, 26), newestConf);

    CHECK(v.locked());
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 26);
    CHECK(v.votedField(TimeFrameVoter::FieldHours) == 6);
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.35f);
}

// ------------------------------------------------------------------------ main
int main() {
    // Golden truth for the clean 3-frame vector: 06:20/06:21/06:22, doy 200,
    // yr 26; DUT1 = -0.3 s (negative sign path + magnitude 3), LYI set, LSW
    // clear, DST code = 10 (begins today -> dst1 set, dst2 clear).
    const FrameTruth kBase = {/*min*/20, /*hr*/6, /*doy*/200, /*yr*/26,
                              /*dut1Tenths*/-3, /*lyi*/true, /*lsw*/false, /*dstCode*/2};
    const int kFrames = 3;
    const std::vector<int> cleanSecs = buildTimeline(kBase, kFrames, /*leadTail*/10, /*leadOut*/11);

    // --- Section: clean bit-exact ALL fields + double-marker minute sync + lock
    {
        const auto x = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0xC10Cu);
        const Capture c = runIrregular(x);  // irregular chunk sizes here

        CHECK(c.finalState == ClockLockState::Locked);
        CHECK(c.finalStation == ClockStation::Wwvb);
        CHECK(c.secondCount > 0);
        // Voted time is "as of the newest frame" -> minute 22.
        checkVoted(c, /*min*/22, /*hr*/6, /*doy*/200, /*yr*/26, "clean");
        checkAuxFields(c, kBase, kFrames);
        // Minutes increment across the emitted frames (double-marker sync worked).
        bool saw20 = false, saw21 = false, saw22 = false;
        for (const auto& f : c.frames) {
            if (f.minute == 20) saw20 = true;
            if (f.minute == 21) saw21 = true;
            if (f.minute == 22) saw22 = true;
        }
        CHECK(saw20 && saw21 && saw22);

        // Callback sequencing: Acquiring before Locked; exactly one Locked transition.
        bool sawAcquiring = false, acqBeforeLock = true;
        int lockTransitions = 0;
        for (ClockLockState s : c.states) {
            if (s == ClockLockState::Acquiring) sawAcquiring = true;
            if (s == ClockLockState::Locked) {
                ++lockTransitions;
                if (!sawAcquiring) acqBeforeLock = false;
            }
        }
        CHECK(sawAcquiring);
        CHECK(acqBeforeLock);
        CHECK(lockTransitions == 1);
    }

    // --- Section: WS-7 diagnostics — stage flips on the clean vector; stage-1
    // readouts stay honest on noise-only air (tone measured, gate not passed).
    {
        const auto x = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0x1D1A6u);
        WwvbDecoder d(24000);
        Capture c; wire(d, c);
        feedFixed(d, x, 512);
        CHECK(d.state() == ClockLockState::Locked);      // precondition
        const ClockDecoderDiagnostics g = d.diagnostics();
        CHECK(g.toneDetected);
        CHECK(g.toneSnrDb > 10.0f);      // gate is peak > 12x median (~10.8 dB)
        CHECK(g.pwmContrast >= 1.4f);    // kMinContrast — a real AM drop exists
        CHECK(g.phaseLocked);
        CHECK(std::isnan(g.delayEstMs)); // WWV-only metric
        CHECK(g.anchored);
        CHECK(g.badFrameStreak == 0);    // WWV-only metric
        CHECK(g.framesInWindow >= 2);
        CHECK(g.windowSize == 8);
        CHECK(g.voteQuality > 0.0f);
        CHECK(g.refusalReason == static_cast<std::uint8_t>(ClockLockRefusal::None));

        // Noise-only: MEASURED decoder behavior (fail-first run 2026-07-22) is
        // that the early stages can transiently pass on pure AWGN — the 12x
        // Goertzel tone gate false-fires within ~60 s of windows, and random
        // symbol chatter can even double-marker-anchor briefly. What refuses
        // noise is the frame/voter machinery: no structurally-valid frame ever
        // completes. The diagnostics must report exactly that shape — late
        // stages empty, no lock, no unexplained verdict.
        WwvbDecoder n(24000);
        Capture cn; wire(n, cn);
        const auto nx = noiseVector(static_cast<std::size_t>(60) * kSR, 0.2, 0xF00Du);
        feedFixed(n, nx, 512);
        CHECK(n.state() != ClockLockState::Locked);
        const ClockDecoderDiagnostics gn = n.diagnostics();
        CHECK(gn.framesInWindow == 0);   // no valid frame ever survives noise
        CHECK(gn.voteQuality == 0.0f);
        CHECK(gn.refusalReason <= 4);    // always a valid ClockLockRefusal
    }

    // --- Section: WS-4.5 — lock demotes on signal loss, cached vote stops
    // (Live 2026-07-20 class: a stale Locked must never be pinned; dead air
    // demotes and the per-second cached re-emission stops with it.)
    {
        const auto x = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0xD37Au);
        WwvbDecoder d(24000);
        Capture c; wire(d, c);
        feedFixed(d, x, 512);
        CHECK(d.state() == ClockLockState::Locked);      // precondition

        const std::vector<float> silence(24000 * 240, 0.0f);
        feedFixed(d, silence, 512);
        CHECK(d.state() != ClockLockState::Locked);      // demoted, not pinned

        // After the demotion no further voted times may be emitted.
        const std::size_t nAtDemote = c.times.size();
        const std::vector<float> moreSilence(24000 * 60, 0.0f);
        feedFixed(d, moreSilence, 512);
        CHECK(c.times.size() == nAtDemote);
    }

    // --- Section: 16 dB envelope SNR bit-exact (assert the pinned floor)
    {
        const double sigma16 = (kCarrierAmp / std::sqrt(2.0)) / std::pow(10.0, kEnvSnrFloor / 20.0);
        const double snr = envelopeSnrDb(kCarrierAmp, sigma16);
        CHECK(snr >= kEnvSnrFloor - 1e-6);  // at/above the pinned floor

        const auto x = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, sigma16, 0x5A1Fu);
        const Capture c = runFixed(x);
        CHECK(c.finalState == ClockLockState::Locked);
        checkVoted(c, 22, 6, 200, 26, "snr16");
        checkAuxFields(c, kBase, kFrames);
    }

    // --- Section: 12 dB depth (AGC compression) bit-exact
    {
        const auto x = synth(cleanSecs, kWwvbAgcDb, /*bpsk*/false, /*sigma*/0.0, 0xA6Cu);
        const Capture c = runFixed(x);
        CHECK(c.finalState == ClockLockState::Locked);
        checkVoted(c, 22, 6, 200, 26, "agc12");
        checkAuxFields(c, kBase, kFrames);
    }

    // --- Section: BPSK-flip vector -> envelope IDENTICAL, decode unchanged (INV-8)
    {
        const auto clean = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0xB0B0u);
        const auto flip  = synth(cleanSecs, kWwvbDropDb, /*bpsk*/true,  /*sigma*/0.0, 0xB0B0u);
        CHECK(clean.size() == flip.size());
        // Amplitude envelope must be byte-for-byte identical: the phase flip only
        // changes carrier sign, so |sample| is unchanged.
        double maxEnvDiff = 0.0;
        for (std::size_t i = 0; i < clean.size(); ++i)
            maxEnvDiff = std::max(maxEnvDiff,
                                  std::fabs(static_cast<double>(std::fabs(clean[i])) -
                                            static_cast<double>(std::fabs(flip[i]))));
        CHECK(maxEnvDiff < 1e-6);

        const Capture cc = runFixed(clean);
        const Capture cf = runFixed(flip);
        CHECK(cc.finalState == ClockLockState::Locked);
        CHECK(cf.finalState == ClockLockState::Locked);
        CHECK(!cc.times.empty());
        CHECK(!cf.times.empty());
        if (!cc.times.empty() && !cf.times.empty()) {
            CHECK(cc.times.back().minute == cf.times.back().minute);
            CHECK(cc.times.back().hour   == cf.times.back().hour);
            CHECK(cc.times.back().doy    == cf.times.back().doy);
            CHECK(cc.times.back().year2  == cf.times.back().year2);
        }
        CHECK(cc.frames.size() == cf.frames.size());
    }

    // --- Section: noise-only (>= 5 minutes of pure AWGN) -> NEVER locks
    {
        const std::size_t fiveMin = static_cast<std::size_t>(5 * 60) * kSR;
        const auto x = noiseVector(fiveMin, 0.2, 0xDEADu);
        const Capture c = runFixed(x);
        CHECK(c.finalState != ClockLockState::Locked);
        CHECK(c.times.empty());
        for (ClockLockState s : c.states) CHECK(s != ClockLockState::Locked);
    }

    // --- Section: carrier-tone-only (no PWM) -> stays Acquiring, NEVER Locked
    {
        const auto x = synth(cleanSecs, /*dropDb*/0.0, /*bpsk*/false, /*sigma*/0.0, 0x7C0Eu);
        const Capture c = runFixed(x);
        CHECK(c.finalState == ClockLockState::Acquiring);
        CHECK(c.times.empty());
        for (ClockLockState s : c.states) CHECK(s != ClockLockState::Locked);
    }

    // --- Section: corrupted markers -> no false lock
    {
        std::vector<int> corrupt = cleanSecs;
        for (int& s : corrupt) if (s == kSymMarker) s = kSymZero;  // destroy every marker
        const auto x = synth(corrupt, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0xC0FFu);
        const Capture c = runFixed(x);
        CHECK(c.finalState != ClockLockState::Locked);
        CHECK(c.times.empty());
        for (ClockLockState s : c.states) CHECK(s != ClockLockState::Locked);
    }

    // --- Section: truncated final frame -> no partial-field emission
    {
        // lead-in(10) + frame20(60) + frame21(60) + 30 s of frame22 (cut off).
        std::vector<int> secs = buildTimeline(kBase, kFrames, /*leadTail*/10, /*leadOut*/0);
        const std::size_t keep = 10 + 60 + 60 + 30;
        if (secs.size() > keep) secs.resize(keep);
        const auto x = synth(secs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0x7A11u);
        const Capture c = runFixed(x);
        // The truncated minute (22) must never surface as a completed frame or vote.
        for (const auto& f : c.frames) CHECK(f.minute != 22);
        for (const auto& t : c.times) CHECK(t.minute != 22);
    }

    // --- Section: direct TimeFrameVoter hour/day/year rollover
    sectionVoterRollover();

    // --- Section: noisy-corpus regression (WS-3 exact-tuple degeneracy)
    sectionVoterNoisyCorpus();

    // --- Section: re-encoded-field weighting (field-min vs field-mean)
    sectionVoterMinuteFadeWeighting();

    // --- Section: quality honesty on a single decisive contested bit
    sectionVoterContestedBitQuality();

    // --- Section: doy-366-in-non-leap-year gate
    sectionVoterDoy366Gate();

    // --- Section: cross-bit Frankenstein composition (coherence gating)
    sectionVoterFrankenstein();

    // --- Section: lone-voter participation scaling
    sectionVoterLoneVoterParticipation();

    // --- WAV dump of the clean golden vector (opt-in via env var) + truth print
    if (const char* dir = std::getenv("AETHERCLOCK_DUMP_WAV_DIR")) {
        const auto golden = synth(cleanSecs, kWwvbDropDb, /*bpsk*/false, /*sigma*/0.0, 0xC10Cu);
        std::string path = std::string(dir) + "/wwvb_golden.wav";
        writeWavStereo16(path, golden, kSR);
        std::printf("golden truth: min=%d hr=%d doy=%d yr=%d frames=%d\n",
                    kBase.min, kBase.hr, kBase.doy, kBase.yr, kFrames);
        std::printf("wrote %s\n", path.c_str());
    }

    if (g_failures) {
        std::fprintf(stderr, "wwvb_decoder_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("wwvb_decoder_test: all checks passed\n");
    return 0;
}
