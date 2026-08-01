#include "core/AudioEngine.h"
#include "core/WsprBeacon.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

using AetherSDR::WsprBeacon;
using AetherSDR::AudioEngine;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void testReferenceVector()
{
    // Canonical vector printed by Joe Taylor's GPL-3 WSPRcode utility and
    // published in the WSPR 3.0 User Guide, pp. 17-18.
    static constexpr std::array<uint8_t, WsprBeacon::kSymbolCount> expected{
        3,3,0,0,2,0,0,0,1,0,2,0,1,3,1,2,2,2,1,0,0,3,2,3,1,3,3,2,2,0,
        2,0,0,0,3,2,0,1,2,3,2,2,0,0,2,2,3,2,1,1,0,2,3,3,2,1,0,2,2,1,
        3,2,1,2,2,2,0,3,3,0,3,0,3,0,1,2,1,0,2,1,2,0,3,2,1,3,2,0,0,3,
        3,2,3,0,3,2,2,0,3,0,2,0,2,0,1,0,2,3,0,2,1,1,1,2,3,3,0,2,3,1,
        2,1,2,2,2,1,3,3,2,0,0,0,0,1,0,3,2,0,1,3,2,2,2,2,2,0,2,3,3,2,
        3,2,3,3,2,0,0,3,1,2,2,2
    };

    const WsprBeacon::EncodeResult result =
        WsprBeacon::encode(QStringLiteral("K1ABC"),
                           QStringLiteral("FN42"), 37);
    check(static_cast<bool>(result), "canonical message encodes");
    check(result.symbols == expected, "channel symbols match WSPRcode");
}

void testValidation()
{
    check(!WsprBeacon::encode(QStringLiteral("K1ABC/P"),
                              QStringLiteral("FN42"), 37),
          "compound callsign is rejected by type-1 encoder");
    check(!WsprBeacon::encode(QStringLiteral("K1ABC"),
                              QStringLiteral("ZZ99"), 37),
          "invalid Maidenhead locator is rejected");
    check(!WsprBeacon::encode(QStringLiteral("K1ABC"),
                              QStringLiteral("FN42"), 38),
          "non-standard WSPR power is rejected");
    // callsignCharacter() maps a space to 36, so an embedded space clears the
    // c3/c4/c5 >= 10 checks and would encode a call the operator never typed.
    check(!WsprBeacon::encode(QStringLiteral("K1A C"),
                              QStringLiteral("FN42"), 37),
          "embedded space in a callsign is rejected, not silently encoded");
    check(!WsprBeacon::encode(QStringLiteral("K1 BC"),
                              QStringLiteral("FN42"), 37),
          "embedded space before the suffix is rejected");
    check(!WsprBeacon::encode(QStringLiteral("K1-BC"),
                              QStringLiteral("FN42"), 37),
          "punctuation in a callsign is rejected");
    // Surrounding whitespace is still trimmed, and short calls still pad.
    check(static_cast<bool>(WsprBeacon::encode(QStringLiteral("  K1ABC  "),
                                               QStringLiteral("FN42"), 37)),
          "surrounding whitespace is trimmed, not treated as an embedded space");
    check(static_cast<bool>(WsprBeacon::encode(QStringLiteral("W1AW"),
                                               QStringLiteral("FN31"), 37)),
          "a short call that needs trailing pad spaces still encodes");
    check(static_cast<bool>(WsprBeacon::encode(QStringLiteral("VE3ABC"),
                                               QStringLiteral("FN25"), 37)),
          "a full six-character call still encodes");
}

