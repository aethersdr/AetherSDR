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

void appendLittleEndianU32(QByteArray* bytes, quint32 value)
{
    bytes->append(static_cast<char>(value & 0xffu));
    bytes->append(static_cast<char>((value >> 8) & 0xffu));
    bytes->append(static_cast<char>((value >> 16) & 0xffu));
    bytes->append(static_cast<char>((value >> 24) & 0xffu));
}

QByteArray extendedWaterfallFrame(quint32 start, quint32 flagsAndZoom,
                                  const QByteArray& payload)
{
    QByteArray frame("W/F", 3);
    frame.append('\x08');
    appendLittleEndianU32(&frame, start);
    appendLittleEndianU32(&frame, flagsAndZoom);
    appendLittleEndianU32(&frame, 0);
    frame.append(payload);
    return frame;
}

QByteArray extendedSoundFrame(quint8 flags, quint32 sequence,
                              const QByteArray& payload)
{
    QByteArray frame("SND", 3);
    frame.append(static_cast<char>(flags));
    appendLittleEndianU32(&frame, sequence);
    frame.append('\0');
    frame.append('\0');
    frame.append(payload);
    return frame;
}

} // namespace

int main()
{
    using namespace AetherSDR::KiwiSdrProtocol;

    if (formatAuthCommand(QString())
        != QStringLiteral("SET auth t=kiwi p=#")) {
        return fail("empty Kiwi password should use the public sentinel");
    }
    if (formatAuthCommand(QStringLiteral("space #100% ü"))
        != QStringLiteral("SET auth t=kiwi p=space%20%23100%25%20%C3%BC")) {
        return fail("Kiwi password should use UTF-8 percent encoding");
    }
    if (!authPasswordFitsServerLimit(QString(256, QLatin1Char('a')))
        || formatAuthCommand(QString(256, QLatin1Char('a'))).isEmpty()) {
        return fail("256 unreserved password characters should fit");
    }
    if (authPasswordFitsServerLimit(QString(257, QLatin1Char('a')))
        || !formatAuthCommand(QString(257, QLatin1Char('a'))).isEmpty()) {
        return fail("passwords beyond the server token limit should fail closed");
    }
    if (!authPasswordFitsServerLimit(QString(85, QLatin1Char(' ')))
        || authPasswordFitsServerLimit(QString(86, QLatin1Char(' ')))) {
        return fail("password limit should apply after percent encoding");
    }

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

    if (!nearlyEqual(waterfallByteToDbm(0), -255.0f)
        || !nearlyEqual(waterfallByteToDbm(55), -200.0f)
        || !nearlyEqual(waterfallByteToDbm(155), -100.0f)
        || !nearlyEqual(waterfallByteToDbm(255), 0.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(0), -255.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(55), -200.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(155), -100.0f)
        || !nearlyEqual(waterfallByteToDisplayLevel(255), 0.0f)) {
        return fail("waterfall byte-to-dBm conversion is wrong");
    }

    if (!nearlyEqual(calibratedWaterfallLevel(-100.0f, 7), -93.0f)
        || !nearlyEqual(calibratedWaterfallLevel(-100.0f, -3), -103.0f)) {
        return fail("waterfall calibration offset is wrong");
    }

    if (!nearlyEqual(waterfallZoomCorrectionDb(-1), 0.0f)
        || !nearlyEqual(waterfallZoomCorrectionDb(3), 9.0f)) {
        return fail("waterfall zoom correction is wrong");
    }

    const WaterfallDisplayRange defaultRange =
        defaultWaterfallDisplayRange(0, 0, 0);
    if (!defaultRange.valid
        || !nearlyEqual(defaultRange.minDbm, -110.0f)
        || !nearlyEqual(defaultRange.maxDbm, -10.0f)) {
        return fail("default waterfall display range is wrong");
    }

    const WaterfallDisplayRange zoomedRange =
        defaultWaterfallDisplayRange(3, 5, -2);
    if (!zoomedRange.valid
        || !nearlyEqual(zoomedRange.minDbm, -121.0f)
        || !nearlyEqual(zoomedRange.maxDbm, -5.0f)) {
        return fail("zoomed waterfall display range is wrong");
    }

    const WaterfallDisplayRange adjustedRange =
        adjustedWaterfallDisplayRange(-98.0f, -42.0f, 3, -4);
    if (!adjustedRange.valid
        || !nearlyEqual(adjustedRange.minDbm, -102.0f)
        || !nearlyEqual(adjustedRange.maxDbm, -39.0f)) {
        return fail("adjusted waterfall display range is wrong");
    }

    const WaterfallDisplayRange zoomAdjustedInRange =
        zoomAdjustedWaterfallDisplayRange(-100.0f, -30.0f, 2, 5);
    if (!zoomAdjustedInRange.valid
        || !nearlyEqual(zoomAdjustedInRange.minDbm, -109.0f)
        || !nearlyEqual(zoomAdjustedInRange.maxDbm, -30.0f)) {
        return fail("stored auto waterfall range did not normalize zoom-in floor");
    }

    const WaterfallDisplayRange zoomAdjustedOutRange =
        zoomAdjustedWaterfallDisplayRange(-100.0f, -30.0f, 5, 2);
    if (!zoomAdjustedOutRange.valid
        || !nearlyEqual(zoomAdjustedOutRange.minDbm, -91.0f)
        || !nearlyEqual(zoomAdjustedOutRange.maxDbm, -30.0f)) {
        return fail("stored auto waterfall range did not normalize zoom-out floor");
    }

    const WaterfallDisplayRange narrowRange =
        adjustedWaterfallDisplayRange(-50.0f, -80.0f, 0, 0);
    if (!narrowRange.valid
        || !nearlyEqual(narrowRange.minDbm, -50.0f)
        || !nearlyEqual(narrowRange.maxDbm, -49.0f)) {
        return fail("waterfall display range span guard is wrong");
    }

    const QVector<MsgToken> waterfallDisplayTokens =
        parseMsgTokens(QStringLiteral("MSG wf_cal=7 maxdb=-55 mindb=-116"));
    if (waterfallDisplayTokens.size() != 3
        || waterfallDisplayTokens[0].key != QStringLiteral("wf_cal")
        || waterfallDisplayTokens[0].value != QStringLiteral("7")
        || waterfallDisplayTokens[1].key != QStringLiteral("maxdb")
        || waterfallDisplayTokens[2].key != QStringLiteral("mindb")) {
        return fail("waterfall display metadata tokens do not parse");
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

    if (apiPolicyFor(-1, 4) != ApiPolicy::Unknown
        || apiPolicyFor(0, 4) != ApiPolicy::Disabled
        || apiPolicyFor(2, -1) != ApiPolicy::Unknown
        || apiPolicyFor(2, 4) != ApiPolicy::Limited
        || apiPolicyFor(4, 4) != ApiPolicy::Open
        || apiPolicyName(ApiPolicy::Limited) != QStringLiteral("limited")) {
        return fail("API policy classification is wrong");
    }

    const ProtocolState defaultState = defaultProtocolState();
    if (!defaultState.sound.requested
        || !defaultState.sound.uncompressedRequested
        || !defaultState.sound.supportedLayouts.contains(
            FrameLayout::SndObservedPcm16WithMeter)
        || !defaultState.sound.supportedLayouts.contains(
            FrameLayout::SndCompressed)
        || !defaultState.waterfall.supportedLayouts.contains(
            FrameLayout::WaterfallDirectBins)
        || !defaultState.waterfall.supportedLayouts.contains(
            FrameLayout::WaterfallCompactEncoded)
        || defaultState.unsupportedFeatureReasons.isEmpty()) {
        return fail("default protocol capability state is wrong");
    }

    QByteArray observedLayoutFrame(1034, '\0');
    observedLayoutFrame[0] = 'S';
    observedLayoutFrame[1] = 'N';
    observedLayoutFrame[2] = 'D';
    observedLayoutFrame[4] = '\x2a';
    FrameObservation soundObservation = classifySoundFrame(observedLayoutFrame);
    if (!soundObservation.supported
        || soundObservation.layout != FrameLayout::SndObservedPcm16WithMeter
        || soundObservation.payloadBytes != 1024) {
        return fail("observed SND frame layout classification is wrong");
    }
    const SoundFrameDecodeResult observedSoundDecode =
        decodeSoundFrame(observedLayoutFrame);
    if (!observedSoundDecode.observation.supported
        || observedSoundDecode.monoSamples.size() != 512) {
        return fail("observed SND frame decode should preserve PCM samples");
    }

    const QByteArray compressedSoundFrame =
        extendedSoundFrame(0x10, 0x44, QByteArray::fromHex("7788"));
    soundObservation = classifySoundFrame(compressedSoundFrame);
    if (!soundObservation.supported
        || soundObservation.layout != FrameLayout::SndCompressed
        || soundObservation.payloadBytes != 2) {
        return fail("compressed SND frame classification is wrong");
    }
    SoundAdpcmState soundAdpcm;
    SoundFrameDecodeResult compressedSoundDecode =
        decodeSoundFrame(compressedSoundFrame, &soundAdpcm);
    if (!compressedSoundDecode.observation.supported
        || compressedSoundDecode.monoSamples.size() != 4
        || !nearlyEqual(compressedSoundDecode.monoSamples[0], 11.0f / 32768.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[1], 41.0f / 32768.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[2], 37.0f / 32768.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[3], 34.0f / 32768.0f)) {
        return fail("compressed SND ADPCM decode is wrong");
    }
    compressedSoundDecode = decodeSoundFrame(
        extendedSoundFrame(0x10, 0x45, QByteArray::fromHex("00")),
        &soundAdpcm);
    if (!compressedSoundDecode.observation.supported
        || compressedSoundDecode.monoSamples.size() != 2
        || !nearlyEqual(compressedSoundDecode.monoSamples[0], 37.0f / 32768.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[1], 40.0f / 32768.0f)) {
        return fail("compressed SND ADPCM state continuity is wrong");
    }
    resetSoundAdpcmState(&soundAdpcm);
    compressedSoundDecode = decodeSoundFrame(
        extendedSoundFrame(0x10, 0x46, QByteArray::fromHex("00")),
        &soundAdpcm);
    if (!compressedSoundDecode.observation.supported
        || compressedSoundDecode.monoSamples.size() != 2
        || !nearlyEqual(compressedSoundDecode.monoSamples[0], 0.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[1], 0.0f)) {
        return fail("compressed SND ADPCM reset behavior is wrong");
    }
    invalidateSoundAdpcmState(&soundAdpcm);
    compressedSoundDecode = decodeSoundFrame(
        extendedSoundFrame(0x10, 0x47, QByteArray::fromHex("77")),
        &soundAdpcm);
    if (compressedSoundDecode.observation.supported
        || compressedSoundDecode.observation.unsupportedReason.isEmpty()
        || !compressedSoundDecode.monoSamples.isEmpty()) {
        return fail("compressed SND ADPCM invalid state should not decode");
    }
    compressedSoundDecode = decodeSoundFrame(
        extendedSoundFrame(0x30, 0x48, QByteArray::fromHex("77")),
        &soundAdpcm);
    if (!compressedSoundDecode.observation.supported
        || !compressedSoundDecode.decoderReset
        || compressedSoundDecode.monoSamples.size() != 2
        || !nearlyEqual(compressedSoundDecode.monoSamples[0], 11.0f / 32768.0f)
        || !nearlyEqual(compressedSoundDecode.monoSamples[1], 41.0f / 32768.0f)) {
        return fail("compressed SND restart flag should reset ADPCM state");
    }
    const SoundFrameDecodeResult malformedCompressedDecode =
        decodeSoundFrame(extendedSoundFrame(0x10, 0x49, QByteArray()));
    if (malformedCompressedDecode.observation.supported
        || malformedCompressedDecode.observation.layout != FrameLayout::SndCompressed
        || !malformedCompressedDecode.monoSamples.isEmpty()) {
        return fail("empty compressed SND frame should not produce samples");
    }
    const SoundFrameDecodeResult unsupportedCompressedDecode =
        decodeSoundFrame(extendedSoundFrame(0x18, 0x4a, QByteArray::fromHex("77")));
    if (unsupportedCompressedDecode.observation.supported
        || unsupportedCompressedDecode.observation.layout != FrameLayout::SndCompressed
        || !unsupportedCompressedDecode.monoSamples.isEmpty()) {
        return fail("compressed stereo/IQ SND frame should not produce samples");
    }

    QByteArray directWaterfallFrame("W/F", 3);
    directWaterfallFrame.append('\x08');
    directWaterfallFrame.append(QByteArray(1024, '\x80'));
    FrameObservation waterfallObservation =
        classifyWaterfallFrame(directWaterfallFrame);
    if (!waterfallObservation.supported
        || waterfallObservation.layout != FrameLayout::WaterfallDirectBins
        || waterfallObservation.payloadBytes != 1024) {
        return fail("direct W/F frame classification is wrong");
    }
    const WaterfallFrameDecodeResult directWaterfallDecode =
        decodeWaterfallFrame(directWaterfallFrame);
    if (!directWaterfallDecode.observation.supported
        || directWaterfallDecode.binsDbm.size() != 1024
        || !nearlyEqual(directWaterfallDecode.binsDbm[0], -127.0f)) {
        return fail("direct W/F frame decode is wrong");
    }

    if (formatSoundCompressionCommand(false)
            != QStringLiteral("SET compression=0")
        || formatSoundCompressionCommand(true)
            != QStringLiteral("SET compression=1")
        || formatWaterfallCompressionCommand(false)
            != QStringLiteral("SET wf_comp=0")
        || formatWaterfallCompressionCommand(true)
            != QStringLiteral("SET wf_comp=1")
        || !diagnosticCompressionFlagEnabled(
            QByteArrayLiteral("yes"))
        || diagnosticCompressionFlagEnabled(
            QByteArrayLiteral("off"))
        || !diagnosticWaterfallCompressionFlagEnabled(
            QByteArrayLiteral("1"))
        || !diagnosticWaterfallCompressionFlagEnabled(
            QByteArrayLiteral("true"))
        || !diagnosticWaterfallCompressionFlagEnabled(
            QByteArrayLiteral("ON"))
        || diagnosticWaterfallCompressionFlagEnabled(
            QByteArrayLiteral("0"))
        || diagnosticWaterfallCompressionFlagEnabled(
            QByteArrayLiteral("false"))) {
        return fail("diagnostic compression command helpers are wrong");
    }

    QByteArray compactPayload(517, '\0');
    compactPayload[5] = '\x17';
    compactPayload[6] = '\x77';
    compactPayload[7] = '\x70';
    const QByteArray compactWaterfallFrame = extendedWaterfallFrame(
        16, 0x00010007u, compactPayload);
    waterfallObservation = classifyWaterfallFrame(compactWaterfallFrame);
    if (!waterfallObservation.supported
        || waterfallObservation.layout != FrameLayout::WaterfallCompactEncoded
        || waterfallObservation.payloadBytes != compactPayload.size()) {
        return fail("compact W/F frame classification is wrong");
    }
    const WaterfallFrameDecodeResult compactWaterfallDecode =
        decodeWaterfallFrame(compactWaterfallFrame);
    if (!compactWaterfallDecode.observation.supported
        || compactWaterfallDecode.binsDbm.size() != 1024
        || !nearlyEqual(compactWaterfallDecode.binsDbm[0], -244.0f)
        || !nearlyEqual(compactWaterfallDecode.binsDbm[3], -157.0f)
        || !nearlyEqual(compactWaterfallDecode.binsDbm[5], -37.0f)) {
        return fail("compact W/F frame decode is wrong");
    }

    const QByteArray capturedCompactWaterfallFrame = QByteArray::fromHex(
        "572f46201e000000070001000000000077010808087817880080889090"
        "83a08c1419c082803b85291b3d90111a931b0f311a101d9349f920"
        "1009c1c114a9b051a01c0222cb12309912d331a3117b00800909a"
        "d90b4189af3a39198239ba97bc88309303c0889c3030d813a930b"
        "f184983a4a901118037ba0883f0908109182992f210088b9995188"
        "299a1b9a4f08000008800090100909999100099991900099c998b"
        "13ba94f00188000a933a219221a309aa9c1a2301f0008c194982"
        "129a0392e082903c9d433f3a01888a0b11960c001b08699103da"
        "12c1b51a90890232f19b80399fb13929181da0302a1991b32029"
        "29b1d3892b1b1c34c9911023c18123fca03c203a919b3993ad2"
        "8219b0301b1a3a90b0bb0708199094a83d29f291a2825ab9591"
        "bc0e40290c231092a18a859190bb3191a039009d222b9933f10"
        "a8a4000c88926b1a5ba9b3935a3f0880b192b339abd384b9123"
        "0f18109aa1718000099b0309111b0091111201b9b91a029d93a"
        "18ac12799001803b73f4a0818109009080923a0b9092a019031d"
        "98121b911d01b10d823991022d0992113bc0b3242e914b9091b"
        "219903a1911b1a1111929291b19c2a303f9a381e111293bc1925"
        "b1c0ac3042a3d0818199a2f0011a19190bb9195299a0f850089"
        "a0823db192f25c802919090b11100ad1c1131a3d9018191109c"
        "220dbb38400d01129b991b1d302281f292be9131bd003f0221a"
        "81192c39d984109930b001");
    const WaterfallFrameDecodeResult capturedCompactWaterfallDecode =
        decodeWaterfallFrame(capturedCompactWaterfallFrame);
    if (!capturedCompactWaterfallDecode.observation.supported
        || capturedCompactWaterfallDecode.binsDbm.size() != 1024
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[0], -201.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[1], -176.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[2], -120.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[3], -96.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[256], -68.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[512], -87.0f)
        || !nearlyEqual(capturedCompactWaterfallDecode.binsDbm[1023], -103.0f)) {
        return fail("captured compact W/F frame decode is wrong");
    }

    const QByteArray compactWaterfallFrameWithoutFlag = extendedWaterfallFrame(
        16, 0x00000007u, compactPayload);
    if (!classifyWaterfallFrame(compactWaterfallFrameWithoutFlag).supported
        || decodeWaterfallFrame(
               compactWaterfallFrameWithoutFlag).binsDbm.size() != 1024) {
        return fail("compact W/F length fallback should decode");
    }

    const QByteArray shortCompactWaterfallFrame = extendedWaterfallFrame(
        16, 0x00010007u, QByteArray(5, '\0'));
    waterfallObservation = classifyWaterfallFrame(shortCompactWaterfallFrame);
    if (waterfallObservation.supported
        || waterfallObservation.layout != FrameLayout::WaterfallCompactEncoded
        || decodeWaterfallFrame(shortCompactWaterfallFrame).binsDbm.size() != 0) {
        return fail("short compact W/F frame should not produce bins");
    }

    const QByteArray unknownWaterfallFrame = extendedWaterfallFrame(
        16, 0x00000007u, QByteArray(300, '\x80'));
    waterfallObservation = classifyWaterfallFrame(unknownWaterfallFrame);
    if (waterfallObservation.supported
        || waterfallObservation.layout != FrameLayout::Unsupported
        || decodeWaterfallFrame(unknownWaterfallFrame).binsDbm.size() != 0) {
        return fail("unknown W/F frame layout should not produce fake bins");
    }

    const ReceiverMetadata statusMetadata = parseStatusPayload(
        QByteArrayLiteral(
            "users=3\n"
            "users_max=4\n"
            "preempt=1\n"
            "ext_api=2\n"
            "gps_good=1\n"
            "adc_clipping=0\n"
            "center_freq=15000000\n"
            "bandwidth=30000000\n"
            "freq=7050\n"
            "rx_chan=1\n"
            "wf_chans=2\n"
            "foo_status=bar baz\n"
            "build=2026-06-28\n"),
        QStringLiteral("KiwiSDR_1.842/Mongoose_7.14"));
    if (statusMetadata.serverVersion != QStringLiteral("1.842")
        || !statusMetadata.hasUsers
        || statusMetadata.users != 3
        || !statusMetadata.hasUsersMax
        || statusMetadata.usersMax != 4
        || statusMetadata.apiPolicy != ApiPolicy::Limited
        || !statusMetadata.hasGpsGood
        || !statusMetadata.gpsGood
        || !statusMetadata.hasAdcClipping
        || statusMetadata.adcClipping
        || !statusMetadata.hasCoverageCenter
        || !nearlyEqual(static_cast<float>(statusMetadata.coverageCenterMhz),
                        15.0f)
        || !statusMetadata.hasCoverageBandwidth
        || !nearlyEqual(static_cast<float>(statusMetadata.coverageBandwidthMhz),
                        30.0f)
        || !statusMetadata.hasReportedFrequency
        || !nearlyEqual(static_cast<float>(statusMetadata.reportedFrequencyKhz),
                        7050.0f)
        || !statusMetadata.hasReceiverChannel
        || statusMetadata.receiverChannel != 1
        || !statusMetadata.hasWaterfallChannels
        || statusMetadata.waterfallChannels != 2
        || statusMetadata.serverBuild != QStringLiteral("2026-06-28")
        || statusMetadata.hasBusy
        || !statusMetadata.stableStatusFields.contains(
            QStringLiteral("foo_status=bar baz"))
        || statusMetadata.stableStatusFields.contains(
            QStringLiteral("users=3"))
        || statusMetadata.stableStatusFields.contains(
            QStringLiteral("center_freq=15000000"))) {
        return fail("status metadata parsing is wrong");
    }

    const ReceiverMetadata fullStatusMetadata = parseStatusPayload(
        QByteArrayLiteral(
            "users=4\n"
            "users_max=4\n"
            "preempt=0\n"
            "ext_api=4\n"),
        QString());
    if (!fullStatusMetadata.hasBusy || !fullStatusMetadata.busy) {
        return fail("full /status metadata should report busy");
    }
    const ReceiverMetadata preemptStatusMetadata = parseStatusPayload(
        QByteArrayLiteral(
            "users=4\n"
            "users_max=4\n"
            "preempt=1\n"
            "ext_api=4\n"),
        QString());
    if (preemptStatusMetadata.hasBusy) {
        return fail("preempt-capable full /status metadata should not report busy");
    }

    ReceiverMetadata mergedMetadata;
    if (!mergeReceiverMetadata(&mergedMetadata, statusMetadata)
        || mergedMetadata.apiPolicy != ApiPolicy::Limited) {
        return fail("receiver metadata merge is wrong");
    }
    const MsgToken busyToken{
        QStringLiteral("too_busy"),
        QStringLiteral("3"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(busyToken, &mergedMetadata)
        || !mergedMetadata.hasBusy
        || !mergedMetadata.busy) {
        return fail("MSG metadata busy update is wrong");
    }
    // Regression: a malformed or empty `too_busy=` must NOT mark a reachable
    // receiver as busy (the parser previously forced busy=true on parse
    // failure, so a garbled/injected MSG could falsely report capacity).
    for (const QString& badValue :
         {QStringLiteral("not-a-number"), QString()}) {
        ReceiverMetadata reachableMetadata;
        const MsgToken malformedBusyToken{
            QStringLiteral("too_busy"),
            badValue,
            true,
        };
        updateReceiverMetadataFromMsgToken(malformedBusyToken,
                                           &reachableMetadata);
        if (reachableMetadata.hasBusy || reachableMetadata.busy) {
            return fail("malformed too_busy must not force busy");
        }
    }
    const MsgToken monitorToken{
        QStringLiteral("monitor"),
        QString(),
        false,
    };
    if (!updateReceiverMetadataFromMsgToken(monitorToken, &mergedMetadata)
        || !mergedMetadata.hasCampStatus
        || mergedMetadata.campStatus != CampStatus::Offered
        || !mergedMetadata.hasBusy
        || !mergedMetadata.busy
        || mergedMetadata.stableStatusFields.contains(
            QStringLiteral("monitor"))) {
        return fail("MSG monitor metadata update is wrong");
    }
    const MsgToken maxCampToken{
        QStringLiteral("max_camp"),
        QStringLiteral("3"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(maxCampToken, &mergedMetadata)
        || !mergedMetadata.hasMaxCampers
        || mergedMetadata.maxCampers != 3) {
        return fail("MSG max_camp metadata update is wrong");
    }
    const MsgToken queueToken{
        QStringLiteral("qpos"),
        QStringLiteral("1,4,0"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(queueToken, &mergedMetadata)
        || mergedMetadata.campStatus != CampStatus::Queued
        || !mergedMetadata.hasCampQueuePosition
        || mergedMetadata.campQueuePosition != 1
        || !mergedMetadata.hasCampQueueWaiters
        || mergedMetadata.campQueueWaiters != 4
        || !mergedMetadata.hasCampQueueReloadRecommended
        || mergedMetadata.campQueueReloadRecommended) {
        return fail("MSG qpos metadata update is wrong");
    }
    const MsgToken queueReloadToken{
        QStringLiteral("qpos"),
        QStringLiteral("1,4,1"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(queueReloadToken, &mergedMetadata)
        || mergedMetadata.campStatus != CampStatus::Queued
        || !mergedMetadata.campQueueReloadRecommended) {
        return fail("MSG qpos reload metadata update is wrong");
    }
    // Regression: a malformed/short `qpos` must NOT flip status to Queued or
    // half-update the queue counters (which would render as a nonsensical
    // "position N of <stale> waiters"). Position-only / empty / non-numeric
    // payloads are rejected wholesale, leaving metadata untouched.
    for (const QString& badValue : {QStringLiteral("99"), QString(),
                                    QStringLiteral("x,y,z")}) {
        ReceiverMetadata q;
        const MsgToken badQpos{QStringLiteral("qpos"), badValue, true};
        updateReceiverMetadataFromMsgToken(badQpos, &q);
        if (q.campStatus == CampStatus::Queued || q.hasCampQueuePosition
            || q.hasCampQueueWaiters) {
            return fail("malformed qpos must not set Queued/partial counters");
        }
    }
    const MsgToken campAcceptedToken{
        QStringLiteral("camp"),
        QStringLiteral("1,2"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(campAcceptedToken, &mergedMetadata)
        || mergedMetadata.campStatus != CampStatus::Accepted
        || !mergedMetadata.hasCampReceiverChannel
        || mergedMetadata.campReceiverChannel != 2
        || campStatusName(mergedMetadata.campStatus)
               != QStringLiteral("accepted")) {
        return fail("MSG camp accepted metadata update is wrong");
    }
    ReceiverMetadata rejectedCampMetadata;
    const MsgToken campRejectedToken{
        QStringLiteral("camp"),
        QStringLiteral("0,3"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(campRejectedToken,
                                            &rejectedCampMetadata)
        || rejectedCampMetadata.campStatus != CampStatus::Rejected
        || rejectedCampMetadata.campReceiverChannel != 3) {
        return fail("MSG camp rejected metadata update is wrong");
    }
    const MsgToken audioCampStoppedToken{
        QStringLiteral("audio_camp"),
        QStringLiteral("1,0"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(audioCampStoppedToken,
                                            &mergedMetadata)
        || mergedMetadata.campStatus != CampStatus::AudioStopped) {
        return fail("MSG audio_camp metadata update is wrong");
    }
    const MsgToken campDisconnectToken{
        QStringLiteral("camp_disconnect"),
        QString(),
        false,
    };
    if (!updateReceiverMetadataFromMsgToken(campDisconnectToken,
                                            &mergedMetadata)
        || mergedMetadata.campStatus != CampStatus::Disconnected) {
        return fail("MSG camp_disconnect metadata update is wrong");
    }
    ReceiverMetadata mergedCampMetadata;
    if (!mergeReceiverMetadata(&mergedCampMetadata, mergedMetadata)
        || !mergedCampMetadata.hasCampStatus
        || mergedCampMetadata.campStatus != CampStatus::Disconnected
        || !mergedCampMetadata.hasMaxCampers
        || mergedCampMetadata.maxCampers != 3
        || !mergedCampMetadata.hasCampQueuePosition
        || mergedCampMetadata.campQueuePosition != 1) {
        return fail("camp metadata merge is wrong");
    }
    const MsgToken unknownStatusToken{
        QStringLiteral("agc_mode"),
        QStringLiteral("fast"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(unknownStatusToken, &mergedMetadata)
        || !mergedMetadata.stableStatusFields.contains(
            QStringLiteral("agc_mode=fast"))) {
        return fail("unknown MSG metadata retention is wrong");
    }
    ReceiverMetadata retentionMetadata;
    const MsgToken stableFieldToken{
        QStringLiteral("rx_location"),
        QStringLiteral("grid CN87"),
        true,
    };
    if (!updateReceiverMetadataFromMsgToken(stableFieldToken,
                                            &retentionMetadata)) {
        return fail("stable MSG metadata retention did not record unknown field");
    }
    for (int i = 0; i < 24; ++i) {
        updateReceiverMetadataFromMsgToken(
            MsgToken{QStringLiteral("center_freq"),
                     QString::number(15000000 + i),
                     true},
            &retentionMetadata);
        updateReceiverMetadataFromMsgToken(
            MsgToken{QStringLiteral("bandwidth"),
                     QString::number(30000000 + i),
                     true},
            &retentionMetadata);
        updateReceiverMetadataFromMsgToken(
            MsgToken{QStringLiteral("wf_fps"), QString::number(i), true},
            &retentionMetadata);
        updateReceiverMetadataFromMsgToken(
            MsgToken{QStringLiteral("zoom"), QString::number(i % 14), true},
            &retentionMetadata);
        updateReceiverMetadataFromMsgToken(
            MsgToken{QStringLiteral("audio_rate"),
                     QStringLiteral("12000"),
                     true},
            &retentionMetadata);
    }
    if (!retentionMetadata.stableStatusFields.contains(
            QStringLiteral("rx_location=grid CN87"))
        || retentionMetadata.stableStatusFields.contains(
            QStringLiteral("wf_fps=23"))
        || retentionMetadata.stableStatusFields.contains(
            QStringLiteral("zoom=9"))
        || retentionMetadata.stableStatusFields.contains(
            QStringLiteral("audio_rate=12000"))) {
        return fail("high-volume MSG metadata should not evict stable fields");
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

    const WaterfallDisplayRange autoScale =
        autoWaterfallDisplayRange(row);
    if (!autoScale.valid
        || !nearlyEqual(autoScale.minDbm, -110.0f)
        || !nearlyEqual(autoScale.maxDbm, -30.0f)) {
        return fail("auto waterfall display range does not follow spec indices");
    }

    QVector<float> signalSignRow(1024, -100.0f);
    for (int i = 1003; i < signalSignRow.size(); ++i) {
        signalSignRow[i] = -70.0f;
    }
    const WaterfallDisplayRange signAutoScale =
        autoWaterfallDisplayRange(signalSignRow);
    if (!signAutoScale.valid
        || !nearlyEqual(signAutoScale.minDbm, -110.0f)
        || !nearlyEqual(signAutoScale.maxDbm, -40.0f)) {
        return fail("auto waterfall signal headroom must add 30 dB algebraically");
    }

    QVector<float> percentileAnchorRow(1024, -100.0f);
    for (int i = 1003; i < percentileAnchorRow.size() - 1; ++i) {
        percentileAnchorRow[i] = -60.0f;
    }
    percentileAnchorRow[percentileAnchorRow.size() - 1] = -10.0f;
    const WaterfallDisplayRange anchoredAutoScale =
        autoWaterfallDisplayRange(percentileAnchorRow);
    if (!anchoredAutoScale.valid
        || !nearlyEqual(anchoredAutoScale.maxDbm, -30.0f)) {
        return fail("auto waterfall signal percentile must stay anchored at index 1003");
    }

    QVector<float> wrongSizeRow(1023, -100.0f);
    if (autoWaterfallDisplayRange(wrongSizeRow).valid) {
        return fail("auto waterfall display range accepted a non-1024 row");
    }

    QVector<float> quietRow(1024, -255.0f);
    for (int i = 512; i < quietRow.size(); ++i) {
        quietRow[i] = -200.0f;
    }
    for (int i = 1003; i < quietRow.size(); ++i) {
        quietRow[i] = -60.0f;
    }
    const WaterfallDisplayRange quietAutoScale =
        autoWaterfallDisplayRange(quietRow);
    if (!quietAutoScale.valid
        || !nearlyEqual(quietAutoScale.minDbm, -210.0f)
        || !nearlyEqual(quietAutoScale.maxDbm, -30.0f)) {
        return fail("auto waterfall display range clipped quiet bins");
    }

    QVector<float> brighterRow(1024, -80.0f);
    for (int i = 1000; i < brighterRow.size(); ++i) {
        brighterRow[i] = -40.0f;
    }
    QVector<float> dimmerRow(1024, -100.0f);
    for (int i = 1000; i < dimmerRow.size(); ++i) {
        dimmerRow[i] = -60.0f;
    }
    const WaterfallDisplayRange averagedAutoScale =
        autoWaterfallDisplayRangeFromRows({dimmerRow, brighterRow});
    if (!averagedAutoScale.valid
        || !nearlyEqual(averagedAutoScale.minDbm, -100.0f)
        || !nearlyEqual(averagedAutoScale.maxDbm, -20.0f)) {
        return fail("multi-row auto waterfall range did not average frames by bin");
    }

    QVector<float> edgeCarrierRow(1024, -100.0f);
    for (int i = 1003; i < edgeCarrierRow.size(); ++i) {
        edgeCarrierRow[i] = -60.0f;
    }
    const WaterfallDisplayRange spatialAutoScale =
        autoWaterfallDisplayRangeFromRows({edgeCarrierRow});
    if (!spatialAutoScale.valid
        || !nearlyEqual(spatialAutoScale.minDbm, -110.0f)
        || !nearlyEqual(spatialAutoScale.maxDbm, -43.333332f, 0.0002f)) {
        return fail("multi-row auto waterfall range did not spatially smooth bins");
    }

    const float index = waterfallColorIndex(-90.0f, -110.0f, -30.0f);
    if (!nearlyEqual(index, 0.25f)) {
        return fail("waterfall color index does not linearly normalize dBm range");
    }
    if (!nearlyEqual(waterfallColorIndex(-140.0f, -110.0f, -30.0f), 0.0f)
        || !nearlyEqual(waterfallColorIndex(-5.0f, -110.0f, -30.0f), 1.0f)
        || !nearlyEqual(waterfallColorIndex(-40.0f, -40.0f, -110.0f), 0.0f)) {
        return fail("waterfall color index does not clamp normalized range");
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
