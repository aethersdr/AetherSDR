// AetherClock WS-1 — WWV/WWVH decoder test (plain main() + CHECK, NOT QtTest).
//
// Self-contained: the WWV signal synthesizer, the AWGN helper, and a tiny WAV
// writer all live in this file (std only). It exercises WwvDecoder through
// process() with realistic streaming chunk sizes and asserts every BCD field
// bit-exact against the synthesized truth.
//
// Signal model + levels are PINNED by SPEC.md §"Signal synthesizer spec"
// (PRD-A §8): 24 kHz; carrier 1000 Hz @ 0.5; 100 Hz subcarrier depth 0.30
// pulse-on / 0.06 pulse-off / 0 during the s0 minute hole; pulse per the NIST
// 170/470/770 ms @ +30 ms encoding; 5 ms tick @ 0.25 at the 2000 Hz image
// (WWVH: 2200 Hz). Field map per the NIST WWV time-code table (SP 432).
//
// The decoder .cpp implementations are authored in parallel; this file is
// syntax-checked against the frozen headers only. Value asserts therefore fix
// exact FIELD values and assert confidence RANGES/FLOORS, never exact
// confidence numbers.

#include "core/TimeFrameVoter.h"
#include "core/WwvDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace AetherSDR;

// ---- test harness (per SPEC.md §"Repo conventions") -----------------------
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int    kFs = 24000;              // pinned sample rate

// Pinned NIST WWV noise floor: bit-exact decode at >= 20 dB SNR in 700-1300 Hz.
constexpr double kWwvSnrFloorDb = 20.0;

// QA-documented confidence floor for a clean / near-clean lock (ranges only —
// exact matched-filter margins are impl-defined). Loosen here if a correct
// implementation legitimately reports lower; never tighten silently.
// Calibrated against the streaming decoder on the clean golden vector
// (observed minima: frameConfidence 0.475, quality 0.500 — the streaming
// biquad chain's matched-filter margins sit slightly below the batch
// prototype's). Noise-only input never locks at all, so these floors bound
// the clean/degraded cases with real headroom.
constexpr float  kConfidenceFloor      = 0.40f;  // clean golden vector
constexpr float  kConfidenceFloor20dB  = 0.25f;  // at the pinned 20 dB floor

// ---- WWV time-code synthesizer --------------------------------------------
// Per-second pulse class of the 100 Hz subcarrier.
enum class Sym { Hole, Zero, One, Marker };

// Broadcast truth for one minute; the synthesizer encodes ALL of these so
// bit-exactness of every field is assertable.
struct Truth {
    int  minute = 0, hour = 0, doy = 0, year2 = 0;
    int  dut1Tenths = 0;   // signed tenths (e.g. -3 => -0.3 s)
    bool dst1 = false;     // WWV s2
    bool dst2 = false;     // WWV s55
    bool lsw  = false;     // WWV s3 (leap-second warning)
};

struct SynthOpts {
    int          numFrames     = 3;                 // >= 3 consecutive minutes
    int          leadInSeconds = 10;                // tail of the prior minute
    int          leadOutSeconds = 10;               // head of the next minute
    ClockStation station       = ClockStation::Wwv; // tick image 2000/2200 Hz
    bool         removeHole     = false; // s0 gets a normal pulse (kills the cue)
    bool         corruptMarkers = false; // markers flattened to 170 ms zeros
    int          truncateSeconds = -1;   // stop the final frame after N seconds
};

using BcdMap = std::vector<std::pair<int,int>>; // (secondOfFrame, weight)

// Field maps, LSB-first within field, per the NIST WWV time-code table.
const BcdMap kMapMin{{10,1},{11,2},{12,4},{13,8},{15,10},{16,20},{17,40}};
const BcdMap kMapHr {{20,1},{21,2},{22,4},{23,8},{25,10},{26,20}};
const BcdMap kMapDoy{{30,1},{31,2},{32,4},{33,8},{35,10},{36,20},{37,40},
                     {38,80},{40,100},{41,200}};
const BcdMap kMapYr {{4,1},{5,2},{6,4},{7,8},{51,10},{52,20},{53,40},{54,80}};

void setBcd(std::array<Sym,60>& s, const BcdMap& m, int value) {
    for (const auto& pr : m) {
        int sec = pr.first, w = pr.second;
        int place = 1;
        while (w / place >= 10) place *= 10;   // decade of this weight
        int bw    = w / place;                  // 1|2|4|8 within the decade
        int digit = (value / place) % 10;
        if (digit & bw) s[sec] = Sym::One;
    }
}

// One minute of classified per-second symbols from broadcast truth.
std::array<Sym,60> encodeMinute(const Truth& t) {
    std::array<Sym,60> s;
    s.fill(Sym::Zero);
    s[0] = Sym::Hole;                                   // minute-mark subcarrier hole
    for (int m : {9,19,29,39,49,59}) s[m] = Sym::Marker; // P1..P5, P0
    setBcd(s, kMapMin, t.minute);
    setBcd(s, kMapHr,  t.hour);
    setBcd(s, kMapDoy, t.doy);
    setBcd(s, kMapYr,  t.year2);
    if (t.dut1Tenths > 0) s[50] = Sym::One;             // DUT1 sign (1 = +)
    int mag = std::abs(t.dut1Tenths);
    if (mag & 1) s[56] = Sym::One;                      // DUT1 magnitude 0.1
    if (mag & 2) s[57] = Sym::One;                      // DUT1 magnitude 0.2
    if (mag & 4) s[58] = Sym::One;                      // DUT1 magnitude 0.4
    if (t.dst1) s[2]  = Sym::One;
    if (t.dst2) s[55] = Sym::One;
    if (t.lsw)  s[3]  = Sym::One;
    return s;
}

// Append one second (kFs samples) of the pinned WWV waveform.
//   sample(t) = 0.5*sin(2pi*1000*t)*(1 + d(t)*sin(2pi*100*t)) + tick(t)
void appendSecond(std::vector<float>& sig, Sym sym, int tickFreq) {
    for (int k = 0; k < kFs; ++k) {
        const double t   = static_cast<double>(sig.size()) / kFs; // absolute -> phase-continuous
        const double tau = static_cast<double>(k) / kFs;          // within-second
        const double car = 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        double d;
        if (sym == Sym::Hole) {
            d = 0.0;                                    // no subcarrier at all
        } else {
            const double dur = (sym == Sym::Zero) ? 0.170
                             : (sym == Sym::One)  ? 0.470
                                                  : 0.770;         // marker
            d = (tau >= 0.030 && tau < 0.030 + dur) ? 0.30 : 0.06; // +30 ms start
        }
        const double sub  = 1.0 + d * std::sin(2.0 * kPi * 100.0 * t);
        const double tick = (tau < 0.005)                          // 5 ms burst
                          ? 0.25 * std::sin(2.0 * kPi * tickFreq * t)
                          : 0.0;
        sig.push_back(static_cast<float>(car * sub + tick));
    }
}

std::vector<float> synthWwv(const Truth& start, const SynthOpts& o) {
    const int tickFreq = (o.station == ClockStation::Wwvh) ? 2200 : 2000;
    std::vector<float> sig;
    sig.reserve(static_cast<size_t>((o.leadInSeconds + 60 * o.numFrames)) * kFs);

    auto emit = [&](std::array<Sym,60> sym, int secStart, int secEnd) {
        for (int sec = secStart; sec < secEnd; ++sec) {
            Sym s = sym[sec];
            if (o.corruptMarkers && s == Sym::Marker) s = Sym::Zero; // flatten markers
            if (o.removeHole && sec == 0)             s = Sym::Zero; // fill the hole
            appendSecond(sig, s, tickFreq);
        }
    };

    // Lead-in: the tail seconds of the prior minute, correctly encoded.
    Truth lead = start;
    lead.minute -= 1;
    if (lead.minute < 0) { lead.minute += 60; lead.hour = (lead.hour + 23) % 24; }
    emit(encodeMinute(lead), 60 - o.leadInSeconds, 60);

    // Consecutive full frames, minutes monotonically incrementing.
    for (int i = 0; i < o.numFrames; ++i) {
        Truth cur = start;
        const int total = start.minute + i;
        cur.minute = total % 60;
        cur.hour   = (start.hour + total / 60) % 24;
        int lastSec = 60;
        if (o.truncateSeconds >= 0 && i == o.numFrames - 1) lastSec = o.truncateSeconds;
        emit(encodeMinute(cur), 0, lastSec);
    }

    // Lead-out: the head seconds of the NEXT minute, correctly encoded, so the
    // final frame can complete (streaming decoders need trailing signal to
    // close the last second/frame). Skipped when truncating — the truncation
    // section depends on the signal ending mid-frame.
    if (o.truncateSeconds < 0 && o.leadOutSeconds > 0) {
        Truth next = start;
        const int total = start.minute + o.numFrames;
        next.minute = total % 60;
        next.hour   = (start.hour + total / 60) % 24;
        emit(encodeMinute(next), 0, o.leadOutSeconds);
    }
    return sig;
}

