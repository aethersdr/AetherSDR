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
// engine ingests float32 INTERLEAVED STEREO (the daxAudioReady payload), so
// each mono sample is duplicated L=R into the QByteArray and fed in ~200 ms
// blocks. A fake host clock is injected and advanced per block so the decoded
// second edge can be compared against a known skew.

#include "core/AetherClockEngine.h"
#include "core/PanadapterStream.h"
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

    // NB: named emitRange, not `emit` — this file includes QtCore, which
    // #defines `emit` to nothing (WS-1 could use `emit` as it pulls no Qt).
    auto emitRange = [&](std::array<Sym,60> sym, int secStart, int secEnd) {
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
void feedStereo(AetherClockEngine& eng, int channel, const std::vector<float>& mono,
                qint64 epochMs, qint64 skewMs,
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
        eng.feedRxAudio(channel, block);
        QCoreApplication::processEvents();
    }
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

    slice.setDaxChannel(3);                    // engine reacquires new-before-old
    QCoreApplication::processEvents();
    CHECK(stream.daxChannelHeldBy(3, Clock::Clock));   // hold moved to 3
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));  // released 2

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

    delete slice;                              // graceful-loss handler fires
    QCoreApplication::processEvents();

    CHECK(!engine.isRunning());
    CHECK(engine.lockState() == ClockLockState::NoSignal);
    CHECK(!stream.daxChannelHeldBy(2, Clock::Clock));  // no orphaned hold
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

    if (g_failures == 0) {
        std::printf("aetherclock_engine_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherclock_engine_test: %d checks FAILED\n", g_failures);
    return 1;
}
