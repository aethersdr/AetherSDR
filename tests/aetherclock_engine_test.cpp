// AetherClock WS-2 — AetherClockEngine integration test (plain main() + CHECK,
// NOT QtTest; harness idiom per tests/amp_model_test.cpp + SPEC "Repo
// conventions"). Drives the ENGINE through its public header contract only:
// setPanadapterStream / setHostClock / setLockDecayTimeoutMs / start / stop /
// applyStationPreset / feedRxAudio, observing the six engine signals via
// QSignalSpy. The decoder
// .cpp is authored in parallel; this file asserts engine contracts, never
// decoder internals.
//
// The WWV signal synthesizer helpers below are COPIED (not included) from
// tests/wwv_decoder_test.cpp — the gate-passed WS-1 vector generator. Only the
// clean-signal path is reused (no AWGN / WAV writer / decoder driver): the
// engine ingests float32 INTERLEAVED STEREO (the daxPcmReady payload), so
// each mono sample is duplicated L=R into the QByteArray and fed in ~200 ms
// blocks. A fake host clock is injected and advanced per block so the decoded
// second edge can be compared against a known skew.

#include "core/AetherClockEngine.h"
#include "core/ClockSampleTimeline.h"
#include "core/backends/flex/PanadapterStream.h"
#include "core/TimeFrameVoter.h"
#include "models/SliceModel.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QList>
#include <QSignalSpy>
#include <QTime>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace AetherSDR;

// ---- test harness (per SPEC "Repo conventions") ---------------------------
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int    kFs = 24000;              // pinned DAX RX sample rate

// ===========================================================================
// WWV time-code synthesizer — COPIED verbatim from tests/wwv_decoder_test.cpp
// (WS-1). Levels pinned by SPEC §"Signal synthesizer spec": 24 kHz; carrier
// 1000 Hz @ 0.5; 100 Hz subcarrier depth 0.30 pulse-on / 0.06 pulse-off / 0 in
// the s0 minute hole; pulse per the NIST 170/470/770 ms @ +30 ms encoding; 5 ms
// tick @ 0.25 at the 2000 Hz image (WWVH: 2200 Hz). Field map per NIST SP 432.
// ===========================================================================

// Per-second pulse class of the 100 Hz subcarrier.
enum class Sym { Hole, Zero, One, Marker };

// Broadcast truth for one minute; the synthesizer encodes ALL of these.
struct Truth {
    int  minute = 0, hour = 0, doy = 0, year2 = 0;
    int  dut1Tenths = 0;   // signed tenths (e.g. -3 => -0.3 s)
    bool dst1 = false;     // WWV s2
    bool dst2 = false;     // WWV s55
    bool lsw  = false;     // WWV s3 (leap-second warning)
};