// ---- AWGN with SNR measured/scaled in the 700-1300 Hz band ----------------
// RBJ constant-0-dB-peak bandpass, f0=1000, Q=f0/BW with BW=600 Hz -> -3 dB
// edges near 700/1300 Hz. The SAME filter measures signal and noise power, so
// the resulting ratio IS the band-limited SNR of SPEC's floor definition.
std::vector<float> bandpass700_1300(const std::vector<float>& x) {
    const double f0 = 1000.0, Q = 1000.0 / 600.0;
    const double w0 = 2.0 * kPi * f0 / kFs;
    const double cs = std::cos(w0), sn = std::sin(w0), alpha = sn / (2.0 * Q);
    double b0 = alpha, b1 = 0.0, b2 = -alpha;
    const double a0 = 1.0 + alpha, a1 = -2.0 * cs, a2 = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0;
    const double na1 = a1 / a0, na2 = a2 / a0;
    std::vector<float> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double xi = x[i];
        const double yi = b0 * xi + b1 * x1 + b2 * x2 - na1 * y1 - na2 * y2;
        y[i] = static_cast<float>(yi);
        x2 = x1; x1 = xi; y2 = y1; y1 = yi;
    }
    return y;
}

double meanSquare(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : s / x.size();
}
double bandPower(const std::vector<float>& x) { return meanSquare(bandpass700_1300(x)); }

// Add AWGN scaled so band-SNR == snrDb. Deterministic (caller owns the PRNG).
std::vector<float> addAwgnAtBandSnr(const std::vector<float>& sig, double snrDb,
                                    std::mt19937& rng) {
    const double psig = bandPower(sig);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> noise(sig.size());
    for (auto& v : noise) v = static_cast<float>(nd(rng));
    const double pn     = bandPower(noise);
    const double snrLin = std::pow(10.0, snrDb / 10.0);
    const double g      = (pn > 0.0) ? std::sqrt(psig / (snrLin * pn)) : 0.0;
    std::vector<float> out(sig.size());
    for (size_t i = 0; i < sig.size(); ++i)
        out[i] = static_cast<float>(sig[i] + g * noise[i]);
    return out;
}

std::vector<float> pureAwgn(size_t n, double amp, std::mt19937& rng) {
    std::normal_distribution<double> nd(0.0, amp);
    std::vector<float> v(n);
    for (auto& x : v) x = static_cast<float>(nd(rng));
    return v;
}

// Measured band-SNR of a noisy vector vs its clean reference.
double measuredBandSnrDb(const std::vector<float>& clean, const std::vector<float>& noisy) {
    std::vector<float> resid(clean.size());
    for (size_t i = 0; i < clean.size(); ++i) resid[i] = noisy[i] - clean[i];
    const double ps = bandPower(clean), pn = bandPower(resid);
    return (pn > 0.0) ? 10.0 * std::log10(ps / pn) : 999.0;
}

// ---- tiny WAV writer (16-bit PCM, stereo dup, 24 kHz) ---------------------
void writeWavStereo(const std::string& path, const std::vector<float>& mono) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    const uint16_t nch = 2, bits = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(nch * bits / 8);
    const uint32_t byteRate   = static_cast<uint32_t>(kFs) * blockAlign;
    const uint32_t dataBytes  = static_cast<uint32_t>(mono.size()) * blockAlign;
    const uint32_t riffSize   = 36u + dataBytes;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); w32(riffSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(nch);
    w32(static_cast<uint32_t>(kFs)); w32(byteRate); w16(blockAlign); w16(bits);
    f.write("data", 4); w32(dataBytes);
    for (float s : mono) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        int16_t v = static_cast<int16_t>(std::lround(c * 32767.0f));
        w16(static_cast<uint16_t>(v)); // L
        w16(static_cast<uint16_t>(v)); // R (duplicated mono)
    }
}

// ---- decoder driving + callback capture -----------------------------------
struct Capture {
    std::vector<ClockFrameInfo> frames;
    std::vector<ClockTimeInfo>  times;
    std::vector<ClockLockState> states;
    int lockedTransitions = 0;
};

void wire(WwvDecoder& d, Capture& c) {
    d.onFrame        = [&c](const ClockFrameInfo& f) { c.frames.push_back(f); };
    d.onTime         = [&c](const ClockTimeInfo&  t) { c.times.push_back(t); };
    d.onStateChanged = [&c](ClockLockState s) {
        c.states.push_back(s);
        if (s == ClockLockState::Locked) ++c.lockedTransitions;
    };
}

void feedChunks(WwvDecoder& d, const std::vector<float>& sig, size_t chunk) {
    for (size_t i = 0; i < sig.size(); ) {
        const size_t n = std::min(chunk, sig.size() - i);
        d.process(sig.data() + i, n);
        i += n;
    }
}

// Irregular chunking to shake out chunk-boundary bugs (edges split mid-tick,
// mid-pulse, single-sample dribbles).
void feedIrregular(WwvDecoder& d, const std::vector<float>& sig) {
    static const size_t sizes[] = {1, 7, 63, 512, 4096, 333, 2048, 9};
    size_t i = 0, k = 0;
    while (i < sig.size()) {
        const size_t n = std::min(sizes[k % 8], sig.size() - i);
        d.process(sig.data() + i, n);
        i += n; ++k;
    }
}

bool minutesIncrement(const std::vector<ClockFrameInfo>& f) {
    if (f.size() < 2) return false;
    for (size_t i = 1; i < f.size(); ++i)
        if (f[i].minute != (f[i - 1].minute + 1) % 60) return false;
    return true;
}

bool acquiringBeforeLock(const std::vector<ClockLockState>& s) {
    size_t li = s.size();
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == ClockLockState::Locked) { li = i; break; }
    if (li == s.size()) return false;
    for (size_t i = 0; i < li; ++i)
        if (s[i] == ClockLockState::Acquiring) return true;
    return false;
}

// The golden broadcast truth: 06:20 -> 06:21 -> 06:22 on doy 200, yr 26,
// DUT1 = -0.3 s, DST1 set, DST2 clear, leap-second warning set.
const Truth   kGold{20, 6, 200, 26, /*dut1*/ -3, /*dst1*/ true, /*dst2*/ false, /*lsw*/ true};
constexpr int kExpectNewestMin = 22; // start.minute + numFrames - 1

// ==== test sections ========================================================

// [wwv.clean] Clean 3-frame synth -> every BCD field bit-exact, station tag,
// lock, minutes increment, callback sequencing, confidence floor.
void sectionCleanBitExact(const std::vector<float>& clean) {
    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, clean, 512);

    CHECK(dec.state()   == ClockLockState::Locked);
    CHECK(dec.station() == ClockStation::Wwv);

    // WS-7 diagnostics snapshot: on the clean locked vector every funnel stage
    // reads "passed" and the refusal tag is None. (Stage 4 lives engine-side.)
    {
        const ClockDecoderDiagnostics g = dec.diagnostics();
        CHECK(g.toneDetected);
        CHECK(g.phaseLocked);
        CHECK(g.toneSnrDb > 0.0f);            // folded tick impulse, honest dB
        CHECK(g.pwmContrast == 0.0f);         // WWVB-only metric
        CHECK(!std::isnan(g.delayEstMs));     // delay settled on the clean vector
        CHECK(g.anchored);
        CHECK(g.badFrameStreak == 0);
        CHECK(g.framesInWindow >= 2);
        CHECK(g.windowSize == 8);
        CHECK(g.voteQuality >= kConfidenceFloor);
        CHECK(g.refusalReason == static_cast<std::uint8_t>(ClockLockRefusal::None));
    }

    // Voted time "as of the newest frame".
    CHECK(!cap.times.empty());
    if (!cap.times.empty()) {
        const ClockTimeInfo& t = cap.times.back();
        CHECK(t.minute == kExpectNewestMin);
        CHECK(t.hour   == 6);
        CHECK(t.doy    == 200);
        CHECK(t.year2  == 26);
        CHECK(t.station == ClockStation::Wwv);
        CHECK(t.quality >= kConfidenceFloor && t.quality <= 1.0f);
    }

    // Per-frame raw decode: fields including DUT1 sign+magnitude, DST, LSW.
    CHECK(!cap.frames.empty());
    if (!cap.frames.empty()) {
        const ClockFrameInfo& f = cap.frames.back();
        CHECK(f.minute      == kExpectNewestMin);
        CHECK(f.hour        == 6);
        CHECK(f.doy         == 200);
        CHECK(f.year2       == 26);
        CHECK(f.dut1Tenths  == -3);          // sign + magnitude bit-exact
        CHECK(f.dst1        == true);
        CHECK(f.dst2        == false);
        CHECK(f.leapPending == true);        // WWV LSW s3
        CHECK(f.leapYear    == false);       // always false for WWV
        CHECK(f.station     == ClockStation::Wwv);
        CHECK(f.frameConfidence >= kConfidenceFloor && f.frameConfidence <= 1.0f);
    }

    CHECK(minutesIncrement(cap.frames));

    // Sequencing: Acquiring precedes Locked; exactly ONE transition to Locked.
    CHECK(acquiringBeforeLock(cap.states));
    CHECK(cap.lockedTransitions == 1);
}