void testSampleTimingAndTailSilence()
{
    check(WsprBeacon::kTransmitDurationMs == 111592,
          "one-second pre-roll plus WSPR frame lasts 111.592 seconds");
    check(WsprBeacon::framesForElapsedNanoseconds(21000000000LL)
              == 21LL * WsprBeacon::kSampleRate,
          "pacer advances exactly 21 seconds of samples after 21 seconds");
    check(WsprBeacon::framesForElapsedNanoseconds(111592000000LL)
              == WsprBeacon::kTotalFrames,
          "pacer reaches the exact end of the complete WSPR transmission");
    check(!WsprBeacon::isInterlockTimeoutSufficient(20000),
          "20-second radio timeout is rejected for WSPR");
    check(WsprBeacon::isInterlockTimeoutSufficient(0)
              && WsprBeacon::isInterlockTimeoutSufficient(120000),
          "disabled and 120-second radio timeouts allow WSPR");

    const WsprBeacon::EncodeResult encoded =
        WsprBeacon::encode(QStringLiteral("K1ABC"),
                           QStringLiteral("FN42"), 37);
    WsprBeacon beacon;
    beacon.prepare(WsprBeacon::kSampleRate);
    beacon.start(encoded.symbols, 1500.0, -20.0f, 10);

    std::vector<float> pcm(20, 1234.0f);
    beacon.process(pcm.data(), 10, 2);
    check(beacon.currentSymbol() == -1, "pre-roll remains before first symbol");
    for (const float sample : pcm) {
        check(sample == 0.0f, "pre-roll replaces microphone with silence");
    }

    pcm.assign(2, 0.0f);
    beacon.process(pcm.data(), 1, 2);
    check(beacon.currentSymbol() == 0, "first symbol starts after exact pre-roll");

    constexpr int remainingFrames =
        WsprBeacon::kSymbolCount * WsprBeacon::kFramesPerSymbol - 1;
    std::vector<float> block(4096 * 2);
    int remaining = remainingFrames;
    while (remaining > 0) {
        const int frames = std::min(remaining, 4096);
        beacon.process(block.data(), frames, 2);
        remaining -= frames;
    }
    check(beacon.isComplete(), "message completes after exactly 162 symbols");
    check(beacon.isActive(), "source stays active until explicit stop");

    block.assign(64, 1234.0f);
    beacon.process(block.data(), 32, 2);
    for (const float sample : block) {
        check(sample == 0.0f, "completed source holds silence until unkey");
    }
    beacon.stop();
    check(!beacon.isActive(), "explicit stop disables source");
}

void testTwentyOneSecondsDoesNotComplete()
{
    const WsprBeacon::EncodeResult encoded =
        WsprBeacon::encode(QStringLiteral("K1ABC"),
                           QStringLiteral("FN42"), 37);
    WsprBeacon beacon;
    beacon.prepare(WsprBeacon::kSampleRate);
    beacon.start(encoded.symbols, 1500.0, -20.0f);

    int64_t remaining =
        WsprBeacon::framesForElapsedNanoseconds(21000000000LL);
    std::vector<float> block(4096 * 2);
    while (remaining > 0) {
        const int frames = static_cast<int>(
            std::min<int64_t>(remaining, 4096));
        beacon.process(block.data(), frames, 2);
        remaining -= frames;
    }
    check(!beacon.isComplete(),
          "21 seconds cannot complete a 111.592-second WSPR transmission");
    check(beacon.currentSymbol() > 0
              && beacon.currentSymbol() < WsprBeacon::kSymbolCount,
          "21-second checkpoint remains inside the WSPR symbol stream");
}

void testIndependentDaxPump()
{
    const WsprBeacon::EncodeResult encoded =
        WsprBeacon::encode(QStringLiteral("K1ABC"),
                           QStringLiteral("FN42"), 37);
    AudioEngine engine;
    engine.setTxStreamId(0x4a000001U);
    engine.setTransmitting(true);
    engine.setRadioTransmitting(true);

    int packetCount = 0;
    QByteArray firstPacket;
    QObject::connect(&engine, &AudioEngine::txPacketReady,
                     [&packetCount, &firstPacket](const QByteArray& packet) {
        ++packetCount;
        if (firstPacket.isEmpty()) {
            firstPacket = packet;
        }
    });

    engine.wsprBeacon()->start(encoded.symbols, 1500.0, -20.0f, 0);
    engine.startWsprPump();
    QEventLoop loop;
    QTimer::singleShot(250, &loop, &QEventLoop::quit);
    loop.exec();
    engine.wsprBeacon()->stop();
    engine.stopWsprPump();

    check(packetCount >= 40 && packetCount <= 50,
          "independent 24 kHz DAX pump emits real-time VITA-49 packets");
    quint32 classWord = 0;
    if (firstPacket.size() >= 16) {
        std::memcpy(&classWord, firstPacket.constData() + 12, sizeof(classWord));
        classWord = qFromBigEndian(classWord);
    }
    check((classWord & 0xffffU) == 0x0123U,
          "independent WSPR pump uses the radio-native DAX packet class");
    check(engine.wsprBeacon()->currentSymbol() == -1,
          "stopping the independent pump resets generator state");
}

