#include "core/KiwiSdrProtocol.h"

#include <QVector>

#include <cmath>
#include <cstdio>

namespace {

bool nearlyEqual(float a, float b, float epsilon = 0.0001f)
{
    return std::fabs(a - b) <= epsilon;
}

int fail(const char* message)
{
    std::fprintf(stderr, "kiwi_sdr_protocol_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using namespace AetherSDR::KiwiSdrProtocol;

    const QByteArray soundFrame =
        QByteArray("SND", 3)
        + QByteArray::fromHex("051234");
    const SoundFrameHeader soundHeader = parseSoundFrameHeader(soundFrame);
    if (!soundHeader.valid
        || soundHeader.sequence != 5
        || soundHeader.flags != 5
        || soundHeader.hasRssi) {
        return fail("SND header sequence parsing should not invent calibrated RSSI");
    }

    const QByteArray serverSoundFrame =
        QByteArray("SND", 3)
        + QByteArray::fromHex("8012340021021c")
        + QByteArray::fromHex("0001fffe");
    const SoundFrameHeader serverSoundHeader =
        parseSoundFrameHeader(serverSoundFrame);
    if (!serverSoundHeader.valid
        || serverSoundHeader.sequence != 0x12
        || serverSoundHeader.flags != 0x80
        || serverSoundHeader.hasRssi) {
        return fail("server SND header sequence parsing is wrong");
    }

    QByteArray observedSoundFrame("SND", 3);
    observedSoundFrame.append('\x01');
    observedSoundFrame.append(QByteArray(1030, '\0'));
    observedSoundFrame[4] = '\x2a';
    const SoundFrameHeader observedSoundHeader =
        parseSoundFrameHeader(observedSoundFrame);
    if (!observedSoundHeader.valid
        || observedSoundHeader.sequence != 42
        || observedSoundHeader.hasRssi) {
        return fail("observed SND compatibility header sequence is wrong");
    }

    const QByteArray waterfallFrame =
        QByteArray("W/F", 3)
        + QByteArray::fromHex("fe00010203");
    const WaterfallLineHeader waterfallHeader =
        parseWaterfallLineHeader(waterfallFrame);
    if (!waterfallHeader.valid || waterfallHeader.sequence != 254) {
        return fail("W/F line sequence parsing is wrong");
    }

    if (sequenceGapCount(5, 7) != 1
        || sequenceGapCount(254, 1) != 2
        || sequenceGapCount(-1, 10) != 0) {
        return fail("sequence gap accounting is wrong");
    }

    if (!nearlyEqual(waterfallByteToDisplayLevel(0), -130.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(255), 0.0f)) {
        return fail("waterfall byte display-level conversion is wrong");
    }

    QVector<float> row(1024, -100.0f);
    for (int i = 1003; i < row.size(); ++i) {
        row[i] = -60.0f;
    }

    const WaterfallAperture aperture = autoWaterfallAperture(row);
    if (!aperture.valid) {
        return fail("auto aperture did not become valid");
    }
    if (!nearlyEqual(aperture.minDbm, -110.0f)
        || !nearlyEqual(aperture.maxDbm, -30.0f)) {
        return fail("auto aperture does not follow median/98th percentile rule");
    }

    const float index = waterfallColorIndex(-90.0f, -110.0f, -30.0f);
    if (!nearlyEqual(index, 0.5f)) {
        return fail("waterfall color index does not use sqrt contrast");
    }

    QByteArray extendedSoundFrame(1034, '\0');
    extendedSoundFrame[0] = 'S';
    extendedSoundFrame[1] = 'N';
    extendedSoundFrame[2] = 'D';
    extendedSoundFrame[8] = static_cast<char>(0x02);
    extendedSoundFrame[9] = static_cast<char>(0x1c);
    const MeterReading experimentalMeter =
        extractMeterFromSndVerifiedLayout(extendedSoundFrame, MeterContext{});
    if (!experimentalMeter.valid
        || experimentalMeter.capability != MeterCapability::Experimental
        || !experimentalMeter.hasRawValue
        || !nearlyEqual(experimentalMeter.rawValue, 540.0f)
        || !experimentalMeter.hasDbm
        || !nearlyEqual(experimentalMeter.dbm, -73.0f)
        || experimentalMeter.sUnits != QStringLiteral("S9")
        || experimentalMeter.label != QStringLiteral("S-Meter")) {
        return fail("experimental SND meter candidate extraction is wrong");
    }

    const MeterReading unavailable =
        extractMeterFromSndVerifiedLayout(soundFrame, MeterContext{});
    if (unavailable.valid
        || unavailable.capability != MeterCapability::Unavailable
        || unavailable.label != QStringLiteral("Meter unavailable")
        || !unavailable.notes.contains(QStringLiteral("layout not verified"))) {
        return fail("non-extended SND meter extraction should remain unavailable");
    }

    QVector<float> silence(128, 0.0f);
    const MeterReading silentAudio =
        computeRelativeAudioLevel(silence.constData(), silence.size());
    if (!silentAudio.valid
        || silentAudio.capability != MeterCapability::RelativeAudio
        || silentAudio.label != QStringLiteral("Audio level, relative")
        || !nearlyEqual(silentAudio.relativeLevel, 0.0f)
        || silentAudio.hasDbm
        || !silentAudio.sUnits.isEmpty()) {
        return fail("silent audio relative meter is wrong");
    }

    QVector<float> fullScale;
    fullScale.reserve(256);
    for (int i = 0; i < 256; ++i) {
        fullScale.append((i % 2) == 0 ? 1.0f : -1.0f);
    }
    const MeterReading fullScaleAudio =
        computeRelativeAudioLevel(fullScale.constData(), fullScale.size());
    if (!fullScaleAudio.valid || fullScaleAudio.relativeLevel < 0.99f
        || fullScaleAudio.hasDbm || !fullScaleAudio.sUnits.isEmpty()) {
        return fail("full-scale audio relative meter is wrong");
    }

    QVector<float> lowNoise(128, 0.01f);
    const MeterReading lowAudio =
        computeRelativeAudioLevel(lowNoise.constData(), lowNoise.size());
    if (!lowAudio.valid
        || lowAudio.relativeLevel <= 0.0f
        || lowAudio.relativeLevel >= 0.5f
        || lowAudio.hasDbm
        || !lowAudio.sUnits.isEmpty()) {
        return fail("low-level audio relative meter is wrong");
    }

    QVector<float> waterfallBins(64, -120.0f);
    for (int i = 60; i < waterfallBins.size(); ++i) {
        waterfallBins[i] = -20.0f;
    }
    const MeterReading waterfallReading =
        computeRelativeWaterfallLevel(waterfallBins);
    if (!waterfallReading.valid
        || waterfallReading.capability != MeterCapability::RelativeWaterfall
        || waterfallReading.label != QStringLiteral("Waterfall intensity, relative")
        || waterfallReading.relativeLevel <= 0.75f
        || waterfallReading.hasDbm
        || !waterfallReading.sUnits.isEmpty()) {
        return fail("waterfall relative meter is wrong");
    }

    if (convertDbmToSUnits(-73.0f) != QStringLiteral("S9")
        || convertDbmToSUnits(-79.0f) != QStringLiteral("S8")
        || convertDbmToSUnits(-85.0f) != QStringLiteral("S7")
        || convertDbmToSUnits(-91.0f) != QStringLiteral("S6")
        || convertDbmToSUnits(-63.0f) != QStringLiteral("S9 + 10 dB")
        || convertDbmToSUnits(-53.0f) != QStringLiteral("S9 + 20 dB")) {
        return fail("dBm to S-unit conversion is wrong");
    }

    return 0;
}