// [wwv.wwvh] WWVH variant (2200 Hz tick image) -> station() == Wwvh, fields
// decode identically (same code, only the tick tone tags the station).
void sectionWwvhStation() {
    SynthOpts o; o.station = ClockStation::Wwvh;
    const std::vector<float> sig = synthWwv(kGold, o);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    CHECK(dec.station() == ClockStation::Wwvh);
    CHECK(dec.state()   == ClockLockState::Locked);
    if (!cap.frames.empty()) {
        CHECK(cap.frames.back().station == ClockStation::Wwvh);
        CHECK(cap.frames.back().minute  == kExpectNewestMin);
    }
    if (!cap.times.empty())
        CHECK(cap.times.back().station == ClockStation::Wwvh);
}

// [wwv.snr20] Pinned noise floor: bit-exact at exactly 20 dB SNR in-band.
void sectionSnr20(const std::vector<float>& clean, std::mt19937& rng) {
    CHECK(kWwvSnrFloorDb == 20.0);           // pinned floor is documented in-test

    const std::vector<float> noisy = addAwgnAtBandSnr(clean, kWwvSnrFloorDb, rng);

    // Independent of the decoder: prove the synth/AWGN path really hit 20 dB.
    const double snrMeas = measuredBandSnrDb(clean, noisy);
    CHECK(std::fabs(snrMeas - kWwvSnrFloorDb) < 0.5);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedIrregular(dec, noisy);               // irregular chunks under noise

    CHECK(dec.state() == ClockLockState::Locked);
    if (!cap.frames.empty()) {
        const ClockFrameInfo& f = cap.frames.back();
        CHECK(f.minute     == kExpectNewestMin);
        CHECK(f.hour       == 6);
        CHECK(f.doy        == 200);
        CHECK(f.year2      == 26);
        CHECK(f.dut1Tenths == -3);
        CHECK(f.dst1 == true && f.dst2 == false && f.leapPending == true);
        CHECK(f.frameConfidence >= kConfidenceFloor20dB && f.frameConfidence <= 1.0f);
    }
    if (!cap.times.empty())
        CHECK(cap.times.back().minute == kExpectNewestMin);
}

// [wwv.noiseonly] >= 5 minutes of pure AWGN -> NEVER locks.
void sectionNoiseOnlyNeverLocks(std::mt19937& rng) {
    const size_t n = static_cast<size_t>(5 * 60) * kFs;   // 5 minutes
    const std::vector<float> noise = pureAwgn(n, 0.3, rng);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, noise, 512);

    CHECK(dec.state() != ClockLockState::Locked);
    CHECK(cap.lockedTransitions == 0);
    CHECK(cap.times.empty());                // no voted time ever emitted
}

// [wwv.badmarkers] Corrupted markers (P-pulses flattened to 170 ms zeros) ->
// no frame sync, no false lock.
void sectionCorruptMarkersNoFalseLock() {
    SynthOpts o; o.corruptMarkers = true;
    const std::vector<float> sig = synthWwv(kGold, o);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    CHECK(dec.state() != ClockLockState::Locked);
    CHECK(cap.lockedTransitions == 0);
}

// [wwv.truncated] Truncated final frame (30 s) -> no partial-field emission
// for the incomplete minute.
void sectionTruncatedNoPartial() {
    SynthOpts o; o.truncateSeconds = 30;     // frame min=22 cut off at s30
    const std::vector<float> sig = synthWwv(kGold, o);
    const int truncatedMin = kExpectNewestMin;   // 22 — must never be emitted

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, sig, 512);

    for (const ClockFrameInfo& f : cap.frames)
        CHECK(f.minute != truncatedMin);     // no partial frame surfaced
    if (!cap.frames.empty())
        CHECK(cap.frames.back().minute == truncatedMin - 1);  // last COMPLETE = 21
    if (!cap.times.empty())
        CHECK(cap.times.back().minute != truncatedMin);
}

// [wwv.decade] Decade degeneracy: marker-only anchoring is degenerate mod 10 s.
// (a) On the clean vector a correct decode landing exactly on truth IS the
//     rejection proof — a 10 s-shifted anchor would decode shifted fields.
// (b) On a hole-omitted-but-marker-consistent vector, the decoder must either
//     refuse to lock or still land on truth.
void sectionDecadeDegeneracy(const std::vector<float>& clean) {
    // (a) clean — decoded minutes match truth exactly, not truth+/-shift.
    {
        WwvDecoder dec(kFs);
        Capture cap; wire(dec, cap);
        feedChunks(dec, clean, 512);
        CHECK(dec.state() == ClockLockState::Locked);
        if (!cap.times.empty())
            CHECK(cap.times.back().minute == kExpectNewestMin);
        // Every per-frame minute is exactly one of the encoded truths.
        for (const ClockFrameInfo& f : cap.frames)
            CHECK(f.minute == 20 || f.minute == 21 || f.minute == 22);
        CHECK(minutesIncrement(cap.frames));
    }
    // (b) hole omitted (s0 carries a normal pulse), markers still consistent.
    {
        SynthOpts o; o.removeHole = true;
        const std::vector<float> sig = synthWwv(kGold, o);
        WwvDecoder dec(kFs);
        Capture cap; wire(dec, cap);
        feedChunks(dec, sig, 512);
        const bool refused = (dec.state() != ClockLockState::Locked);
        const bool onTruth = (!cap.times.empty() &&
                              cap.times.back().minute == kExpectNewestMin);
        CHECK(refused || onTruth);           // refuse, or still land on truth
    }
}

// [wwv.confidence] Documented confidence floor: reported per-frame and voted
// confidences sit in [0,1] and clear the QA floor on the clean vector.
void sectionConfidenceFloor(const std::vector<float>& clean) {
    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, clean, 512);

    for (const ClockFrameInfo& f : cap.frames) {
        CHECK(f.frameConfidence >= 0.0f && f.frameConfidence <= 1.0f);
        CHECK(f.frameConfidence >= kConfidenceFloor);   // documented floor
    }
    for (const ClockTimeInfo& t : cap.times) {
        CHECK(t.quality >= 0.0f && t.quality <= 1.0f);
        CHECK(t.quality >= kConfidenceFloor);
    }
    float minFrameConf = 1.0f, minQuality = 1.0f;
    for (const ClockFrameInfo& f : cap.frames)
        minFrameConf = std::min(minFrameConf, f.frameConfidence);
    for (const ClockTimeInfo& t : cap.times)
        minQuality = std::min(minQuality, t.quality);
    std::fprintf(stderr, "[wwv.confidence] documented floor = %.2f "
                 "(observed clean minima: frameConfidence %.3f, quality %.3f)\n",
                 static_cast<double>(kConfidenceFloor),
                 static_cast<double>(minFrameConf),
                 static_cast<double>(minQuality));
}

// ---- direct TimeFrameVoter rollover tests (WWV field maps) ----------------
// These drive TimeFrameVoter directly (no audio path) so the whole-timestamp
// value vote is exercised across hour/day/year rollovers — the case no golden
// audio vector reached because every vector stayed inside one hour.