struct SynthOpts {
    int          sampleRateHz = kFs;
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
void appendSecond(std::vector<float>& sig, Sym sym, int tickFreq, int sampleRateHz) {
    for (int k = 0; k < sampleRateHz; ++k) {
        const double t   = static_cast<double>(sig.size()) / sampleRateHz;
        const double tau = static_cast<double>(k) / sampleRateHz;
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

    // NB: named emitRange, not `emit` — this file includes QtCore, which
    // #defines `emit` to nothing (WS-1 could use `emit` as it pulls no Qt).
    auto emitRange = [&](std::array<Sym,60> sym, int secStart, int secEnd) {
        for (int sec = secStart; sec < secEnd; ++sec) {
            Sym s = sym[sec];
            if (o.corruptMarkers && s == Sym::Marker) s = Sym::Zero; // flatten markers
            if (o.removeHole && sec == 0)             s = Sym::Zero; // fill the hole
            appendSecond(sig, s, tickFreq, o.sampleRateHz);
        }
    };

    // Lead-in: the tail seconds of the prior minute, correctly encoded.
    Truth lead = start;
    lead.minute -= 1;
    if (lead.minute < 0) { lead.minute += 60; lead.hour = (lead.hour + 23) % 24; }
    emitRange(encodeMinute(lead), 60 - o.leadInSeconds, 60);

    // Consecutive full frames, minutes monotonically incrementing.
    for (int i = 0; i < o.numFrames; ++i) {
        Truth cur = start;
        const int total = start.minute + i;
        cur.minute = total % 60;
        cur.hour   = (start.hour + total / 60) % 24;
        int lastSec = 60;
        if (o.truncateSeconds >= 0 && i == o.numFrames - 1) lastSec = o.truncateSeconds;
        emitRange(encodeMinute(cur), 0, lastSec);
    }

    // Lead-out: the head seconds of the NEXT minute so the final frame closes.
    if (o.truncateSeconds < 0 && o.leadOutSeconds > 0) {
        Truth next = start;
        const int total = start.minute + o.numFrames;
        next.minute = total % 60;
        next.hour   = (start.hour + total / 60) % 24;
        emitRange(encodeMinute(next), 0, o.leadOutSeconds);
    }
    return sig;
}

// ===========================================================================
// WS-2 test scaffolding
// ===========================================================================

// The golden broadcast truth: 06:20 -> 06:21 -> 06:22 on doy 200, yr 26.
// Matches WS-1's kGold so the voter's "newest frame" minute is 22.
const Truth   kGold{20, 6, 200, 26, /*dut1*/ -3, /*dst1*/ true, /*dst2*/ false, /*lsw*/ true};
constexpr int kExpectNewestMin = 22;        // start.minute + numFrames - 1
constexpr qint64 kSkewMs = 400;             // host clock skewed AHEAD -> offset ~ -400

// UTC (ms since epoch) of synth sample 0 = the first lead-in sample, i.e. the
// frame-0 broadcast minute minus the lead-in seconds. With the fake host clock
// defined as (epoch + samplesFed/24 kHz + skew), the host reads exactly
// true-broadcast-time + skew at every sample, so a correctly disciplined engine
// reports offsetMs = decodedUtc - hostUtc ~ -skew at the decoded edge.
qint64 synthEpochMs(const Truth& g, const SynthOpts& o) {
    const int year = 2000 + g.year2;
    const QDate d  = QDate(year, 1, 1).addDays(g.doy - 1);            // doy is 1-based
    const QDateTime frame0(d, QTime(g.hour, g.minute, 0), QTimeZone::utc());
    return frame0.addSecs(-o.leadInSeconds).toMSecsSinceEpoch();
}

// Feed `mono` to the engine as ~200 ms float32 interleaved-STEREO blocks on
// `channel`, duplicating each mono sample into L and R (the engine downmixes
// mono = 0.5*(L+R), recovering the original). When `advanceClock`, *fakeNow is
// updated per block to model host = epoch + samplesFed/24 kHz + skew at the END
// of the block (the anchor point). processEvents() drains any queued paths.
void feedStereoVia(const std::function<void(const QByteArray&)>& sink,
                   const std::vector<float>& mono, qint64 epochMs, qint64 skewMs,
                   qint64& samplesFed, qint64& fakeNow, bool advanceClock) {
    constexpr size_t kBlockFrames = 4800;   // 200 ms @ 24 kHz
    for (size_t i = 0; i < mono.size(); i += kBlockFrames) {
        const size_t n = std::min(kBlockFrames, mono.size() - i);
        QByteArray block;
        block.resize(static_cast<int>(n * 2 * sizeof(float)));
        auto* out = reinterpret_cast<float*>(block.data());
        for (size_t k = 0; k < n; ++k) {
            out[2 * k]     = mono[i + k];   // L
            out[2 * k + 1] = mono[i + k];   // R == L
        }
        samplesFed += static_cast<qint64>(n);
        if (advanceClock)
            fakeNow = epochMs + samplesFed * 1000 / kFs + skewMs;
        sink(block);
        QCoreApplication::processEvents();
    }
}

// Channel-keyed feed (Flex / daxPcmReady).
void feedStereo(AetherClockEngine& eng, int channel, const std::vector<float>& mono,
                qint64 epochMs, qint64 skewMs,
                qint64& samplesFed, qint64& fakeNow, bool advanceClock) {
    feedStereoVia([&eng, channel](const QByteArray& b) { eng.feedRxAudio(channel, b); },
                  mono, epochMs, skewMs, samplesFed, fakeNow, advanceClock);
}

// Slice-keyed feed (seam-native / backendSliceAudioFrameReady). Same payload
// bytes, different slot — that parity is part of what these tests assert.
void feedStereoSlice(AetherClockEngine& eng, int sliceId, const std::vector<float>& mono,
                     qint64 epochMs, qint64 skewMs,
                     qint64& samplesFed, qint64& fakeNow, bool advanceClock) {
    feedStereoVia([&eng, sliceId](const QByteArray& b) { eng.feedRxSliceAudio(sliceId, b); },
                  mono, epochMs, skewMs, samplesFed, fakeNow, advanceClock);
}

using Clock = PanadapterStream::DaxConsumer;   // ::Clock == time-signal consumer

// Wire the engine's injected DAX-hold provider to a REAL central registry
// (PanadapterStream), per the amended header: the engine drives these lambdas
// with the bound slice's channel; they acquire/release under DaxConsumer::Clock
// so stream.daxChannelHeldBy(ch, Clock) observes the exact same holds the
// production wiring layer would create.
void wireProvider(AetherClockEngine& eng, PanadapterStream& stream) {
    eng.setDaxChannelProvider(
        [&stream](int ch) { stream.acquireDaxChannel(ch, Clock::Clock); },
        [&stream](int ch) { stream.releaseDaxChannel(ch, Clock::Clock); });
}

// Did any recorded lockStateChanged carry the Locked state? (Value read via the
// direct getter to avoid enum-from-QVariant fragility; count proves transitions.)
bool sawLocked(const QSignalSpy& lockSpy, const AetherClockEngine& eng) {
    return eng.lockState() == ClockLockState::Locked && lockSpy.count() >= 1;
}

// ==== test sections ========================================================

// [1] Happy path: start(slice @ dax 2, Wwv), feed >=3 clean synth frames on
// ch 2 -> timeDecoded fires; utc == truth; offset ~ -skew; quality high; Locked.
void sectionHappyPath() {
    SynthOpts opts;                                   // 3 frames, lead-in/out 10 s
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    qint64 fakeNow = epochMs + kSkewMs;               // host at sample 0
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyTime(&engine, &AetherClockEngine::timeDecoded);
    QSignalSpy spyLock(&engine, &AetherClockEngine::lockStateChanged);
    QSignalSpy spyLocked(&engine, &AetherClockEngine::lockedChanged);
    QSignalSpy spyRunning(&engine, &AetherClockEngine::runningChanged);

    engine.start(&slice, ClockStation::Wwv);
    CHECK(engine.isRunning());
    CHECK(spyRunning.count() >= 1);
    CHECK(engine.lockState() == ClockLockState::NoSignal);   // starts NoSignal

    qint64 samplesFed = 0;
    feedStereo(engine, 2, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);

    // timeDecoded fired, reached Locked.
    CHECK(!spyTime.isEmpty());
    CHECK(sawLocked(spyLock, engine));
    bool lockedTrue = false;
    for (int i = 0; i < spyLocked.count(); ++i)
        if (spyLocked.at(i).at(0).toBool()) lockedTrue = true;
    CHECK(lockedTrue);

    if (!spyTime.isEmpty()) {
        const QList<QVariant> last = spyTime.back();
        const QDateTime utc   = last.at(0).toDateTime().toUTC();
        const double    offMs = last.at(1).toDouble();
        const int       qual  = last.at(2).toInt();

        // utc == synth truth: correct date/hour and the newest voted minute.
        // At the frame boundary where onTime fires, the most recent edge may
        // already sit in the next broadcast minute, so a disciplined engine may
        // report minute 22 (newest voted frame) or 23 (edge minute) — both are
        // real synth-truth instants. The offset assertion below is what pins
        // the decodedUtc<->host alignment. See test report: cross-task risk R1.
        CHECK(utc.isValid());
        CHECK(utc.date() == QDate(2026, 1, 1).addDays(kGold.doy - 1));
        CHECK(utc.time().hour() == 6);
        CHECK(utc.time().minute() == kExpectNewestMin ||
              utc.time().minute() == kExpectNewestMin + 1);

        // Host clock skewed +skew ahead of broadcast -> host is BEHIND -> the
        // engine's offset (decodedUtc - hostUtc) is NEGATIVE and ~ -skew.
        CHECK(std::abs(offMs - (-static_cast<double>(kSkewMs))) <= 60.0);

        // Quality high: WS-1's documented clean-lock floor is 0.40 -> >= 40/100.
        CHECK(qual >= 40 && qual <= 100);
    }

    engine.stop();
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
}

// [2] DAX filter: feed the same signal tagged channel 3 while the slice is
// still dax 2 -> zero signals, state stays NoSignal.
void sectionDaxFilter() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyTime(&engine, &AetherClockEngine::timeDecoded);
    QSignalSpy spyLock(&engine, &AetherClockEngine::lockStateChanged);
    QSignalSpy spyAlign(&engine, &AetherClockEngine::alignmentFrame);

    engine.start(&slice, ClockStation::Wwv);
    qint64 samplesFed = 0;
    feedStereo(engine, 3, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);

    CHECK(spyTime.isEmpty());                 // wrong channel -> nothing decoded
    CHECK(spyAlign.isEmpty());
    CHECK(spyLock.isEmpty());                 // no state change away from NoSignal
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    engine.stop();
}

// [3] Hold lifecycle (INV-3): after start the slice's dax channel is held by
// the clock consumer; after stop() the hold is gone; start/stop never leak.
void sectionHoldLifecycle() {
    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);

