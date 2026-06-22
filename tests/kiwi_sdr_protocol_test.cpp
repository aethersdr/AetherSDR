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
        || serverSoundHeader.squelched
        || serverSoundHeader.hasRssi) {
        return fail("server SND header sequence parsing is wrong");
    }

    const QByteArray squelchedSoundFrame =
        QByteArray("SND", 3)
        + QByteArray::fromHex("40010000000000000102");
    const SoundFrameHeader squelchedSoundHeader =
        parseSoundFrameHeader(squelchedSoundFrame);
    if (!squelchedSoundHeader.valid
        || !squelchedSoundHeader.squelched
        || squelchedSoundHeader.flags != 0x40) {
        return fail("server SND squelch flag parsing is wrong");
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

    if (!nearlyEqual(waterfallByteToDisplayLevel(0), -200.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(55), -200.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(155), -100.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(255), 0.0f)) {
        return fail("waterfall byte display-level conversion is wrong");
    }

    const IpLimitNotice encodedIpLimit =
        parseIpLimitNotice(QStringLiteral("180%2c192.0.2.44"));
    if (!encodedIpLimit.valid
        || encodedIpLimit.minutes != 180
        || encodedIpLimit.address != QStringLiteral("192.0.2.44")) {
        return fail("encoded ip_limit notice parsing is wrong");
    }

    const IpLimitNotice minuteOnlyIpLimit =
        parseIpLimitNotice(QStringLiteral("45"));
    if (!minuteOnlyIpLimit.valid
        || minuteOnlyIpLimit.minutes != 45
        || !minuteOnlyIpLimit.address.isEmpty()) {
        return fail("minute-only ip_limit notice parsing is wrong");
    }

    if (parseIpLimitNotice(QStringLiteral("0,192.0.2.1")).valid
        || parseIpLimitNotice(QStringLiteral("not-a-limit")).valid) {
        return fail("invalid ip_limit notice should be rejected");
    }

    if (formatSquelchCommand(false, 20)
            != QStringLiteral("SET squelch=0 param=0.00")
        || formatSquelchCommand(true, 0)
            != QStringLiteral("SET squelch=1 param=0.00")
        || formatSquelchCommand(true, 20)
            != QStringLiteral("SET squelch=20 param=0.00")
        || formatSquelchCommand(true, -20)
            != QStringLiteral("SET squelch=-20 param=0.00")
        || formatSquelchCommand(true, -4)
            != QStringLiteral("SET squelch=-4 param=0.00")
        || formatSquelchCommand(true, -250)
            != QStringLiteral("SET squelch=-99 param=0.00")
        || formatSquelchCommand(true, 250)
            != QStringLiteral("SET squelch=99 param=0.00")) {
        return fail("squelch command formatting is wrong");
    }

    if (squelchSliderLevelToMarginDb(-10) != -99
        || squelchSliderLevelToMarginDb(0) != -99
        || squelchSliderLevelToMarginDb(1) != -97
        || squelchSliderLevelToMarginDb(20) != -59
        || squelchSliderLevelToMarginDb(49) != -1
        || squelchSliderLevelToMarginDb(50) != 1
        || squelchSliderLevelToMarginDb(75) != 51
        || squelchSliderLevelToMarginDb(99) != 99
        || squelchSliderLevelToMarginDb(100) != 99
        || squelchSliderLevelToMarginDb(150) != 99) {
        return fail("squelch slider-to-margin mapping is wrong");
    }

    if (agcDecayMsForMode(QStringLiteral("fast")) != kAgcFastDecayMs
        || agcDecayMsForMode(QStringLiteral("FAST")) != kAgcFastDecayMs
        || agcDecayMsForMode(QStringLiteral("med")) != kAgcMedDecayMs
        || agcDecayMsForMode(QStringLiteral("slow")) != kAgcSlowDecayMs
        || agcDecayMsForMode(QStringLiteral("off")) != kAgcMedDecayMs
        || agcDecayMsForMode(QStringLiteral("unexpected")) != kAgcMedDecayMs) {
        return fail("AGC mode-to-decay mapping is wrong");
    }

    if (formatAgcCommand(true, false, -100, 50, agcDecayMsForMode("med"))
            != QStringLiteral("SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50")
        || formatAgcCommand(true, false, -80, 50, agcDecayMsForMode("fast"))
            != QStringLiteral("SET agc=1 hang=0 thresh=-80 slope=6 decay=300 manGain=50")
        || formatAgcCommand(true, false, -120, 50, agcDecayMsForMode("slow"))
            != QStringLiteral("SET agc=1 hang=0 thresh=-120 slope=6 decay=3000 manGain=50")
        || formatAgcCommand(false, false, -100, 44, agcDecayMsForMode("off"))
            != QStringLiteral("SET agc=0 hang=0 thresh=-100 slope=6 decay=1000 manGain=44")
        || formatAgcCommand(true, true, -999, 200, 99999)
            != QStringLiteral("SET agc=1 hang=1 thresh=-160 slope=6 decay=5000 manGain=100")
        || formatAgcCommand(true, false, 20, -5, 1)
            != QStringLiteral("SET agc=1 hang=0 thresh=0 slope=6 decay=20 manGain=0")) {
        return fail("AGC command formatting is wrong");
    }

    const QVector<MsgToken> msgTokens = parseMsgTokens(
        QStringLiteral("MSG wb_only password_timeout inactivity_timeout=15 "
                       "kiwi_kick=1%2coperator%20request badp=5 =ignored"));
    if (msgTokens.size() != 5) {
        return fail("MSG token parser should keep flags and key-value tokens");
    }
    if (msgTokens[0].key != QStringLiteral("wb_only")
        || msgTokens[0].hasValue) {
        return fail("MSG flag-only token parsing is wrong");
    }
    if (msgTokens[1].key != QStringLiteral("password_timeout")
        || msgTokens[1].hasValue) {
        return fail("MSG second flag-only token parsing is wrong");
    }
    if (msgTokens[2].key != QStringLiteral("inactivity_timeout")
        || msgTokens[2].value != QStringLiteral("15")
        || !msgTokens[2].hasValue) {
        return fail("MSG key-value token parsing is wrong");
    }
    if (msgTokens[3].key != QStringLiteral("kiwi_kick")
        || msgTokens[3].value != QStringLiteral("1%2coperator%20request")
        || !msgTokens[3].hasValue) {
        return fail("MSG encoded key-value token parsing is wrong");
    }
    if (msgTokens[4].key != QStringLiteral("badp")
        || msgTokens[4].value != QStringLiteral("5")
        || !msgTokens[4].hasValue) {
        return fail("MSG badp token parsing is wrong");
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
    const MeterReading sndMeter =
        extractMeterFromSndVerifiedLayout(extendedSoundFrame, MeterContext{});
    if (!sndMeter.valid
        || sndMeter.capability != MeterCapability::CalibratedSndMeter
        || sndMeter.confidence != MeterConfidence::Verified
        || !sndMeter.hasRawValue
        || !nearlyEqual(sndMeter.rawValue, 540.0f)
        || !sndMeter.hasDbm
        || !nearlyEqual(sndMeter.dbm, -73.0f)
        || sndMeter.sUnits != QStringLiteral("S9")
        || sndMeter.label != QStringLiteral("S-Meter")) {
        return fail("SND meter extraction is wrong");
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