TimeFrameVoter::Config wwvVoterConfig() {
    auto toFieldMap = [](const BcdMap& m) {
        ClockFieldMap fm;
        for (const auto& pr : m) fm.push_back(ClockBitWeight{pr.first, pr.second});
        return fm;
    };
    TimeFrameVoter::Config cfg;
    cfg.fields[TimeFrameVoter::FieldMinutes] = toFieldMap(kMapMin);
    cfg.fields[TimeFrameVoter::FieldHours]   = toFieldMap(kMapHr);
    cfg.fields[TimeFrameVoter::FieldDoy]     = toFieldMap(kMapDoy);
    cfg.fields[TimeFrameVoter::FieldYear]    = toFieldMap(kMapYr);
    cfg.markerSeconds = {9, 19, 29, 39, 49, 59};
    cfg.window = 8;
    cfg.minFramesForLock = 2;
    cfg.agingFactor = 0.9f;
    return cfg;
}

// One voter frame (per-second ClockSymbol) from a timestamp; the s0 minute hole
// is a non-data second, mapped to Marker (never in a field map).
std::array<ClockSymbol, 60> wwvVoterFrame(int mn, int hr, int doy, int yr) {
    Truth t; t.minute = mn; t.hour = hr; t.doy = doy; t.year2 = yr;
    const std::array<Sym, 60> s = encodeMinute(t);
    std::array<ClockSymbol, 60> out{};
    for (int i = 0; i < 60; ++i) {
        out[i] = (s[i] == Sym::One)    ? ClockSymbol::One
               : (s[i] == Sym::Zero)   ? ClockSymbol::Zero
                                       : ClockSymbol::Marker;  // Marker or Hole
    }
    return out;
}

// [wwv.voter.rollover] After every appended frame the voted timestamp equals the
// newest frame's truth across hour/day/year boundaries, and a mixed-hour window
// never votes an off-air hour.
void sectionVoterRollover() {
    std::array<float, 60> conf; conf.fill(1.0f);

    // (a) advanceMinutes helper is calendar-correct in isolation.
    CHECK(advanceMinutes(TimeFields{59, 21, 200, 26}, 1) == (TimeFields{0, 22, 200, 26}));
    CHECK(advanceMinutes(TimeFields{59, 23, 200, 26}, 1) == (TimeFields{0, 0, 201, 26}));
    CHECK(advanceMinutes(TimeFields{59, 23, 365, 26}, 1) == (TimeFields{0, 0, 1, 27})); // 2026 not leap
    CHECK(advanceMinutes(TimeFields{59, 23, 366, 28}, 1) == (TimeFields{0, 0, 1, 29})); // 2028 leap

    // (b) hour rollover 21:57 -> 22:02.
    {
        TimeFrameVoter v(wwvVoterConfig());
        struct MH { int mn, hr; };
        for (const MH e : {MH{57,21}, {58,21}, {59,21}, {0,22}, {1,22}, {2,22}}) {
            v.addFrame(wwvVoterFrame(e.mn, e.hr, 200, 26), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == e.mn);
            CHECK(v.votedField(TimeFrameVoter::FieldHours)   == e.hr);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
            CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);
        }
    }

    // (c) day rollover 23:58 -> 00:01, doy 200 -> 201.
    {
        TimeFrameVoter v(wwvVoterConfig());
        struct MHD { int mn, hr, doy; };
        for (const MHD e : {MHD{58,23,200}, {59,23,200}, {0,0,201}, {1,0,201}}) {
            v.addFrame(wwvVoterFrame(e.mn, e.hr, e.doy, 26), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == e.mn);
            CHECK(v.votedField(TimeFrameVoter::FieldHours)   == e.hr);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == e.doy);
        }
    }

    // (d) year rollover on doy 365 -> 1, yr 26 -> 27 (non-leap year length).
    {
        TimeFrameVoter v(wwvVoterConfig());
        struct T { int mn, hr, doy, yr; };
        for (const T e : {T{58,23,365,26}, {59,23,365,26}, {0,0,1,27}, {1,0,1,27}}) {
            v.addFrame(wwvVoterFrame(e.mn, e.hr, e.doy, e.yr), conf);
            CHECK(v.votedField(TimeFrameVoter::FieldDoy)  == e.doy);
            CHECK(v.votedField(TimeFrameVoter::FieldYear) == e.yr);
        }
    }

    // (e) bizarre-blend regression: a window holding hour-21 AND hour-22 frames
    //     must vote an hour of ONLY 21 or 22 — never a per-bit-mixed third value
    //     (the live 20:xx symptom). 7 consecutive hour-21 frames, then hour-22.
    {
        TimeFrameVoter v(wwvVoterConfig());
        struct MH { int mn, hr; };
        for (const MH e : {MH{53,21}, {54,21}, {55,21}, {56,21}, {57,21}, {58,21},
                           {59,21}, {0,22}, {1,22}, {2,22}}) {
            v.addFrame(wwvVoterFrame(e.mn, e.hr, 200, 26), conf);
            const int h = v.votedField(TimeFrameVoter::FieldHours);
            CHECK(h == 21 || h == 22);                 // never an off-air hour
            CHECK(h == e.hr);                          // and the newest hour wins
        }
    }
}

// One voter frame from a timestamp, then per-second corruptions injected at
// chosen confidences: each Corrupt overrides one second's symbol and its
// matched-filter margin. This is how the noisy-corpus vector shapes single-bit
// fades (low margin) and confident misreads (high margin) the way the live
// receiver saw them.
struct Corrupt { int second; ClockSymbol symbol; float confidence; };
std::pair<std::array<ClockSymbol, 60>, std::array<float, 60>>
wwvNoisyFrame(int mn, int hr, int doy, int yr, float baseConf,
              const std::vector<Corrupt>& corruptions) {
    std::array<ClockSymbol, 60> syms = wwvVoterFrame(mn, hr, doy, yr);
    std::array<float, 60> conf;
    conf.fill(baseConf);
    for (const Corrupt& c : corruptions) {
        syms[c.second] = c.symbol;
        conf[c.second] = c.confidence;
    }
    return {syms, conf};
}

// [wwv.voter.noisycorpus] Regression for the WS-3 exact-tuple degeneracy: an
// 8-frame window inside one hour (06:18..06:25, doy 200, yr 26) where EVERY
// frame is corrupt in a different field, so no whole-tuple key ever repeats.
// The old whole-tuple vote then degenerates to the newest frame's corrupt tuple
// (it carries yr=66, so the voter reports the year 25 off) at a high, honest-
// looking per-bit static quality — the exact "confident garbage" the live
// receiver locked onto. Normalize-then-per-bit recovers the true timestamp from
// the per-field majorities: single-bit fades lose on their low margin, confident
// misreads lose to the aged weight of the many correct frames, and the reported
// quality now dips because the voted space itself is contested.
void sectionVoterNoisyCorpus() {
    constexpr float C  = 0.35f;   // clean-bit margin (the live corpus ran ~0.35..0.41)
    constexpr float LO = 0.05f;   // faded minute bit — a low-margin miss
    constexpr float HI = 0.90f;   // a confident misread — high margin, wrong bit

    TimeFrameVoter v(wwvVoterConfig());

    // Frames added oldest -> newest (newest kept at the back of the window).
    // The three oldest carry a faded minutes tens bit at LOW margin; the five
    // newest are minutes-clean but each carries one confident wrong static bit,
    // so NO frame decodes a fully-correct tuple.
    struct NF { int mn, hr, doy, yr; std::vector<Corrupt> corr; };
    const NF frames[] = {
        // min18: drop the minutes 10-bit (sec 15) -> raw minute 08.
        {18, 6, 200, 26, {{15, ClockSymbol::Zero, LO}}},
        // min19: drop the minutes 10-bit -> raw minute 09.
        {19, 6, 200, 26, {{15, ClockSymbol::Zero, LO}}},
        // min20: drop the minutes 20-bit (sec 16) -> raw minute 00.
        {20, 6, 200, 26, {{16, ClockSymbol::Zero, LO}}},
        // min21: set the doy 2-bit (sec 31) -> raw doy 202.
        {21, 6, 200, 26, {{31, ClockSymbol::One, HI}}},
        // min22: set the doy 1-bit (sec 30) -> raw doy 201.
        {22, 6, 200, 26, {{30, ClockSymbol::One, HI}}},
        // min23: drop the year 20-bit (sec 52) -> raw year 06.
        {23, 6, 200, 26, {{52, ClockSymbol::Zero, HI}}},
        // min24: drop the hour 4-bit (sec 22) -> raw hour 02.
        {24, 6, 200, 26, {{22, ClockSymbol::Zero, HI}}},
        // min25 (newest): set the year 40-bit (sec 53) -> raw year 66. An
        // as-newest / exact-tuple voter reports THIS frame's year (66).
        {25, 6, 200, 26, {{53, ClockSymbol::One, HI}}},
    };
    for (const NF& f : frames) {
        const auto [syms, conf] = wwvNoisyFrame(f.mn, f.hr, f.doy, f.yr, C, f.corr);
        v.addFrame(syms, conf);
    }

    // Normalize-then-per-bit recovers the TRUE timestamp as of the newest frame,
    // even though the newest frame's own decode carries yr=66 and no tuple repeats.
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);

    // The newest raw frame really does decode yr=66, so the reported year (26)
    // could ONLY come from the cross-frame normalized vote, not from any single
    // frame — this is what the WS-3 exact-tuple voter got wrong.
    CHECK(v.lastFrameMinute() == 25);

    // Quality is coupled to the voted space now: lockConfidence is the MINIMUM
    // per-bit margin, so this window's several contested bits collapse it far out
    // of the clean-lock band — measured ~0.292 (the single most-contested voted
    // bit's normalized margin), vs the ~0.926 the mean-aggregating voter reported
    // on this same window while emitting the WRONG time. No confident garbage.
    const float q = v.lockConfidence();
    CHECK(q > 0.0f && q <= 1.0f);
    CHECK(q < 0.35f);

    // A clean 8-frame window over the same minutes votes the same timestamp at
    // strictly higher quality — proving the dip tracks the contention, not luck.
    TimeFrameVoter clean(wwvVoterConfig());
    std::array<float, 60> cleanConf;
    cleanConf.fill(C);
    for (int mn = 18; mn <= 25; ++mn)
        clean.addFrame(wwvVoterFrame(mn, 6, 200, 26), cleanConf);
    CHECK(clean.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(clean.votedField(TimeFrameVoter::FieldYear)    == 26);
    CHECK(clean.lockConfidence() > q);
}