    for (int cycle = 0; cycle < 3; ++cycle) {
        engine.start(&slice, ClockStation::Wwv);
        CHECK(engine.isRunning());
        CHECK(stream.daxChannelHeldBy(2, Clock::Clock));      // held while running
        engine.stop();
        CHECK(!engine.isRunning());
        CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));     // released on stop
    }
    // No leak on any channel after the cycles.
    for (int ch = 1; ch <= 4; ++ch)
        CHECK(!stream.daxChannelHeldBy(ch, Clock::Clock));
}

// [4] DAX reassign: while running, setDaxChannel(3) -> hold moves 2 -> 3; audio
// fed on 3 is accepted, on 2 ignored.
void sectionDaxReassign() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    engine.start(&slice, ClockStation::Wwv);
    CHECK(stream.daxChannelHeldBy(2, Clock::Clock));

    // A selected source already has a complete vote and diagnostics history.
    // Moving DAX must retire these with its input conversion history.
    qint64 beforeChangeSamples = 0;
    feedStereo(engine, 2, mono, epochMs, kSkewMs, beforeChangeSamples, fakeNow, true);
    CHECK(engine.lockState() == ClockLockState::Locked);
    CHECK(engine.currentDiagnostics().framesInWindow > 0);
    CHECK(engine.currentDiagnostics().classifiedPct > 0);

    slice.setDaxChannel(3);                    // engine reacquires new-before-old
    QCoreApplication::processEvents();
    CHECK(stream.daxChannelHeldBy(3, Clock::Clock));   // hold moved to 3
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));  // released 2

    // DAX RX runs 1-8 on the radios that have it, and the decoder route and the
    // holder registry both span that range. A slice parked on 5-8 must take a
    // real hold here too, not fall through to "no channel assigned".
    for (int high : {5, 6, 7, 8}) {
        slice.setDaxChannel(high);
        QCoreApplication::processEvents();
        CHECK(stream.daxChannelHeldBy(high, Clock::Clock));
    }
    slice.setDaxChannel(3);
    QCoreApplication::processEvents();
    CHECK(stream.daxChannelHeldBy(3, Clock::Clock));
    for (int high : {5, 6, 7, 8}) {
        CHECK(!stream.daxChannelHeldBy(high, Clock::Clock));
    }
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    CHECK(engine.currentDiagnostics().classifiedPct == 0);

    // Audio on the NEW channel is accepted (alignment frames flow).
    QSignalSpy spyAlign(&engine, &AetherClockEngine::alignmentFrame);
    qint64 samplesFed = 0;
    feedStereo(engine, 3, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);
    CHECK(!spyAlign.isEmpty());                        // ch 3 accepted post-reassign

    // Audio on the OLD channel is now ignored.
    const int alignAfterCh3 = spyAlign.count();
    QByteArray oneBlock;
    oneBlock.resize(static_cast<int>(4800 * 2 * sizeof(float)));
    auto* out = reinterpret_cast<float*>(oneBlock.data());
    for (size_t k = 0; k < 4800; ++k) { out[2 * k] = mono[k]; out[2 * k + 1] = mono[k]; }
    engine.feedRxAudio(2, oneBlock);
    QCoreApplication::processEvents();
    CHECK(spyAlign.count() == alignAfterCh3);          // ch 2 ignored

    engine.stop();
}

// [5] Slice removal: heap-allocate a slice, start, delete it, processEvents ->
// isRunning() false, lockState NoSignal, no hold remains, no crash.
void sectionSliceRemoval() {
    PanadapterStream stream;
    AetherClockEngine engine;
    wireProvider(engine, stream);

    auto* slice = new SliceModel(0);
    slice->setDaxChannel(2);

    QSignalSpy spyRunning(&engine, &AetherClockEngine::runningChanged);
    engine.start(slice, ClockStation::Wwv);
    CHECK(engine.isRunning());
    CHECK(stream.daxChannelHeldBy(2, Clock::Clock));

    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    qint64 fakeNow = epochMs + kSkewMs;
    qint64 samplesFed = 0;
    engine.setHostClock([&fakeNow] { return fakeNow; });
    feedStereo(engine, 2, mono, epochMs, kSkewMs, samplesFed, fakeNow, true);
    CHECK(engine.currentDiagnostics().classifiedPct > 0);

    delete slice;                              // graceful-loss handler fires
    QCoreApplication::processEvents();

    CHECK(!engine.isRunning());
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));  // no orphaned hold
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
}

// [6] applyStationPreset tunes the BOUND slice: Wwv/10.0 -> 9.999 MHz USB;
// Wwvb/0.060 -> 0.059 MHz, AGC off.
void sectionStationPreset() {
    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.start(&slice, ClockStation::Wwv);   // bind the slice

    engine.applyStationPreset(ClockStation::Wwv, 10.0);
    CHECK(std::abs(slice.frequency() - 9.999) < 1e-9);
    CHECK(slice.mode() == QStringLiteral("USB"));

    engine.applyStationPreset(ClockStation::Wwvb, 0.060);
    CHECK(std::abs(slice.frequency() - 0.059) < 1e-9);
    CHECK(slice.agcMode() == QStringLiteral("off"));

    engine.stop();
}

// [7] Statics: preset lists exactly as frozen.
void sectionStatics() {
    const QVector<double> wwv = AetherClockEngine::wwvCarrierFrequenciesMHz();
    const QVector<double> expect{2.5, 5.0, 10.0, 15.0, 20.0};
    CHECK(wwv.size() == expect.size());
    if (wwv.size() == expect.size())
        for (int i = 0; i < wwv.size(); ++i)
            CHECK(std::abs(wwv[i] - expect[i]) < 1e-9);

    CHECK(std::abs(AetherClockEngine::wwvbCarrierFrequencyMHz() - 0.060) < 1e-9);
    CHECK(std::abs(AetherClockEngine::listeningDialMHz(10.0) - 9.999) < 1e-9);
    CHECK(std::abs(AetherClockEngine::listeningDialMHz(0.060) - 0.059) < 1e-9);
}

