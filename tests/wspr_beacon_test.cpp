#include "core/AudioEngine.h"
#include "core/WsprBeacon.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <array>
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
    beacon.start(encoded.symbols, 1500.0f, -20.0f, 10);

    std::vector<int16_t> pcm(20, 1234);
    beacon.process(pcm.data(), 10, 2);
    check(beacon.currentSymbol() == -1, "pre-roll remains before first symbol");
    for (const int16_t sample : pcm) {
        check(sample == 0, "pre-roll replaces microphone with silence");
    }

    pcm.assign(2, 0);
    beacon.process(pcm.data(), 1, 2);
    check(beacon.currentSymbol() == 0, "first symbol starts after exact pre-roll");

    constexpr int remainingFrames =
        WsprBeacon::kSymbolCount * WsprBeacon::kFramesPerSymbol - 1;
    std::vector<int16_t> block(4096 * 2);
    int remaining = remainingFrames;
    while (remaining > 0) {
        const int frames = std::min(remaining, 4096);
        beacon.process(block.data(), frames, 2);
        remaining -= frames;
    }
    check(beacon.isComplete(), "message completes after exactly 162 symbols");
    check(beacon.isActive(), "source stays active until explicit stop");

    block.assign(64, 1234);
    beacon.process(block.data(), 32, 2);
    for (const int16_t sample : block) {
        check(sample == 0, "completed source holds silence until unkey");
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
    beacon.start(encoded.symbols, 1500.0f, -20.0f);

    int64_t remaining =
        WsprBeacon::framesForElapsedNanoseconds(21000000000LL);
    std::vector<int16_t> block(4096 * 2);
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

    engine.wsprBeacon()->start(encoded.symbols, 1500.0f, -20.0f, 0);
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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testReferenceVector();
    testValidation();
    testSampleTimingAndTailSilence();
    testTwentyOneSecondsDoesNotComplete();
    testIndependentDaxPump();
    if (failures == 0) {
        std::puts("WSPR beacon tests passed");
    }
    return failures == 0 ? 0 : 1;
}