// [wwv.voter.minutefade] Guards the re-encoded-field weighting: minutes ALWAYS
// take the re-encode path (extrapolation always moves them), so the per-bit fade
// rescue reaches minutes only through that path's weight. A window of 5 newest
// frames whose minutes-20 bit faded (LOW margin) over 3 older clean frames votes
// the WRONG minute (05, the live "min=03/06" symptom) if the re-encoded value is
// weighted by its field-MEAN confidence — the one faded bit is diluted ~1/7 and
// the five heavy-but-wrong frames tip the tally. Weighting by the field-MIN
// confidence makes each faded frame vote its minutes at ~0.05, so the three
// clean frames carry the 20 bit and the true minute (25) survives.
void sectionVoterMinuteFadeWeighting() {
    constexpr float C  = 0.40f;   // clean-bit margin
    constexpr float LO = 0.05f;   // faded minutes-20 bit — a low-margin miss

    TimeFrameVoter v(wwvVoterConfig());
    // Oldest three frames clean (min 18/19/20); newest five fade the minutes 20
    // bit (sec 16) at LOW margin — each still decodes the other minute bits, so
    // only the tens is wrong, and all statics stay clean.
    struct NF { int mn; std::vector<Corrupt> corr; };
    const NF frames[] = {
        {18, {}}, {19, {}}, {20, {}},
        {21, {{16, ClockSymbol::Zero, LO}}}, // 21 has the 20 bit -> raw 01
        {22, {{16, ClockSymbol::Zero, LO}}}, // -> raw 02
        {23, {{16, ClockSymbol::Zero, LO}}}, // -> raw 03
        {24, {{16, ClockSymbol::Zero, LO}}}, // -> raw 04
        {25, {{16, ClockSymbol::Zero, LO}}}, // newest -> raw 05
    };
    for (const NF& f : frames) {
        const auto [syms, conf] = wwvNoisyFrame(f.mn, 6, 200, 26, C, f.corr);
        v.addFrame(syms, conf);
    }

    // Field-MIN weighting: the true minute survives the five heavier faded frames.
    // (Field-MEAN weighting would vote 05 here — the regression this pins.)
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)    == 26);
}

// [wwv.voter.contested-bit-quality] Quality must be HONEST about a single
// decisive contested bit. The year-40 bit is flipped in the four NEWEST frames
// (raw year 26 -> 66, in-range so the frame gate keeps them); their aged weight
// (0.40 x (0.9^0..0.9^3) = 1.3756) beats the four clean oldest frames' (0.40 x
// (0.9^4..0.9^7) = 0.9025), so votedField votes year 66 — recency wins and that
// is accepted. The invariant here is quality: exactly ONE bit decided a whole
// field against real cross-frame disagreement, so lockConfidence must collapse
// to that bit's normalized margin ((1.3756-0.9025)/(1.3756+0.9025) = 0.2077),
// not average it away. A mean over the 31 timestamp bits reads (30*1.0 + 0.208)/
// 31 = 0.974 — confident garbage; the MIN aggregation reports ~0.208.
void sectionVoterContestedBitQuality() {
    std::array<float, 60> conf;
    conf.fill(0.40f);

    TimeFrameVoter v(wwvVoterConfig());
    // Minutes increment (18..25) so the window locks; oldest four carry the true
    // year 26, newest four carry 66 (only the year-40 bit differs).
    for (int mn = 18; mn <= 21; ++mn) v.addFrame(wwvVoterFrame(mn, 6, 200, 26), conf);
    for (int mn = 22; mn <= 25; ++mn) v.addFrame(wwvVoterFrame(mn, 6, 200, 66), conf);

    CHECK(v.locked());                                          // it IS a lock...
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 66);       // ...voting year 66...
    // ...but quality is honest about the lone decisive bit (mean-agg reads ~0.974).
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.35f);
    // The other three fields are unanimous, so they don't prop the quality up.
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 25);
    CHECK(v.votedField(TimeFrameVoter::FieldHours)   == 6);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)     == 200);
}

// [wwv.voter.doy366gate] A doy-366 frame in a NON-leap year is off-air and must
// be gated out: advanceMinutes wraps doy 366 > 365 into Jan 1 of the next year
// (even at age 0, where the wrap loop still runs), so an un-gated corrupt frame
// would drag the vote onto an off-air date. Here a confident (high-margin) newest
// frame decodes doy 366 in yr 27 (2027 is not a leap year); the gate excludes it
// so the two clean frames hold the vote at doy 200 / yr 27. Without the gate this
// frame outweighs them and the vote becomes doy 1 / yr 28.
void sectionVoterDoy366Gate() {
    constexpr float C  = 0.35f;
    constexpr float HI = 0.90f;
    std::array<float, 60> cc;  cc.fill(C);
    std::array<float, 60> chi; chi.fill(HI);

    TimeFrameVoter v(wwvVoterConfig());
    v.addFrame(wwvVoterFrame(23, 6, 200, 27), cc);   // clean
    v.addFrame(wwvVoterFrame(24, 6, 200, 27), cc);   // clean
    v.addFrame(wwvVoterFrame(25, 6, 366, 27), chi);  // corrupt newest: doy 366 in a non-leap year

    // The corrupt frame is gated out; the clean frames hold the true date.
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)  == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 27);
}