// [8] applyStationPreset(SliceModel*, ...) overload: tunes ANY given slice
// without binding it or starting the engine; a locked slice refuses the whole
// preset (all-or-nothing). This is the applet's Tune-while-stopped path.
void sectionStationPresetOverload() {
    AetherClockEngine engine;   // never started; nothing bound, no provider

    // Unlocked WWV: dial = carrier - 1 kHz, USB. The engine stays stopped and
    // unbound (the overload touches only the slice it is handed).
    SliceModel wwv(0);
    engine.applyStationPreset(&wwv, ClockStation::Wwv, 10.0);
    CHECK(std::abs(wwv.frequency() - 9.999) < 1e-9);
    CHECK(wwv.mode() == QStringLiteral("USB"));
    CHECK(!engine.isRunning());
    CHECK(engine.boundSliceId() == -1);

    // Unlocked WWVB additionally forces AGC off on that slice.
    SliceModel wwvb(1);
    engine.applyStationPreset(&wwvb, ClockStation::Wwvb, 0.060);
    CHECK(std::abs(wwvb.frequency() - 0.059) < 1e-9);
    CHECK(wwvb.mode() == QStringLiteral("USB"));
    CHECK(wwvb.agcMode() == QStringLiteral("off"));

    // Locked slice: the lock check refuses the preset, so nothing changes.
    SliceModel locked(2);
    locked.setFrequency(14.0);
    locked.setMode(QStringLiteral("LSB"));
    locked.setLocked(true);
    engine.applyStationPreset(&locked, ClockStation::Wwv, 10.0);
    CHECK(std::abs(locked.frequency() - 14.0) < 1e-9);
    CHECK(locked.mode() == QStringLiteral("LSB"));
}

// [9] Lock-decay watchdog at the NoSignal floor: after start() the state is
// NoSignal and the watchdog fires repeatedly (60 ms) but must NEVER emit —
// there is nothing below NoSignal to demote to.
void sectionLockDecayNoSignalStable() {
    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.setLockDecayTimeoutMs(60);   // well under the 200 ms spin below

    QSignalSpy spyLock(&engine, &AetherClockEngine::lockStateChanged);
    engine.start(&slice, ClockStation::Wwv);
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    // wait() spins an event loop so the QTimer can fire; it returns false when
    // no lockStateChanged arrives within the window — exactly what we want at
    // the floor. No audio is fed, so no second re-arms toward a real state.
    CHECK(!spyLock.wait(200));
    CHECK(spyLock.isEmpty());
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    engine.stop();
}

// [10] Lock-decay watchdog demotes a stalled lock one step at a time. Reach
// Locked on clean synth audio, then stop feeding: with a 60 ms timeout the
// engine-side watchdog walks Locked -> Acquiring -> NoSignal and then stops
// re-arming at the floor. (The handleSecond resync — which re-pulls the
// decoder's live state after a decay — is exercised implicitly by the happy
// path, where fast feeding re-arms every classified second and any transient
// decay self-heals back to the decoder's Locked; live QA covers the on-air
// re-emit-after-decay recovery directly.)
void sectionLockDecayDemotes() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.setLockDecayTimeoutMs(60);
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyLock(&engine, &AetherClockEngine::lockStateChanged);
    engine.start(&slice, ClockStation::Wwv);

    qint64 samplesFed = 0;
    feedStereo(engine, 2, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);
    CHECK(sawLocked(spyLock, engine));
    CHECK(engine.lockState() == ClockLockState::Locked);   // no more audio flows

    // With feeding stopped the 60 ms watchdog decays the stale lock stepwise.
    // Each wait() spins the loop until the next demotion edge.
    CHECK(spyLock.wait(2000));                              // Locked -> Acquiring
    CHECK(engine.lockState() == ClockLockState::Acquiring);
    CHECK(spyLock.wait(2000));                              // Acquiring -> NoSignal
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    // At the floor the watchdog stops re-arming: no further edges.
    CHECK(!spyLock.wait(300));
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    engine.stop();
}

// [WS-7] Acquisition telemetry: currentDiagnostics() stages flip on the happy
// path, frameDecoded re-emits every completed frame (previously discarded at
// the engine boundary), the classified-seconds ring reports a full last
// minute, the refusal tag is None once locked, and the ~1 Hz timer emission
// fires while running.
void sectionDiagnosticsTelemetry() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    slice.setDaxChannel(2);

    AetherClockEngine engine;
    wireProvider(engine, stream);
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyFrame(&engine, &AetherClockEngine::frameDecoded);
    QSignalSpy spyDiag(&engine, &AetherClockEngine::diagnosticsUpdated);

    // Not running: default-constructed snapshot.
    {
        const ClockDiagnostics d0 = engine.currentDiagnostics();
        CHECK(!d0.toneDetected);
        CHECK(d0.framesInWindow == 0);
        CHECK(d0.classifiedPct == 0);
    }

    engine.start(&slice, ClockStation::Wwv);
    qint64 samplesFed = 0;
    feedStereo(engine, 2, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);
    CHECK(engine.lockState() == ClockLockState::Locked);

    const ClockDiagnostics d = engine.currentDiagnostics();
    // stages 1-2: tick fold locked on the synth signal; delay settled.
    CHECK(d.toneDetected);
    CHECK(d.phaseLocked);
    CHECK(d.toneSnrDb > 0.0f);
    CHECK(std::isfinite(d.delayEstMs));
    // stage 3
    CHECK(d.anchored);
    CHECK(d.badFrameStreak == 0);
    // stage 4: the fake clock advanced with the feed, so the last 60 s of host
    // time carry a full minute of classified seconds.
    CHECK(d.classifiedPct >= 90);
    // stage 5
    CHECK(d.framesInWindow >= 2);
    CHECK(d.windowSize == 8);
    CHECK(d.voteQuality > 0.0f);
    CHECK(d.refusalReason == quint8(ClockLockRefusal::None));

    // frameDecoded re-emission: one per completed synth frame (3 frames), with
    // the raw fields intact.
    CHECK(spyFrame.count() >= 3);
    if (spyFrame.count() >= 1) {
        const auto fi = spyFrame.back().at(0).value<ClockFrameInfo>();
        CHECK(fi.station == ClockStation::Wwv);
        CHECK(fi.frameConfidence > 0.0f);
        CHECK(fi.minute >= 0 && fi.minute <= 59);
    }

    // ~1 Hz emission while running (wall-clock timer; one wait is enough).
    CHECK(spyDiag.count() >= 1 || spyDiag.wait(1500));

    engine.stop();
    const ClockDiagnostics dStop = engine.currentDiagnostics();
    CHECK(dStop.framesInWindow == 0);   // decoder torn down -> defaults
    CHECK(dStop.classifiedPct == 0);    // ring cleared
}

