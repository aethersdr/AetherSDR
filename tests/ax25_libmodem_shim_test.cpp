#include "core/tnc/AetherAx25LibmodemShim.h"

#include "bitstream.h"

#include <QCoreApplication>
#include <QVector>

#include <cstdio>
#include <vector>

using namespace AetherSDR;
namespace lm = aether_libmodem_core;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

QVector<quint8> toQtBits(const std::vector<uint8_t>& bits)
{
    QVector<quint8> out;
    out.reserve(static_cast<qsizetype>(bits.size()));
    for (uint8_t bit : bits)
        out.append(bit ? 1 : 0);
    return out;
}

lm::packet knownPacket()
{
    return lm::packet("N0CALL-9", "APRS",
                      {"WIDE1-1", "WIDE2-1"},
                      "hello world");
}

void testConstructsWithHf300Config()
{
    AetherAx25LibmodemShim shim;
    const auto cfg = shim.config();
    report("default profile is HF 300", cfg.profile == Ax25ModemProfile::Hf300);
    report("default sample rate", cfg.sampleRate == 24000);
    report("default baud", cfg.baud == 300);
    report("default tones", cfg.markHz == 1600.0 && cfg.spaceHz == 1800.0);
}

void testVhf1200ProfileConfig()
{
    AetherAx25LibmodemShim shim;
    shim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    const auto cfg = shim.config();
    report("VHF profile is retained", cfg.profile == Ax25ModemProfile::Vhf1200);
    report("VHF sample rate", cfg.sampleRate == 24000);
    report("VHF baud", cfg.baud == 1200);
    report("VHF tones", cfg.markHz == 1200.0 && cfg.spaceHz == 2200.0);
    report("VHF description names profile", shim.demodDescription().contains(QStringLiteral("1200 baud VHF")));
}

void testKnownGoodBitstreamDecodes()
{
    AetherAx25LibmodemShim shim;
    lm::ax25_bitstream_converter converter;
    const auto bits = converter.encode(knownPacket(), 6, 2);
    const auto frames = shim.processRecoveredBitsForTest(toQtBits(bits));

    report("known-good AX.25 bitstream emits one frame", frames.size() == 1);
    if (frames.isEmpty())
        return;
    const auto& frame = frames.first();
    report("decoded source", frame.source == QStringLiteral("N0CALL-9"));
    report("decoded destination", frame.destination == QStringLiteral("APRS"));
    report("decoded path", frame.path == QStringList({QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")}));
    report("decoded UI frame", frame.isUiFrame && frame.control == 0x03 && frame.pid == 0xf0);
    report("decoded payload", frame.payloadText == QStringLiteral("hello world"));
    report("decoded FCS accepted", frame.fcsOk);
}

void testBadFcsDoesNotEmit()
{
    AetherAx25LibmodemShim shim;
    std::vector<uint8_t> frameBytes = lm::ax25::encode_frame(knownPacket());
    if (!frameBytes.empty())
        frameBytes.back() ^= 0x40u;
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 6, 2);
    const auto frames = shim.processRecoveredBitsForTest(toQtBits(bits));
    report("bad-FCS AX.25 bitstream emits no valid frames", frames.isEmpty());
}

void testTonePolarityConfig()
{
    AetherAx25LibmodemShim shim;
    const QString normal = shim.demodDescription();

    auto cfg = shim.config();
    cfg.polarity = Ax25TonePolarity::Inverted;
    shim.configure(cfg);
    const QString inverted = shim.demodDescription();

    report("normal and inverted demod setup are distinct", normal != inverted);
    report("inverted config retained", shim.config().polarity == Ax25TonePolarity::Inverted);
}

void testSampleRateMismatchIsIgnored()
{
    AetherAx25LibmodemShim shim;
    const float samples[8] = {};
    const auto frames = shim.processMonoFloat(samples, 8, 48000);
    report("non-24k sample-rate input is ignored for now", frames.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testConstructsWithHf300Config();
    testVhf1200ProfileConfig();
    testKnownGoodBitstreamDecodes();
    testBadFcsDoesNotEmit();
    testTonePolarityConfig();
    testSampleRateMismatchIsIgnored();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