// Generate `frames` mono float frames with the pre-roll already spent.
std::vector<float> generate(const WsprBeacon::Symbols& symbols,
                            double toneZeroHz, float levelDb, int64_t frames,
                            int skipFrames = 0)
{
    WsprBeacon beacon;
    beacon.prepare(WsprBeacon::kSampleRate);
    beacon.start(symbols, toneZeroHz, levelDb, 0, skipFrames);
    std::vector<float> out(static_cast<size_t>(frames), 0.0f);
    int64_t done = 0;
    while (done < frames) {
        const int block =
            static_cast<int>(std::min<int64_t>(frames - done, 8192));
        beacon.process(out.data() + done, block, 1);
        done += block;
    }
    return out;
}

WsprBeacon::Symbols uniformSymbols(uint8_t value)
{
    WsprBeacon::Symbols symbols{};
    symbols.fill(value);
    return symbols;
}

// Positive-going zero crossings, which is one per cycle.
int countCycles(const std::vector<float>& pcm, int64_t from, int64_t to)
{
    int cycles = 0;
    for (int64_t i = from + 1; i < to; ++i) {
        if (pcm[static_cast<size_t>(i - 1)] <= 0.0f
            && pcm[static_cast<size_t>(i)] > 0.0f) {
            ++cycles;
        }
    }
    return cycles;
}

// Tone 0 must sit AT the requested frequency with the constellation running
// upward from it, as in WSJT-X (Modulator.cpp: m_frequency + itone*spacing).
//
// The arithmetic is exact and that is what makes this a sharp test: one symbol
// is 16384/24000 s and one tone step is 12000/8192 Hz, so each step adds
// exactly ONE cycle per symbol. At 1500 Hz an all-zeros frame is 1024 cycles
// per symbol on the nose, and all-threes is 1027.
//
// The superseded convention centred the four tones on the requested frequency,
// which put tone 0 at 1497.803 Hz — 1022.5 cycles, and every spot 2.2 Hz low.
void testToneConventionMatchesWsjtx()
{
    // Ten symbols, starting at symbol 1 so the head ramp is behind us. Ten
    // rather than one because a window edge can land on the crossing sample
    // itself and cost a count — a ±1 artifact against a 30-cycle signal.
    constexpr int kSymbols = 10;
    constexpr int64_t kFrom = WsprBeacon::kFramesPerSymbol;
    constexpr int64_t kTo = kFrom + kSymbols * WsprBeacon::kFramesPerSymbol;
    const std::vector<float> tone0 =
        generate(uniformSymbols(0), 1500.0, -6.0f, kTo);
    const std::vector<float> tone3 =
        generate(uniformSymbols(3), 1500.0, -6.0f, kTo);

    // 1500 Hz for ten symbols is 10240 cycles. The superseded centred
    // convention put tone 0 at 1497.803 Hz — 10225 — so this is 15 cycles
    // clear of the behaviour it replaced, not a rounding-width assertion.
    check(std::abs(countCycles(tone0, kFrom, kTo) - 10240) <= 1,
          "symbol 0 is emitted at the requested frequency, not below it");
    check(std::abs(countCycles(tone3, kFrom, kTo) - 10270) <= 1,
          "symbol 3 sits three tone spacings above the requested frequency");
}