// [wwv.voter.frankenstein] Per-bit voting alone can assemble a value NO frame
// held when sibling bits are won by DIFFERENT frame camps. Three clean frames
// read hour 2 with the 4-bit (sec 22, truly Zero) FADED (sec21 One@0.90, sec22
// Zero@0.05); one confident newest frame reads hour 4 (sec21 Zero@0.05, sec22
// One@0.90). Per-bit: sec21 -> One (the three), sec22 -> One (the one) -> compose
// = 2+4 = 6, against a 3:1 frame majority for 2. Coherence catches it: sec22's
// confidence-winner (One) contradicts its aging-count majority (Zero, 3 frames),
// so the hour field falls back to the top HELD value (2). Quality collapses to
// the weak held-value margin (both camps vote at field-min conf 0.05: value 2 =
// 0.05x(0.9^1+0.9^2+0.9^3)=0.122, value 4 = 0.05, margin 0.072/0.172 = 0.418).
// The round-3 min-margin voter read hour 6 at q0.76 — confident garbage.
void sectionVoterFrankenstein() {
    constexpr float LO = 0.05f;   // faded / wrong-camp bit
    constexpr float HI = 0.90f;   // confident bit
    constexpr float C  = 0.40f;

    TimeFrameVoter v(wwvVoterConfig());
    // Minutes 18..21 increment so it locks; three clean hour-2 frames then one
    // confident hour-4 newest. (sec 21 = hour 2-bit, sec 22 = hour 4-bit.)
    for (int mn = 18; mn <= 20; ++mn) {
        const auto [syms, conf] = wwvNoisyFrame(
            mn, 2, 200, 26, C,
            {{21, ClockSymbol::One, HI}, {22, ClockSymbol::Zero, LO}});
        v.addFrame(syms, conf);
    }
    {
        const auto [syms, conf] = wwvNoisyFrame(
            21, 4, 200, 26, C,
            {{21, ClockSymbol::Zero, LO}, {22, ClockSymbol::One, HI}});
        v.addFrame(syms, conf);  // corrupt newest reads hour 4
    }

    CHECK(v.locked());
    CHECK(v.votedField(TimeFrameVoter::FieldHours) == 2);   // held majority, NOT 6
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 21);
    CHECK(v.votedField(TimeFrameVoter::FieldDoy)   == 200);
    CHECK(v.votedField(TimeFrameVoter::FieldYear)  == 26);
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.45f);   // held-value margin ~0.418; round-3 read ~0.76 at hour 6
}

// [wwv.voter.phantom-rotating] Refuter-round-5 regression (2026-07-20): even
// with EVERY bit coherent (confidence-winner == count majority), rotating
// 2-of-3 misreads at uniform noise-band confidence can win each hour bit from
// a different frame camp and compose an hour NO frame held: frames holding
// {9, 10, 3} per-bit-compose to 8+2+1 = 11, in range, every winning side
// carrying a floor-clearing vote (the pre-fix voter emitted it at q~0.20,
// above the lock floor). Per-bit coherence is necessary but not sufficient --
// the composed value must itself be a HELD value, else the field demotes to
// the held-value fallback (here: the clean newest frame's hour 3, whose
// contested margin then honestly refuses the lock).
void sectionVoterPhantomRotating() {
    constexpr float N = 0.10f;   // uniform noise-band margin -- clears the
                                 // trust floor, below the clean corpus band

    TimeFrameVoter::Config cfg = wwvVoterConfig();
    cfg.minBitConfidence = 0.05f;   // production floors (WS-4.5)
    cfg.minLockQuality   = 0.05f;
    TimeFrameVoter v(cfg);

    // Minutes increment 18..20 (lockable); hour truth 3 (s20 + s21). The two
    // older frames each misread TWO hour bits; the newest is fully correct.
    {   // holds hour 9 (8+1): s23 set, s21 dropped
        const auto [syms, conf] = wwvNoisyFrame(
            18, 3, 200, 26, N,
            {{23, ClockSymbol::One, N}, {21, ClockSymbol::Zero, N}});
        v.addFrame(syms, conf);
    }
    {   // holds hour 10 (8+2): s23 set, s20 dropped
        const auto [syms, conf] = wwvNoisyFrame(
            19, 3, 200, 26, N,
            {{23, ClockSymbol::One, N}, {20, ClockSymbol::Zero, N}});
        v.addFrame(syms, conf);
    }
    {   // newest: clean, holds the true hour 3
        std::array<float, 60> conf;
        conf.fill(N);
        v.addFrame(wwvVoterFrame(20, 3, 200, 26), conf);
    }

    const int h = v.votedField(TimeFrameVoter::FieldHours);
    CHECK(h != 11);   // the phantom -- held by no frame
    CHECK(h == 3);    // held-value fallback: the newest (true) hour
    CHECK(v.votedField(TimeFrameVoter::FieldMinutes) == 20);
    // If it claims a lock at all, the hour must be a value some frame held.
    CHECK(!v.locked() || h == 3 || h == 9 || h == 10);
    // WS-7: any refusal must carry a tag (never an unexplained "no").
    if (!v.locked())
        CHECK(v.lockRefusal() != ClockLockRefusal::None);
}

// [wwv.voter.lone-voter-participation] A bit only ONE frame actually voted must
// not read as certain. sec 54 (year-80 bit, truly Zero) is Unknown in three of
// four otherwise-clean frames; only the newest classifies it (Zero). Its winning
// margin is ~1.0 (no opposition), but participation = aged_newest / Σ aged =
// 1.0/(1+0.9+0.81+0.729) = 0.291, so its TRUST = 0.291 pulls quality down. The
// value is unaffected (year 26); the round-3 min-margin voter read this window
// at q~1.0 — certain about a bit almost no frame saw.
void sectionVoterLoneVoterParticipation() {
    constexpr float C = 0.40f;

    TimeFrameVoter v(wwvVoterConfig());
    for (int mn = 18; mn <= 20; ++mn) {
        const auto [syms, conf] =
            wwvNoisyFrame(mn, 6, 200, 26, C, {{54, ClockSymbol::Unknown, C}});
        v.addFrame(syms, conf);
    }
    std::array<float, 60> newestConf;
    newestConf.fill(C);
    v.addFrame(wwvVoterFrame(21, 6, 200, 26), newestConf);  // newest votes sec 54

    CHECK(v.locked());
    CHECK(v.votedField(TimeFrameVoter::FieldYear) == 26);   // value unaffected
    CHECK(v.votedField(TimeFrameVoter::FieldHours) == 6);
    const float q = v.lockConfidence();
    CHECK(q > 0.0f);
    CHECK(q <= 0.35f);   // participation ~0.291 caps it; round-3 read ~1.0
}

// [wwv.voter.unanimous-fade] WS-4.5 regression for the 2026-07-20 live false
// lock (decoded 2006-01-01 at q100): a deep fade zero-biased the SAME date bits
// in EVERY window frame — doy 201 -> 001 (s36/s38/s40 lost), year 26 -> 06
// (s52 lost) — so the wrongness was unanimous: margin 1.0, full participation,
// and the disagreement-based quality metric certified confident garbage at
// ~1.0. With the eligibility floor (minBitConfidence) noise-grade bits stop
// voting entirely: participation collapses, the affected fields' trust goes to
// ~0, and the minLockQuality gate refuses the lock.
void sectionVoterUnanimousFadeNoConfidentGarbage() {
    constexpr float C  = 0.35f;   // healthy bits
    constexpr float LO = 0.02f;   // deep-fade margin — noise-grade

    auto fadedFrame = [](int mn) {
        return wwvNoisyFrame(mn, 3, 201, 26, C,
                             {{36, ClockSymbol::Zero, LO},
                              {38, ClockSymbol::Zero, LO},
                              {40, ClockSymbol::Zero, LO},
                              {52, ClockSymbol::Zero, LO}});
    };

    // (a) With the production floors armed the unanimously-faded window must
    //     not lock, and quality must say why.
    TimeFrameVoter::Config hardened = wwvVoterConfig();
    hardened.minBitConfidence = 0.05f;
    hardened.minLockQuality   = 0.10f;
    TimeFrameVoter v(hardened);
    for (int mn = 45; mn <= 52; ++mn) {
        const auto [syms, conf] = fadedFrame(mn);
        v.addFrame(syms, conf);
    }
    CHECK(!v.locked());                 // the 2006-01-01 event must not lock
    CHECK(v.lockConfidence() < 0.10f);
    // WS-7: the refusal is attributable — the quality floor said no.
    CHECK(v.lockRefusal() == ClockLockRefusal::QualityFloor);

    // (b) The same window through a floors-off voter locks at high quality —
    //     pinning that the gate is what kills the live failure mode.
    TimeFrameVoter legacy(wwvVoterConfig());
    for (int mn = 45; mn <= 52; ++mn) {
        const auto [syms, conf] = fadedFrame(mn);
        legacy.addFrame(syms, conf);
    }
    CHECK(legacy.locked());
    CHECK(legacy.lockConfidence() > 0.9f);
    CHECK(legacy.lockRefusal() == ClockLockRefusal::None);   // locked -> None

    // (c) The floors must not break a CLEAN window: same minutes, no fade.
    TimeFrameVoter cleanV(hardened);
    std::array<float, 60> conf;
    conf.fill(C);
    for (int mn = 45; mn <= 52; ++mn)
        cleanV.addFrame(wwvVoterFrame(mn, 3, 201, 26), conf);
    CHECK(cleanV.locked());
    CHECK(cleanV.votedField(TimeFrameVoter::FieldDoy)  == 201);
    CHECK(cleanV.votedField(TimeFrameVoter::FieldYear) == 26);
    CHECK(cleanV.lockConfidence() > 0.5f);
}