// [12] Seam-native per-slice ingest — the Hermes-Lite 2 shape: a bound slice
// with NO DAX channel, on a radio that declares no DAX plane at all. Asserts
// the sibling slot accepts the bound slice id and rejects any other, that a
// dax==0 slice decodes fine through it, and that nothing is acquired on a
// DAX registry that does not exist here.
void sectionSeamSliceAudio() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    CHECK(slice.daxChannel() == 0);        // HL2: none assigned, none ever will be

    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.setDaxAvailabilityProvider([] { return false; });   // no DAX plane
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyAlign(&engine, &AetherClockEngine::alignmentFrame);
    QSignalSpy spyTime(&engine, &AetherClockEngine::timeDecoded);

    engine.start(&slice, ClockStation::Wwv);
    CHECK(engine.isRunning());
    for (int ch = 1; ch <= 4; ++ch)        // nothing held; there is nothing to hold
        CHECK(!stream.daxChannelHeldBy(ch, Clock::Clock));

    // Another slice's audio is ignored.
    qint64 samplesFed = 0;
    feedStereoSlice(engine, 1, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);
    CHECK(spyAlign.isEmpty());
    CHECK(spyTime.isEmpty());

    // The bound slice decodes — on a slice whose daxChannel() is 0. The decoder
    // consumed nothing above, so the fake clock rewinds with the feed.
    samplesFed = 0;
    fakeNow = epochMs + kSkewMs;
    feedStereoSlice(engine, 0, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);
    CHECK(!spyAlign.isEmpty());
    CHECK(!spyTime.isEmpty());

    engine.stop();
}

// [13] Flex regression guard for the channel-keyed slot. A Flex slice may sit
// at daxChannel()==0 (unassigned) while the radio's DAX plane is very much
// alive. Loosening the filter to `want != 0 && channel != want` — the tempting
// way to make HL2 work without a sibling slot — would make THIS slice accept
// every other slice's DAX audio, which is exactly what the filter exists to
// prevent. Nothing may be accepted here.
void sectionDaxZeroChannelStillFilters() {
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);

    PanadapterStream stream;
    SliceModel slice(0);
    CHECK(slice.daxChannel() == 0);        // unassigned, on a DAX-capable radio

    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.setDaxAvailabilityProvider([] { return true; });    // Flex: DAX exists
    qint64 fakeNow = epochMs + kSkewMs;
    engine.setHostClock([&fakeNow] { return fakeNow; });

    QSignalSpy spyAlign(&engine, &AetherClockEngine::alignmentFrame);
    QSignalSpy spyTime(&engine, &AetherClockEngine::timeDecoded);

    engine.start(&slice, ClockStation::Wwv);
    qint64 samplesFed = 0;
    feedStereo(engine, 1, mono, epochMs, kSkewMs, samplesFed, fakeNow, /*advance*/ true);

    CHECK(spyAlign.isEmpty());             // ch 1 MUST NOT reach a dax==0 slice
    CHECK(spyTime.isEmpty());
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    engine.stop();
}

// Typed producer fixtures exercise the production engine ingress. No audio
// devices, radio connections, peers or mutable global sample rates are used.
PcmFrame feedTyped(AetherClockEngine& engine, PcmProducer& producer,
                   const std::vector<float>& mono, PcmFormat format,
                   qint64 epochMs, qint64& fakeNow, quint64 generation,
                   int key = 5, bool dax = false, bool queued = false,
                   quint64 firstSample = 12345)
{
    const std::array<std::size_t, 7> chunks{1, 255, 17, 4093, 1024, 31, 8192};
    PcmFrame last;
    std::size_t offset = 0;
    std::size_t part = 0;
    while (offset < mono.size()) {
        const std::size_t n = std::min(chunks[part++ % chunks.size()], mono.size() - offset);
        QVector<float> samples(static_cast<qsizetype>(n * format.channels()));
        for (std::size_t i = 0; i < n; ++i) {
            if (format.layout == PcmLayout::Mono) {
                samples[static_cast<qsizetype>(i)] = mono[offset + i];
            } else {
                samples[static_cast<qsizetype>(2 * i)] = mono[offset + i] * 0.75f;
                samples[static_cast<qsizetype>(2 * i + 1)] = mono[offset + i] * 1.25f;
            }
        }
        const auto frame = producer.produce(std::move(samples), firstSample + offset, offset == 0);
        CHECK(frame.has_value());
        if (!frame) {
            return {};
        }
        last = *frame;
        offset += n;
        fakeNow = epochMs + kSkewMs + static_cast<qint64>(offset * 1000 / format.sampleRateHz);
        const auto deliver = [&engine, frame = *frame, generation, key, dax] {
            if (dax) {
                engine.feedRxAudio(key, frame, generation);
            } else {
                engine.feedRxSliceAudio(key, frame, generation);
            }
        };
        if (queued) {
            QMetaObject::invokeMethod(&engine, deliver, Qt::QueuedConnection);
        } else {
            deliver();
        }
    }
    return last;
}

void sectionTypedRateTiming()
{
    std::array<double, 4> offsets{};
    std::size_t trial = 0;
    for (int rate : {24000, 48000}) {
        SynthOpts opts;
        opts.sampleRateHz = rate;
        const std::vector<float> mono = synthWwv(kGold, opts);
        for (PcmLayout layout : {PcmLayout::Mono, PcmLayout::Stereo}) {
            SliceModel slice(5); // Sparse live id, not a channel or ordinal.
            AetherClockEngine engine;
            engine.setDaxAvailabilityProvider([] { return false; });
            engine.setDaxChannelProvider([](int) {}, [](int) {});
            qint64 fakeNow = synthEpochMs(kGold, opts) + kSkewMs;
            engine.setHostClock([&fakeNow] { return fakeNow; });
            engine.start(&slice, ClockStation::Wwv);
            PcmProducer producer;
            const PcmFormat format{rate, layout};
            CHECK(producer.start(PcmPurpose::Slice, 5, format));
            QSignalSpy time(&engine, &AetherClockEngine::timeDecoded);
            feedTyped(engine, producer, mono, format, synthEpochMs(kGold, opts),
                      fakeNow, engine.inputGeneration(), 5, false, false,
                      std::numeric_limits<quint64>::max() - mono.size() - 1024);
            CHECK(engine.lockState() == ClockLockState::Locked);
            CHECK(!time.isEmpty());
            if (!time.isEmpty()) {
                offsets[trial] = time.back().at(1).toDouble();
                CHECK(std::abs(offsets[trial] + kSkewMs) <= 60.0);
                CHECK(time.back().at(0).toDateTime().date()
                      == QDate(2026, 1, 1).addDays(kGold.doy - 1));
            }
            ++trial;
        }
    }
    // WWV's 200 Hz classifier quantizes edges at5ms. This relative check
    // catches omitted/wrong-sign48k converter delay (~70.58ms) independently
    // of the existing detector's absolute alignment tolerance.
    for (double offset : offsets) {
        CHECK(std::abs(offset - offsets[0]) < 8.0);
    }
}