// Both ends of the frame are tapered, so the transmission neither starts nor
// stops on a step discontinuity. WSJT-X fades its tail over 0.017 symbols
// (Modulator.cpp); the head ramp is ours, and costs 1.7% of one sync symbol.
void testFrameEndsAreTapered()
{
    const std::vector<float> pcm = generate(
        uniformSymbols(0), 1500.0, -20.0f, WsprBeacon::kMessageFrames);
    constexpr float kAmplitude = 0.1f;   // -20 dBFS

    const auto peakOver = [&pcm](int64_t from, int64_t to) {
        float peak = 0.0f;
        for (int64_t i = from; i < to; ++i) {
            peak = std::max(peak, std::abs(pcm[static_cast<size_t>(i)]));
        }
        return peak;
    };

    // Full amplitude through the body.
    const float body = peakOver(WsprBeacon::kFramesPerSymbol,
                                2LL * WsprBeacon::kFramesPerSymbol);
    check(body > kAmplitude * 0.99f && body <= kAmplitude * 1.001f,
          "body of the frame runs at the requested level");

    // Both ends land near -97 dB. Before the taper existed the final sample
    // was an arbitrary point on a full-scale sine — a step straight to zero,
    // which is a broadband click in a 200 Hz-wide shared sub-band.
    check(peakOver(0, 4) < kAmplitude * 1e-3f,
          "frame ramps up rather than starting at full amplitude");
    check(peakOver(WsprBeacon::kMessageFrames - 4,
                   WsprBeacon::kMessageFrames) < kAmplitude * 1e-3f,
          "frame is faded out rather than cut at an arbitrary phase");

    // And the taper is confined to the ends — a symbol boundary well inside
    // the frame must not be attenuated, since phase is continuous there.
    const float atBoundary = peakOver(
        80LL * WsprBeacon::kFramesPerSymbol - 64,
        80LL * WsprBeacon::kFramesPerSymbol + 64);
    check(atBoundary > kAmplitude * 0.99f,
          "interior symbol boundaries are not tapered");
}

// A slot boundary already missed truncates the HEAD of the frame rather than
// sliding the whole thing later, which is what WSJT-X does with m_ic. The
// head ramp still applies, because it follows the audio and not the symbol
// clock — a skipped start is still a start.
void testLateStartSkipsIntoTheFrame()
{
    constexpr int kSkip = WsprBeacon::kSampleRate;   // one second late
    const std::vector<float> pcm =
        generate(uniformSymbols(0), 1500.0, -20.0f, 64, kSkip);

    WsprBeacon beacon;
    beacon.prepare(WsprBeacon::kSampleRate);
    beacon.start(uniformSymbols(0), 1500.0, -20.0f, 0, kSkip);
    std::vector<float> one(1, 0.0f);
    beacon.process(one.data(), 1, 1);
    check(beacon.currentSymbol() == kSkip / WsprBeacon::kFramesPerSymbol,
          "a late start resumes at the symbol the slot clock is already on");

    float peak = 0.0f;
    for (int i = 0; i < 4; ++i) {
        peak = std::max(peak, std::abs(pcm[static_cast<size_t>(i)]));
    }
    check(peak < 0.1f * 1e-3f,
          "a skipped start is still ramped, not keyed at full amplitude");
}

// A skip deep enough to land inside the tail region must still end faded.
//
// updateBeaconState() cannot produce one — kBeaconMaxSlotLatenessMs caps the
// skip at 24000 frames against a 2654208-frame message — but start() is public
// and clamps only to kMessageFrames - 1, so the generator has to be correct on
// its own terms. If the head ramp took precedence, the envelope would climb for
// whatever frames remained and the message would then stop dead at that level:
// a step discontinuity, which is the click the taper exists to remove.
//
// Falsifiable by construction: put the `m_framesEmitted < kTaperFrames` branch
// back in front of the tail branch and this fails at 0.96 of full amplitude,
// not marginally — the skip below is sized so the head ramp has exactly enough
// frames to climb back to unity before the message runs out.
void testSkipIntoTheTailStillFadesOut()
{
    constexpr int64_t kRemaining = WsprBeacon::kTaperFrames - 1;   // 277
    constexpr int64_t kSkip = WsprBeacon::kMessageFrames - kRemaining;
    const std::vector<float> pcm =
        generate(uniformSymbols(0), 1500.0, -20.0f, kRemaining,
                 static_cast<int>(kSkip));
    constexpr float kAmplitude = 0.1f;   // -20 dBFS

    float peak = 0.0f;
    for (const float sample : pcm) {
        peak = std::max(peak, std::abs(sample));
    }
    check(peak < kAmplitude * 1e-3f,
          "a skip into the tail region decays instead of ramping back up");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testReferenceVector();
    testValidation();
    testSampleTimingAndTailSilence();
    testTwentyOneSecondsDoesNotComplete();
    testToneConventionMatchesWsjtx();
    testFrameEndsAreTapered();
    testLateStartSkipsIntoTheFrame();
    testSkipIntoTheTailStillFadesOut();
    testIndependentDaxPump();
    if (failures == 0) {
        std::puts("WSPR beacon tests passed");
    }
    return failures == 0 ? 0 : 1;
}