// [wwv.voter.plausibility] Absolute-plausibility leg (WS-4.5): even a
// confident, unanimous, self-consistent decode must not lock when the voted
// timestamp is implausibly far from the reference clock. Systematic misreads
// are self-consistent by construction — the reference is the only independent
// evidence. The bound is generous: a host minutes or hours wrong still
// measures; decades cannot lock.
void sectionVoterPlausibilityGate() {
    constexpr float C = 0.35f;
    std::array<float, 60> conf;
    conf.fill(C);

    const TimeFields ref{50, 3, 201, 26};   // "host now": 03:50 doy 201 yr 26

    TimeFrameVoter::Config gated = wwvVoterConfig();
    gated.plausibilityBoundMinutes = 24 * 60;
    gated.referenceNow = [ref] { return ref; };

    // (a) Confident garbage 20 years out (the live event): must not lock.
    TimeFrameVoter garbage(gated);
    for (int mn = 45; mn <= 52; ++mn)
        garbage.addFrame(wwvVoterFrame(mn, 3, 1, 6), conf);   // 2006-01-01
    CHECK(!garbage.locked());
    // WS-7: the refusal is attributable — the plausibility gate said no.
    CHECK(garbage.lockRefusal() == ClockLockRefusal::Plausibility);

    // (b) A host ~40 minutes wrong is WITHIN the bound — measuring that error
    //     is the applet's purpose, so this must still lock.
    TimeFrameVoter hostSlow(gated);
    for (int mn = 10; mn <= 17; ++mn)
        hostSlow.addFrame(wwvVoterFrame(mn, 3, 201, 26), conf);
    CHECK(hostSlow.locked());
    CHECK(hostSlow.votedField(TimeFrameVoter::FieldMinutes) == 17);
    CHECK(hostSlow.lockRefusal() == ClockLockRefusal::None);

    // (c) Gate disarmed (bound 0, the default): back-compat is explicit.
    TimeFrameVoter ungated(wwvVoterConfig());
    for (int mn = 45; mn <= 52; ++mn)
        ungated.addFrame(wwvVoterFrame(mn, 3, 1, 6), conf);
    CHECK(ungated.locked());

    // (d) Bound edge: one day out (1438 min at the newest frame) locks at a
    //     24 h bound; two days out does not.
    TimeFrameVoter atEdge(gated);
    for (int mn = 45; mn <= 52; ++mn)
        atEdge.addFrame(wwvVoterFrame(mn, 3, 200, 26), conf);
    CHECK(atEdge.locked());
    TimeFrameVoter pastEdge(gated);
    for (int mn = 45; mn <= 52; ++mn)
        pastEdge.addFrame(wwvVoterFrame(mn, 3, 199, 26), conf);
    CHECK(!pastEdge.locked());

    // (e) setPlausibility arms the gate on a default-constructed config too.
    TimeFrameVoter armed(wwvVoterConfig());
    armed.setPlausibility([ref] { return ref; }, 24 * 60);
    for (int mn = 45; mn <= 52; ++mn)
        armed.addFrame(wwvVoterFrame(mn, 3, 1, 6), conf);
    CHECK(!armed.locked());
}

// [wwv.voter.dead-reckoning] Garbage frames must not let the voter free-run on
// the stale survivors: pre-WS-4.5, a range-invalid frame (the all-zeros decode
// a misaligned window produces; doy 0 is invalid) vanished from the normalized
// window entirely, so the aged good frames kept extrapolating +1 min per
// garbage frame at q1.00 — advancing timestamps with zero live support (the
// live receiver did exactly this after its window drifted). The staleness
// bound caps the free-run; participation accounting drags the quality down.
void sectionVoterNoDeadReckoning() {
    constexpr float C = 0.35f;
    std::array<float, 60> conf;
    conf.fill(C);

    TimeFrameVoter v(wwvVoterConfig());
    for (int mn = 10; mn <= 14; ++mn)
        v.addFrame(wwvVoterFrame(mn, 6, 200, 26), conf);
    CHECK(v.locked());

    // Confident garbage: every second Zero at margin C -> minute 0, hour 0,
    // doy 0 (range-invalid) — the all-zeros shape a misaligned window decodes.
    std::array<ClockSymbol, 60> zeros{};
    zeros.fill(ClockSymbol::Zero);
    int locksAfterGarbage = 0;
    for (int g = 0; g < 6; ++g) {
        v.addFrame(zeros, conf);
        if (v.locked()) ++locksAfterGarbage;
    }
    CHECK(!v.locked());                  // stale window must not stay locked
    CHECK(locksAfterGarbage <= 3);       // free-run bounded by the staleness age
    // WS-7: the refusal is attributable — the staleness bound said no.
    CHECK(v.lockRefusal() == ClockLockRefusal::Staleness);
}

// [wwv.voter.refusal-tags] WS-7: lockRefusal() distinguishes "still
// collecting" (None) from an actual gate refusal, and tags the disagreement
// gates as Contested. locked()/lockRefusal() read one verdict pass, so a
// locked voter is always None and a refused one is never unexplained.
void sectionVoterRefusalTags() {
    constexpr float C = 0.35f;
    std::array<float, 60> conf;
    conf.fill(C);

    // (a) Collecting: one frame is below minFramesForLock — not a refusal.
    TimeFrameVoter collecting(wwvVoterConfig());
    collecting.addFrame(wwvVoterFrame(10, 6, 200, 26), conf);
    CHECK(!collecting.locked());
    CHECK(collecting.frameCount() == 1);
    CHECK(collecting.lockRefusal() == ClockLockRefusal::None);

    // (b) Contested: two frames whose minutes do not chain (+1) — the window
    //     disagrees with itself.
    TimeFrameVoter contested(wwvVoterConfig());
    contested.addFrame(wwvVoterFrame(10, 6, 200, 26), conf);
    contested.addFrame(wwvVoterFrame(25, 6, 200, 26), conf);
    CHECK(!contested.locked());
    CHECK(contested.lockRefusal() == ClockLockRefusal::Contested);

    // (c) Locked: a clean incrementing window refuses nothing.
    TimeFrameVoter clean(wwvVoterConfig());
    for (int mn = 10; mn <= 13; ++mn)
        clean.addFrame(wwvVoterFrame(mn, 6, 200, 26), conf);
    CHECK(clean.locked());
    CHECK(clean.lockRefusal() == ClockLockRefusal::None);
}

// ---- WS-4.5 decoder-level hardening ---------------------------------------

// [wwv.demote] Lock must not survive the signal: after a clean lock, dead air
// must demote the decoder out of Locked. (Live 2026-07-20: the applet pinned
// Locked q100 for 45 minutes of stale decode — nothing in the decoder could
// ever lower the state once set.)
void sectionLockDemotesOnSignalLoss() {
    SynthOpts opts;
    opts.numFrames = 5;
    const std::vector<float> clean = synthWwv(kGold, opts);

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, clean, 512);
    CHECK(dec.state() == ClockLockState::Locked);       // precondition

    const std::vector<float> silence(static_cast<size_t>(kFs) * 300, 0.0f);
    feedChunks(dec, silence, 512);
    CHECK(dec.state() != ClockLockState::Locked);       // demoted, not pinned
}