void sectionTypedLifetime()
{
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    qint64 fakeNow = epochMs + kSkewMs;
    auto slice = std::make_unique<SliceModel>(5);
    AetherClockEngine engine;
    engine.setDaxAvailabilityProvider([] { return false; });
    engine.setDaxChannelProvider([](int) {}, [](int) {});
    engine.setHostClock([&fakeNow] { return fakeNow; });
    engine.start(slice.get(), ClockStation::Wwv);
    PcmProducer producer;
    CHECK(producer.start(PcmPurpose::Slice, 5));
    const quint64 oldGeneration = engine.inputGeneration();
    // These events have never crossed the engine's replay gate. Their producer
    // stays live across restart, so only binding generation can reject them.
    feedTyped(engine, producer, mono, {}, epochMs, fakeNow, oldGeneration, 5, false, true);
    engine.stop();
    engine.start(slice.get(), ClockStation::Wwv);
    QCoreApplication::processEvents();
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    CHECK(engine.lockState() == ClockLockState::NoSignal);

    // A fresh forward segment on the same producer is legitimate after restart.
    const quint64 next = 12345 + mono.size();
    PcmFrame last = feedTyped(engine, producer, mono, {}, epochMs, fakeNow,
                             engine.inputGeneration(), 5, false, false, next);
    CHECK(engine.lockState() == ClockLockState::Locked);
    QSignalSpy time(&engine, &AetherClockEngine::timeDecoded);
    const int classified = engine.currentDiagnostics().classifiedPct;
    engine.feedRxSliceAudio(5, last, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::Locked);
    CHECK(engine.currentDiagnostics().classifiedPct == classified);
    CHECK(time.isEmpty());

    // A wrong slot, speaker-purpose frame or another still-live producer must
    // not reset the current receiver, let alone feed its detector.
    for (PcmPurpose purpose : {PcmPurpose::Speaker, PcmPurpose::Slice}) {
        PcmProducer other;
        CHECK(other.start(purpose, purpose == PcmPurpose::Slice ? 5 : -1));
        const auto frame = other.produce(QVector<float>{0.1f, 0.1f});
        CHECK(frame.has_value());
        engine.feedRxSliceAudio(5, *frame, engine.inputGeneration());
        CHECK(engine.lockState() == ClockLockState::Locked);
    }
    engine.feedRxSliceAudio(3, last, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::Locked);

    // An accepted48k frame too short to make an output batch still retires the
    // detector, votes and diagnostics immediately on the format transition.
    CHECK(producer.setFormat({48000, PcmLayout::Mono}));
    const auto tiny = producer.produce(QVector<float>{0.1f});
    CHECK(tiny.has_value());
    engine.feedRxSliceAudio(5, *tiny, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    engine.feedRxSliceAudio(5, last, engine.inputGeneration()); // revoked24
    CHECK(engine.currentDiagnostics().classifiedPct == 0);

    CHECK(producer.setFormat({24000, PcmLayout::Stereo}));
    last = feedTyped(engine, producer, mono, {}, epochMs, fakeNow, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::Locked);
    const auto missed = producer.produce(QVector<float>(512, 0.0f));
    const auto afterGap = producer.produce(QVector<float>{0.0f, 0.0f});
    CHECK(missed && afterGap);
    engine.feedRxSliceAudio(5, *afterGap, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    engine.feedRxSliceAudio(5, last, engine.inputGeneration()); // replay after reset
    CHECK(engine.currentDiagnostics().classifiedPct == 0);

    // Same-slot replacement cannot revive the old still-live source. Its new
    // source is admitted without waiting for the old object's producer to die.
    slice.reset();
    CHECK(!engine.isRunning());
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    slice = std::make_unique<SliceModel>(5);
    engine.start(slice.get(), ClockStation::Wwv);
    const auto stale = producer.produce(QVector<float>{0.1f, 0.1f});
    engine.feedRxSliceAudio(5, *stale, engine.inputGeneration());
    PcmProducer replacement;
    CHECK(replacement.start(PcmPurpose::Slice, 5));
    feedTyped(engine, replacement, mono, {}, epochMs, fakeNow, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::Locked);
}

void sectionTimeline()
{
    ClockSampleTimeline timeline;
    CHECK(timeline.anchor(1001, 5801, 48000, 3389, 1000000));
    CHECK(std::abs(timeline.hostMsAtSample(2400) - 999929.3958333333) < 0.000001);
    CHECK(timeline.anchor(std::numeric_limits<quint64>::max() - 4800,
                          std::numeric_limits<quint64>::max(), 48000, 3389, 1000000));
    CHECK(std::abs(timeline.hostMsAtSample(2400) - 999929.3958333333) < 0.000001);
    // End includes one unprocessed source frame: its half-output-frame time
    // stays in the mapping without rounding sample positions per callback.
    CHECK(timeline.anchor(500, 761, 48000, 0, 2000));
    CHECK(std::abs(timeline.hostMsAtSample(128) - 1999.8958333333) < 0.000001);
    CHECK(timeline.anchor(9, 24009, 24000, 0, 1000));
    CHECK(timeline.hostMsAtSample(12000) == 500.0);
    CHECK(!timeline.anchor(10, 9, 24000, 0, 1000));
    CHECK(!timeline.anchor(0, 10, 48000, -1, 1000));
}

void sectionTypedDaxSelection()
{
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    qint64 fakeNow = epochMs + kSkewMs;
    PanadapterStream stream;
    SliceModel slice(5);
    slice.setDaxChannel(2);
    AetherClockEngine engine;
    wireProvider(engine, stream);
    engine.setHostClock([&fakeNow] { return fakeNow; });
    engine.start(&slice, ClockStation::Wwv);
    PcmProducer channel2;
    PcmProducer channel3;
    CHECK(channel2.start(PcmPurpose::Auxiliary));
    CHECK(channel3.start(PcmPurpose::Auxiliary));
    const PcmFrame last2 = feedTyped(engine, channel2, mono, {}, epochMs, fakeNow,
                                     engine.inputGeneration(), 2, true);
    CHECK(engine.lockState() == ClockLockState::Locked);
    feedTyped(engine, channel3, mono, {}, epochMs, fakeNow,
              engine.inputGeneration(), 3, true, true);
    slice.setDaxChannel(3);
    CHECK(stream.daxChannelHeldBy(3, Clock::Clock));
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
    QCoreApplication::processEvents();
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    feedTyped(engine, channel3, mono, {}, epochMs, fakeNow,
              engine.inputGeneration(), 3, true, false, 12345 + mono.size());
    CHECK(engine.lockState() == ClockLockState::Locked);
    slice.setDaxChannel(2);
    engine.feedRxAudio(2, last2, engine.inputGeneration());
    CHECK(engine.currentDiagnostics().classifiedPct == 0);
    CHECK(engine.currentDiagnostics().framesInWindow == 0);
    feedTyped(engine, channel2, mono, {}, epochMs, fakeNow,
              engine.inputGeneration(), 2, true, false, 12345 + mono.size());
    CHECK(engine.lockState() == ClockLockState::Locked);
    engine.stop();
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
    CHECK(!stream.daxChannelHeldBy(3, Clock::Clock));
}

void sectionReentrantClockOutput()
{
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    for (int action = 0; action < 4; ++action) {
        SliceModel slice(5);
        AetherClockEngine engine;
        engine.setDaxAvailabilityProvider([] { return false; });
        engine.setDaxChannelProvider([](int) {}, [](int) {});
        qint64 fakeNow = epochMs + kSkewMs;
        engine.setHostClock([&fakeNow] { return fakeNow; });
        engine.start(&slice, ClockStation::Wwv);
        PcmProducer producer;
        CHECK(producer.start(PcmPurpose::Slice, 5));
        feedTyped(engine, producer, mono, {}, epochMs, fakeNow, engine.inputGeneration());
        CHECK(engine.lockState() == ClockLockState::Locked);
        QSignalSpy alignment(&engine, &AetherClockEngine::alignmentFrame);
        QMetaObject::Connection change;
        change = QObject::connect(&engine, &AetherClockEngine::alignmentFrame, &engine,
                                  [&] {
            QObject::disconnect(change);
            if (action == 0) {
                producer.invalidate();
            } else if (action == 1) {
                // Restart inside decoder::process. Its old instance must stay
                // alive until that call returns, and publish no further events.
                engine.start(&slice, ClockStation::Wwv);
            } else if (action == 2) {
                slice.setFrequency(9.999);
            } else {
                slice.setMode(QStringLiteral("AM"));
            }
        });
        QVector<float> samples(65536 * 2);
        for (int i = 0; i < 65536; ++i) {
            samples[2 * i] = samples[2 * i + 1] = mono[static_cast<std::size_t>(i)];
        }
        const auto frame = producer.produce(std::move(samples));
        CHECK(frame.has_value());
        fakeNow += 2731;
        engine.feedRxSliceAudio(5, *frame, engine.inputGeneration());
        CHECK(alignment.count() == 1);
        CHECK(engine.isRunning());
        CHECK(engine.lockState() == ClockLockState::NoSignal);
        CHECK(engine.currentDiagnostics().classifiedPct == 0);
        CHECK(engine.currentDiagnostics().framesInWindow == 0);
    }
}

void sectionDeleteDuringClockSignal()
{
    // Immediate QObject deletion is legal from a direct signal observer. The
    // executing decoder and callback state must survive until processing exits.
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    qint64 fakeNow = epochMs + kSkewMs;
    SliceModel slice(5);
    for (int signalKind = 0; signalKind < 4; ++signalKind) {
        auto engine = std::make_unique<AetherClockEngine>();
        engine->setDaxAvailabilityProvider([] { return false; });
        engine->setDaxChannelProvider([](int) {}, [](int) {});
        engine->setHostClock([&fakeNow] { return fakeNow; });
        engine->setLockDecayTimeoutMs(60);
        if (signalKind == 0) {
            QObject::connect(engine.get(), &AetherClockEngine::sourceGenerationChanged,
                             &slice, [&](quint64) { engine.reset(); });
            engine->start(&slice, ClockStation::Wwv);
            CHECK(!engine);
            continue;
        }
        engine->start(&slice, ClockStation::Wwv);
        PcmProducer producer;
        CHECK(producer.start(PcmPurpose::Slice, 5));
        feedTyped(*engine, producer, mono, {}, epochMs, fakeNow, engine->inputGeneration());
        CHECK(engine->lockState() == ClockLockState::Locked);
        if (signalKind == 1) {
            QObject::connect(engine.get(), &AetherClockEngine::lockStateChanged,
                             &slice, [&](ClockLockState) { engine.reset(); });
            engine->stop();
        } else if (signalKind == 2) {
            QObject::connect(engine.get(), &AetherClockEngine::alignmentFrame,
                             &slice, [&](const ClockAlignmentFrame&) { engine.reset(); });
            QVector<float> samples(65536 * 2);
            for (int i = 0; i < 65536; ++i) {
                samples[2 * i] = samples[2 * i + 1] = mono[static_cast<std::size_t>(i)];
            }
            const auto frame = producer.produce(std::move(samples));
            CHECK(frame.has_value());
            fakeNow += 2731;
            engine->feedRxSliceAudio(5, *frame, engine->inputGeneration());
        } else {
            QSignalSpy stateChanged(engine.get(), &AetherClockEngine::lockStateChanged);
            QObject::connect(engine.get(), &AetherClockEngine::lockStateChanged,
                             &slice, [&](ClockLockState) { engine.reset(); });
            CHECK(stateChanged.wait(2000)); // Watchdog callback owns no public-call stack.
        }
        CHECK(!engine);
    }
}

void sectionStartSupersession()
{
    // A direct generation observer can supersede the operation that emitted it.
    // Its final selection/running state must survive the outer start returning.
    for (int action = 0; action < 3; ++action) {
        auto selected = std::make_unique<SliceModel>(5);
        SliceModel replacement(6);
        AetherClockEngine engine;
        engine.setDaxAvailabilityProvider([] { return false; });
        engine.setDaxChannelProvider([](int) {}, [](int) {});
        QSignalSpy running(&engine, &AetherClockEngine::runningChanged);
        QMetaObject::Connection change;
        change = QObject::connect(&engine, &AetherClockEngine::sourceGenerationChanged,
                                  &engine, [&](quint64) {
            QObject::disconnect(change);
            if (action == 0) {
                engine.stop();
            } else if (action == 1) {
                selected.reset();
            } else {
                engine.start(&replacement, ClockStation::Wwvb);
            }
        });
        engine.start(selected.get(), ClockStation::Wwv);
        CHECK(engine.isRunning() == (action == 2));
        CHECK(engine.boundSliceId() == (action == 2 ? 6 : -1));
        CHECK(running.count() == (action == 2 ? 2 : 1));
        CHECK(!running.isEmpty());
        if (!running.isEmpty()) {
            CHECK(running.back().at(0).toBool() == (action == 2));
        }
        if (action == 2) {
            CHECK(engine.configuredStation() == ClockStation::Wwvb);
        }
    }
}

void sectionResetSupersession()
{
    SynthOpts opts;
    const std::vector<float> mono = synthWwv(kGold, opts);
    const qint64 epochMs = synthEpochMs(kGold, opts);
    qint64 fakeNow = epochMs + kSkewMs;
    SliceModel selected(5);
    SliceModel replacement(6);
    AetherClockEngine engine;
    engine.setDaxAvailabilityProvider([] { return false; });
    engine.setDaxChannelProvider([](int) {}, [](int) {});
    engine.setHostClock([&fakeNow] { return fakeNow; });
    engine.start(&selected, ClockStation::Wwv);
    PcmProducer first;
    PcmProducer second;
    CHECK(first.start(PcmPurpose::Slice, 5));
    CHECK(second.start(PcmPurpose::Slice, 6));
    feedTyped(engine, first, mono, {}, epochMs, fakeNow, engine.inputGeneration());
    CHECK(engine.lockState() == ClockLockState::Locked);
    QMetaObject::Connection change;
    change = QObject::connect(&engine, &AetherClockEngine::lockStateChanged, &engine,
                              [&](ClockLockState state) {
        if (state != ClockLockState::NoSignal) {
            return;
        }
        QObject::disconnect(change);
        engine.start(&replacement, ClockStation::Wwv);
        feedTyped(engine, second, mono, {}, epochMs, fakeNow, engine.inputGeneration(), 6);
        CHECK(engine.lockState() == ClockLockState::Locked);
    });
    // Resetting the old detector emits NoSignal inline, before ingest anchors
    // this gap block. The callback establishes a complete replacement context.
    const auto gap = first.produce(QVector<float>{0.1f, 0.1f}, 12345 + mono.size() + 100, true);
    CHECK(gap.has_value());
    engine.feedRxSliceAudio(5, *gap, engine.inputGeneration());
    CHECK(engine.boundSliceId() == 6);
    CHECK(engine.isRunning());
    CHECK(engine.lockState() == ClockLockState::Locked);
    CHECK(engine.currentDiagnostics().classifiedPct > 0);
    CHECK(engine.currentDiagnostics().framesInWindow > 0);
}

void sectionStopSupersession()
{
    for (bool fromProvider : {false, true}) {
        PanadapterStream stream;
        SliceModel selected(5);
        SliceModel replacement(6);
        selected.setDaxChannel(2);
        replacement.setDaxChannel(3);
        AetherClockEngine engine;
        bool restartOnRelease = false;
        engine.setDaxChannelProvider(
            [&stream](int ch) { stream.acquireDaxChannel(ch, Clock::Clock); },
            [&](int ch) {
                stream.releaseDaxChannel(ch, Clock::Clock);
                if (restartOnRelease) {
                    restartOnRelease = false;
                    engine.start(&replacement, ClockStation::Wwv);
                }
            });
        engine.start(&selected, ClockStation::Wwv);
        CHECK(stream.daxChannelHeldBy(2, Clock::Clock));
        QMetaObject::Connection change;
        if (fromProvider) {
            restartOnRelease = true;
        } else {
            change = QObject::connect(&engine, &AetherClockEngine::sourceGenerationChanged,
                                      &engine, [&](quint64) {
                QObject::disconnect(change);
                engine.start(&replacement, ClockStation::Wwv);
            });
        }
        engine.stop();
        CHECK(engine.isRunning());
        CHECK(engine.boundSliceId() == 6);
        CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
        CHECK(stream.daxChannelHeldBy(3, Clock::Clock));
        const quint64 generation = engine.inputGeneration();
        selected.setFrequency(9.999);
        selected.setMode(QStringLiteral("AM"));
        selected.setDaxChannel(4);
        CHECK(engine.inputGeneration() == generation);
        CHECK(!stream.daxChannelHeldBy(4, Clock::Clock));
        CHECK(stream.daxChannelHeldBy(3, Clock::Clock));
        engine.stop();
        CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
        CHECK(!stream.daxChannelHeldBy(3, Clock::Clock));
        CHECK(!stream.daxChannelHeldBy(4, Clock::Clock));
    }
}

void sectionAcquireSupersession()
{
    for (int action = 0; action < 3; ++action) {
        PanadapterStream stream;
        SliceModel selected(5);
        SliceModel replacement(6);
        selected.setDaxChannel(2);
        replacement.setDaxChannel(4);
        auto engine = std::make_unique<AetherClockEngine>();
        bool changeOnAcquire = false;
        engine->setDaxChannelProvider(
            [&](int ch) {
                stream.acquireDaxChannel(ch, Clock::Clock);
                if (std::exchange(changeOnAcquire, false)) {
                    CHECK(stream.daxChannelHeldBy(2, Clock::Clock));
                    CHECK(stream.daxChannelHeldBy(3, Clock::Clock));
                    if (action == 0) {
                        engine->stop();
                    } else if (action == 1) {
                        engine->start(&replacement, ClockStation::Wwv);
                    } else {
                        engine.reset();
                    }
                }
            },
            [&](int ch) { stream.releaseDaxChannel(ch, Clock::Clock); });
        engine->start(&selected, ClockStation::Wwv);
        changeOnAcquire = true;
        selected.setDaxChannel(3);
        CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
        CHECK(!stream.daxChannelHeldBy(3, Clock::Clock));
        CHECK(stream.daxChannelHeldBy(4, Clock::Clock) == (action == 1));
        if (engine) {
            CHECK(engine->isRunning() == (action == 1));
            CHECK(engine->boundSliceId() == (action == 1 ? 6 : -1));
            engine->stop();
        }
        CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));
        CHECK(!stream.daxChannelHeldBy(3, Clock::Clock));
        CHECK(!stream.daxChannelHeldBy(4, Clock::Clock));
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // Register the queued-connection metatypes (the engine ctor also does this;
    // registration is idempotent and makes the QSignalSpy captures robust).
    qRegisterMetaType<AetherSDR::ClockAlignmentFrame>();
    qRegisterMetaType<AetherSDR::ClockLockState>("AetherSDR::ClockLockState");
    qRegisterMetaType<AetherSDR::ClockStation>("AetherSDR::ClockStation");

    sectionHappyPath();
    sectionDaxFilter();
    sectionHoldLifecycle();
    sectionDaxReassign();
    sectionSliceRemoval();
    sectionStationPreset();
    sectionStatics();
    sectionStationPresetOverload();
    sectionLockDecayNoSignalStable();
    sectionLockDecayDemotes();
    sectionDiagnosticsTelemetry();
    sectionSeamSliceAudio();
    sectionDaxZeroChannelStillFilters();
    sectionTypedRateTiming();
    sectionTypedLifetime();
    sectionTimeline();
    sectionTypedDaxSelection();
    sectionReentrantClockOutput();
    sectionDeleteDuringClockSignal();
    sectionStartSupersession();
    sectionResetSupersession();
    sectionStopSupersession();
    sectionAcquireSupersession();

    if (g_failures == 0) {
        std::printf("aetherclock_engine_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherclock_engine_test: %d checks FAILED\n", g_failures);
    return 1;
}