// [wwv.gap] A sample-stream discontinuity (dropped DAX buffers) must not
// permanently derail the decoder: pre-WS-4.5 the tick phase and frame anchor
// were estimated once and frozen, so a 300 ms gap misaligned every subsequent
// window — confident systematic misreads forever (the live white-vs-blue
// drift). Structural re-validation detects it, soft reacquire + the leaky
// fold re-lock on the CURRENT phase, and the decoder is locked on truth again
// by the end.
void sectionGapRealignsAndRelocks() {
    SynthOpts opts;
    opts.numFrames = 18;
    opts.leadOutSeconds = 30;
    const Truth t{10, 6, 200, 26, -3, true, false, true};
    const std::vector<float> full = synthWwv(t, opts);

    // 10 s lead-in + 5.5 frames -> cut 300 ms mid-minute.
    const size_t cutAt = static_cast<size_t>(kFs) * (10 + 5 * 60 + 30);
    const size_t gap = static_cast<size_t>(kFs) * 3 / 10;

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    dec.process(full.data(), cutAt);
    CHECK(dec.state() == ClockLockState::Locked);        // locked pre-gap

    const std::vector<float> tail(full.begin() + static_cast<long>(cutAt + gap),
                                  full.end());
    feedChunks(dec, tail, 512);

    CHECK(dec.state() == ClockLockState::Locked);        // re-locked post-heal
    CHECK(!cap.times.empty());
    const ClockTimeInfo& last = cap.times.back();
    CHECK(last.hour == 6);
    CHECK(last.doy == 200);
    CHECK(last.year2 == 26);
    CHECK(last.minute >= t.minute + 11 && last.minute <= t.minute + 17);
    CHECK(last.quality > 0.1f);

    // The re-lock must be LIVE decode, not dead reckoning: post-heal FRAMES
    // (raw, pre-voting) must decode the on-air truth again. (The old decoder
    // decoded all-zeros forever here while the voter free-ran — a voted time
    // in range proved nothing by itself.)
    bool liveDecodePostGap = false;
    for (const ClockFrameInfo& f : cap.frames) {
        if (f.minute >= t.minute + 11 && f.minute <= t.minute + 17 &&
            f.hour == 6 && f.doy == 200 && f.year2 == 26) {
            liveDecodePostGap = true;
        }
    }
    CHECK(liveDecodePostGap);
}

// [wwv.drift] Slow sample-clock drift must be ABSORBED, not accumulated into
// misreads: +5 ms of stream slip every 20 s (~250 ppm, far beyond any real
// DAX rate offset) from minute 3 on. The old 80 ms matched-filter cap froze
// the alignment and turned accumulated drift into the live receiver's
// systematic misreads; the tracked delay estimate must follow the drift,
// keep the decode exact and hold the lock the whole way.
void sectionSlowDriftHoldsLock() {
    SynthOpts opts;
    opts.numFrames = 10;
    opts.leadOutSeconds = 20;
    const Truth t{30, 6, 200, 26, -3, true, false, true};
    const std::vector<float> base = synthWwv(t, opts);

    std::vector<float> drifty;
    drifty.reserve(base.size() + base.size() / 100);
    const size_t insEvery = static_cast<size_t>(kFs) * 20;
    size_t nextIns = static_cast<size_t>(kFs) * (10 + 3 * 60);
    for (size_t i = 0; i < base.size(); ++i) {
        if (i == nextIns) {
            drifty.insert(drifty.end(), 120, 0.0f);   // +5 ms of stream slip
            nextIns += insEvery;
        }
        drifty.push_back(base[i]);
    }

    WwvDecoder dec(kFs);
    Capture cap; wire(dec, cap);
    feedChunks(dec, drifty, 512);

    CHECK(dec.state() == ClockLockState::Locked);
    CHECK(!cap.times.empty());
    const ClockTimeInfo& last = cap.times.back();
    CHECK(last.minute == t.minute + opts.numFrames - 1);   // exact to the end
    CHECK(last.hour == 6);
    CHECK(last.doy == 200);
    CHECK(last.year2 == 26);

    // The absorbed drift must not have cost the lock even transiently.
    bool sawLock = false, demotedAfterLock = false;
    for (ClockLockState s : cap.states) {
        if (s == ClockLockState::Locked) sawLock = true;
        else if (sawLock) demotedAfterLock = true;
    }
    CHECK(sawLock);
    CHECK(!demotedAfterLock);

    // Edge labels must TRACK the drift, not the frozen window grid: the span
    // between frame s0 edges of minute 30 and minute 39 covers 9 broadcast
    // minutes PLUS the 18-19 slips inserted between them (the insertion at the
    // exact minute-39 boundary is ±1). The old fixed-grid labels missed by the
    // full accumulated drift (~90-110 ms); the compensated labels stay within
    // a few series samples. (Constant chain-delay bias cancels in the span.)
    const ClockFrameInfo* f30 = nullptr;
    const ClockFrameInfo* f39 = nullptr;
    for (const ClockFrameInfo& f : cap.frames) {
        if (f.minute == 30 && !f30) f30 = &f;
        if (f.minute == 39) f39 = &f;
    }
    CHECK(f30 != nullptr && f39 != nullptr);
    if (f30 && f39) {
        const long long trueSpan = 9LL * 60 * kFs + 18LL * 120;
        const long long measured = f39->frameStartSample - f30->frameStartSample;
        const long long tolerance = 6LL * (kFs / 200) + 120;   // 30 ms + slip ambiguity
        CHECK(std::llabs(measured - trueSpan) <= tolerance);
    }
}

// [wwv.plumb-plausibility] setPlausibility reaches the shared voter: a signal
// broadcasting a date 20 years from the reference must never lock, while the
// same signal locks against a matching reference. (Gate semantics are
// unit-tested at the voter level; this pins the decoder plumbing.)
void sectionDecoderPlausibilityPlumbing() {
    SynthOpts opts;
    opts.numFrames = 4;
    const Truth wrongEra{20, 6, 1, 6, 0, false, false, false};   // 2006-001
    const std::vector<float> sig = synthWwv(wrongEra, opts);

    WwvDecoder rej(kFs);
    rej.setPlausibility([] { return TimeFields{50, 3, 201, 26}; }, 24 * 60);
    Capture capR; wire(rej, capR);
    feedChunks(rej, sig, 512);
    CHECK(rej.state() != ClockLockState::Locked);
    CHECK(capR.times.empty());

    WwvDecoder acc(kFs);
    acc.setPlausibility([] { return TimeFields{21, 6, 1, 6}; }, 24 * 60);
    Capture capA; wire(acc, capA);
    feedChunks(acc, sig, 512);
    CHECK(acc.state() == ClockLockState::Locked);
    CHECK(!capA.times.empty());
}

} // namespace

int main() {
    std::mt19937 rng(0xC0FFEEu);             // fixed seed -> reproducible AWGN

    // Build the clean golden vector once; reuse across sections + WAV dump.
    SynthOpts opts;
    const std::vector<float> clean = synthWwv(kGold, opts);

    sectionCleanBitExact(clean);
    sectionWwvhStation();
    sectionSnr20(clean, rng);
    sectionNoiseOnlyNeverLocks(rng);
    sectionCorruptMarkersNoFalseLock();
    sectionTruncatedNoPartial();
    sectionDecadeDegeneracy(clean);
    sectionConfidenceFloor(clean);
    sectionVoterRollover();
    sectionVoterNoisyCorpus();
    sectionVoterMinuteFadeWeighting();
    sectionVoterContestedBitQuality();
    sectionVoterDoy366Gate();
    sectionVoterFrankenstein();
    sectionVoterPhantomRotating();
    sectionVoterLoneVoterParticipation();
    sectionVoterUnanimousFadeNoConfidentGarbage();
    sectionVoterPlausibilityGate();
    sectionVoterNoDeadReckoning();
    sectionVoterRefusalTags();
    sectionLockDemotesOnSignalLoss();
    sectionGapRealignsAndRelocks();
    sectionSlowDriftHoldsLock();
    sectionDecoderPlausibilityPlumbing();

    // Print encoded truth for the python cross-check gate.
    std::printf("golden truth: min=%d hr=%d doy=%d yr=%d frames=%d\n",
                kGold.minute, kGold.hour, kGold.doy, kGold.year2, opts.numFrames);
    std::printf("golden extra: dut1_tenths=%d dst1=%d dst2=%d lsw=%d station=WWV\n",
                kGold.dut1Tenths, kGold.dst1 ? 1 : 0, kGold.dst2 ? 1 : 0,
                kGold.lsw ? 1 : 0);

    // WAV dump for the cross-check gate: 16-bit PCM stereo (dup mono), 24 kHz.
    if (const char* dir = std::getenv("AETHERCLOCK_DUMP_WAV_DIR")) {
        const std::string path = std::string(dir) + "/wwv_golden.wav";
        writeWavStereo(path, clean);
        std::fprintf(stderr, "wrote golden vector: %s (%zu samples)\n",
                     path.c_str(), clean.size());
    }

    if (g_failures == 0) {
        std::printf("wwv_decoder_test: all checks passed\n");
        return 0;
    }
    std::printf("wwv_decoder_test: %d checks FAILED\n", g_failures);
    return 1;
}
