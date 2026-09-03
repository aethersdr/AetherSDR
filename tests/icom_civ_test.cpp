// IcomCIV Phase 1 — CI-V command-plane test. Pins the framing, the two BCD
// endiannesses, the reassembler's resynchronisation rules, the mode vocabulary
// mapping, and the scope command encodings.
//
// Pure protocol: no sockets, no Qt, no hardware.

#include "core/backends/icom/CivCodec.h"
#include "core/DtcsCodes.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR::icom;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr std::uint8_t kIc705 = 0xA4;

static bool bytesAre(const std::vector<std::uint8_t>& got,
                     std::initializer_list<std::uint8_t> want)
{
    if (got.size() != want.size())
        return false;
    std::size_t i = 0;
    for (std::uint8_t b : want)
        if (got[i++] != b)
            return false;
    return true;
}

static void testFraming()
{
    auto f = buildFrame(kIc705, cmd::kReadFreq);
    check(bytesAre(f, {0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFD}), "bare read-frequency frame");

    auto s = buildFrameSub(kIc705, cmd::kScope, scope::kOnOff, std::array<std::uint8_t, 1>{0x01});
    check(bytesAre(s, {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x10, 0x01, 0xFD}), "scope on frame");

    auto parsed = parseFrame(s);
    check(parsed.has_value(), "parses a sub-addressed frame");
    check(parsed->to == 0xA4 && parsed->from == 0xE0, "addresses");
    check(parsed->cmd == 0x27 && parsed->hasSub && parsed->sub == 0x10, "command and subcommand");
    check(parsed->data.size() == 1 && parsed->data[0] == 0x01, "payload");

    // Command 0x05 has NO subcommand: its first data byte is a frequency digit.
    // Treating every second byte as a subcommand would eat it.
    auto freqFrame = cmdSetFrequency(kIc705, 14195000);
    auto pf = parseFrame(freqFrame);
    check(pf.has_value() && !pf->hasSub, "set-frequency has no subcommand");
    check(pf->data.size() == 5, "all five frequency bytes survive as data");

    const auto rxAntOn = cmdSetRxAntenna(0xB6, true);
    check(bytesAre(rxAntOn,
                   {0xFE, 0xFE, 0xB6, 0xE0, 0x12, 0x00, 0x01, 0xFD}),
          "IC-7300MK2 RX-ANT enable frame");
    const auto parsedRxAnt = parseFrame(rxAntOn);
    check(parsedRxAnt && parsedRxAnt->hasSub && parsedRxAnt->sub == 0x00
              && parsedRxAnt->data == std::vector<std::uint8_t>{0x01},
          "RX-ANT reply keeps subcommand separate from value");

    // FB / FA acknowledgements.
    auto ok = parseFrame(std::vector<std::uint8_t>{0xFE, 0xFE, 0xE0, 0xA4, 0xFB, 0xFD});
    check(ok.has_value() && ok->isOk(), "FB is an acknowledgement");
    auto ng = parseFrame(std::vector<std::uint8_t>{0xFE, 0xFE, 0xE0, 0xA4, 0xFA, 0xFD});
    check(ng.has_value() && ng->isNg(), "FA is a rejection");

    check(!parseFrame(std::vector<std::uint8_t>{0xFE, 0xA4, 0xE0, 0x03, 0xFD}).has_value(),
          "a single preamble byte is not a frame");
}

static void testBcd()
{
    // FREQUENCY runs LITTLE-endian — least significant digit pair first. This
    // is Icom's own manual example.
    check(bytesAre(encodeFreq(14195000), {0x00, 0x50, 0x19, 0x14, 0x00}),
          "14.195000 MHz encodes little-endian BCD");
    check(bytesAre(encodeFreq(7074000), {0x00, 0x40, 0x07, 0x07, 0x00}),
          "7.074000 MHz encodes little-endian BCD");
    check(bytesAre(encodeFreq(430000000), {0x00, 0x00, 0x00, 0x30, 0x04}),
          "430 MHz reaches the top digit pair");

    for (std::uint64_t hz : {30000ULL, 7074000ULL, 14195000ULL, 144200000ULL, 470000000ULL}) {
        auto rt = decodeFreq(encodeFreq(hz));
        check(rt.has_value() && *rt == hz, "frequency round-trips");
    }

    // A nibble above 9 is not BCD. REJECT rather than clamp: a corrupt
    // frequency that decodes to something still retunes the radio, and on
    // transmit that is an out-of-band emission.
    const std::vector<std::uint8_t> corrupt{0x00, 0x5A, 0x19, 0x14, 0x00};
    check(!decodeFreq(corrupt).has_value(), "a non-BCD nibble is rejected, not clamped");

    // SCOPE EDGES can be negative. The IC-7300MK2's guide documents 0xF in the
    // 1 GHz digit — the high nibble of the last byte — as a sign flag, for when
    // a wide span sits near the bottom of the tuning range and the window
    // extends below 0 Hz.
    auto lower = encodeFreq(500'000);
    lower.back() |= 0xF0;                       // flag it negative
    check(!decodeFreq(lower).has_value(),
          "the STRICT decoder still rejects it — an operating frequency is never negative");
    auto signedLower = decodeFreqSigned(lower);
    check(signedLower.has_value() && *signedLower == -500'000,
          "but the signed decoder reads it as -500 kHz");
    // Positive values must survive the signed path unchanged.
    check(decodeFreqSigned(encodeFreq(14'195'000)).value_or(0) == 14'195'000,
          "and a positive edge decodes identically through the signed path");
    // Corruption must still be refused, sign flag or not.
    check(!decodeFreqSigned(corrupt).has_value(), "the signed decoder is not a corruption bypass");

    // EVERYTHING ELSE runs BIG-endian.
    auto lvl = encodeLevel(255);
    check(lvl[0] == 0x02 && lvl[1] == 0x55, "level 0255 is big-endian BCD");
    check(decodeLevel(lvl).value_or(-1) == 255, "level round-trips");
    check(decodeLevel(std::array<std::uint8_t, 2>{0x01, 0x20}).value_or(-1) == 120,
          "S9 raw value 120 decodes");

    // The radio displays level registers by truncating 0..255 into 0..100.
    // Every requested percentage must therefore be encoded into the FIRST raw
    // value in that display bucket. The old floor-write / nearest-read pair
    // made the app one point ahead of the radio across roughly half the range.
    for (int pct = 0; pct <= 100; ++pct) {
        const int raw = percentToLevelRaw(pct);
        check(raw >= 0 && raw <= 255, "percent encoding stays inside 0..255");
        check(levelRawToPercent(raw) == pct,
              "every displayed percentage round-trips through the Icom register");
    }
    check(percentToLevelRaw(10) == 26,
          "10% selects raw 26, whose radio display is 10 (raw 25 displays 9)");
    check(levelRawToPercent(25) == 9,
          "raw 25 follows the radio's truncated display instead of rounding up");

    check(encodeBcdByte(11) == 0x11, "division 11 is BCD 0x11, not 0x0b");
    check(decodeBcdByte(0x11) == 11, "and decodes back");
}

static void testFmRepeaterCommands()
{
    check(bytesAre(cmdReadRepeaterOffsetDirection(kIc705),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x0F, 0xFD}),
          "repeater direction read frame");
    check(bytesAre(cmdSetRepeaterOffsetDirection(kIc705,
                                                  RepeaterOffsetDirection::Down),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x0F, 0x11, 0xFD}),
          "DUP- writes direction 0x11");
    check(decodeRepeaterOffsetDirection(std::array<std::uint8_t, 1>{0x12})
              == RepeaterOffsetDirection::Up,
          "DUP+ direction decodes");
    check(!decodeRepeaterOffsetDirection(std::array<std::uint8_t, 1>{0x13}),
          "unknown repeater direction is rejected");

    check(bytesAre(cmdSetRepeaterOffset(kIc705, 600'000),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x0D, 0x00, 0x60, 0x00, 0xFD}),
          "600 kHz offset is little-endian BCD in 100 Hz units");
    check(decodeRepeaterOffsetHz(std::array<std::uint8_t, 3>{0x00, 0x60, 0x00})
              == 600'000,
          "repeater offset decodes to Hz");
    check(!decodeRepeaterOffsetHz(std::array<std::uint8_t, 3>{0x00, 0x6A, 0x00}),
          "non-BCD repeater offset is rejected");

    check(bytesAre(cmdReadRepeaterTone(kIc705),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1B, 0x00, 0xFD}),
          "repeater tone read frame");
    check(bytesAre(cmdSetRepeaterTone(kIc705, 88.5),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1B, 0x00, 0x00, 0x08, 0x85, 0xFD}),
          "88.5 Hz CTCSS is big-endian BCD in tenths");
    check(std::abs(decodeRepeaterToneHz(
                       std::array<std::uint8_t, 3>{0x00, 0x10, 0x00})
                       .value_or(0.0) - 100.0) < 0.001,
          "100.0 Hz CTCSS decodes");
    check(!decodeRepeaterToneHz(std::array<std::uint8_t, 3>{0x01, 0x10, 0x00}),
          "invalid repeater tone prefix is rejected");
    check(bytesAre(cmdReadRepeaterAccess(0xA2),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x16, 0x5D, 0xFD}),
          "IC-9700 extended access read is 16 5D");
    check(decodeRepeaterAccess(std::array<std::uint8_t, 1>{0x08}) == 0x08,
           "documented mixed DTCS/TSQL access value decodes");
    check(!decodeRepeaterAccess(std::array<std::uint8_t, 1>{0x04}),
           "reserved repeater access value is rejected");
    const std::array<std::pair<std::uint8_t, std::string_view>, 8> accessModes{{
        {0x00, "off"},
        {0x01, "ctcss_tx"},
        {0x02, "ctcss_rx"},
        {0x03, "dtcs_txrx"},
        {0x06, "dtcs_tx"},
        {0x07, "ctcss_tx_dtcs_rx"},
        {0x08, "dtcs_tx_ctcss_rx"},
        {0x09, "ctcss_txrx"},
    }};
    for (const auto& [wireValue, expectedMode] : accessModes) {
        check(repeaterAccessModeName(wireValue) == expectedMode,
              "every documented 16 5D value keeps its radio-authoritative meaning");
        check(repeaterAccessModeValue(expectedMode) == wireValue,
              "every selectable repeater access mode round-trips to its wire value");
    }
    check(repeaterAccessModeName(0x04).empty(),
          "reserved 16 5D value has no normalized state token");
    check(!repeaterAccessModeValue("future_dtcs_mode"),
          "unknown repeater access intent fails closed");
    check(bytesAre(cmdReadRepeaterToneRegister(0xA2, repeaterTone::kRxCtcss),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1B, 0x01, 0xFD}),
          "IC-9700 RX CTCSS read is 1B 01");
    check(bytesAre(cmdSetRepeaterToneRegister(
                       0xA2, repeaterTone::kRxCtcss, 885),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1B, 0x01,
                    0x00, 0x08, 0x85, 0xFD}),
          "IC-9700 RX CTCSS write uses the extended 1B 01 register");
    check(bytesAre(cmdSetRepeaterToneRegister(
                       0xA2, repeaterTone::kDtcs, 23, true, false),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1B, 0x02,
                    0x10, 0x00, 0x23, 0xFD}),
          "IC-9700 DTCS write preserves code and independent polarity");
    for (const std::uint8_t registerId : {
             repeaterTone::kTxCtcss, repeaterTone::kRxCtcss, repeaterTone::kDtcs}) {
        const auto write = parseFrame(cmdSetRepeaterToneRegister(
            0xA2, registerId, registerId == repeaterTone::kDtcs ? 23 : 885));
        const auto confirmation = write
            ? repeaterToneConfirmationForWrite(0xA2, *write) : std::nullopt;
        check(confirmation
                  && *confirmation == cmdReadRepeaterToneRegister(0xA2, registerId),
              "every repeater tone write receives its matching radio readback");
    }
    const auto dtcs = decodeRepeaterToneRegister(
        std::array<std::uint8_t, 3>{0x11, 0x00, 0x23});
    check(dtcs && dtcs->value == 23 && dtcs->txReverse && dtcs->rxReverse,
          "DTCS code and independent polarity bits decode");
    check(bytesAre(cmdSetDtcsTone(0xA2, 23, true, false),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1B, 0x02,
                    0x10, 0x00, 0x23, 0xFD}),
          "IC-9700 DTCS write preserves the leading zero and TX polarity");
    check(cmdSetDtcsTone(0xA2, 1000, false, false).empty(),
          "out-of-range DTCS code is refused before frame construction");
    check(cmdSetDtcsTone(0xA2, 123, false, false).empty()
              && cmdSetDtcsTone(0xA2, 888, false, false).empty(),
          "non-standard and non-octal DTCS intent is refused by the encoder");
    check(AetherSDR::kDtcsCodes.size() == 104
              && AetherSDR::isCanonicalDtcsCode(23)
              && AetherSDR::isCanonicalDtcsCode(754)
              && !AetherSDR::isCanonicalDtcsCode(123),
          "DTCS UI and backend share the standard 104-code vocabulary");
    check(!decodeRepeaterToneRegister(
              std::array<std::uint8_t, 3>{0x02, 0x00, 0x23}),
          "reserved DTCS polarity bits are rejected");
    check(!decodeRepeaterToneRegister(
              std::array<std::uint8_t, 3>{0x00, 0x0A, 0x23}),
          "non-BCD extended tone data is rejected");
    const std::array<std::uint8_t, 5> txFrequency{
        0x00, 0x56, 0x42, 0x48, 0x04};
    check(decodeFreqExact(txFrequency, kFreqBytes) == 448'425'600ULL,
          "IC-9700 transmit-frequency readback decodes at exact arity");
    check(!decodeFreqExact(
              std::array<std::uint8_t, 4>{0x00, 0x56, 0x42, 0x48}, kFreqBytes),
          "truncated transmit-frequency readback is rejected");
    check(!decodeFreqExact(
              std::array<std::uint8_t, 6>{0x00, 0x56, 0x42, 0x48, 0x04, 0x00},
              kFreqBytes),
          "oversized transmit-frequency readback is rejected");
    check(bytesAre(cmdSetRepeaterAccess(kIc705, 0x09),
                   {0xFE, 0xFE, kIc705, kControllerAddress, 0x16, 0x5D, 0x09, 0xFD}),
          "CTCSS TX/RX access selector write frame");
    check(bytesAre(cmdReadRepeaterToneRegister(
                       kIc705, repeaterTone::kRxCtcss),
                   {0xFE, 0xFE, kIc705, kControllerAddress, 0x1B, 0x01, 0xFD}),
          "receive CTCSS tone read frame");
    check(bytesAre(cmdSetCtcssTone(kIc705, repeaterTone::kRxCtcss, 103.5),
                   {0xFE, 0xFE, kIc705, kControllerAddress,
                    0x1B, 0x01, 0x00, 0x10, 0x35, 0xFD}),
          "receive CTCSS tone write frame");
}

static void testReassembler()
{
    CivReassembler r;

    // Rule 1: a lone 0xFE is not a frame start. Feed one inside noise and check
    // the decoder does not latch onto it.
    auto none = r.feed(std::vector<std::uint8_t>{0x00, 0xFE, 0x12, 0x34});
    check(none.empty(), "a lone preamble byte does not start a frame");
    check(!r.framePending(), "and leaves nothing pending");

    // A frame split across three chunks.
    r.reset();
    auto a = r.feed(std::vector<std::uint8_t>{0xFE, 0xFE});
    check(a.empty() && r.framePending(), "double preamble starts a frame");
    auto b = r.feed(std::vector<std::uint8_t>{0xA4, 0xE0, 0x03});
    check(b.empty(), "still assembling");
    auto c = r.feed(std::vector<std::uint8_t>{0x00, 0x50, 0x19, 0x14, 0x00, 0xFD});
    check(c.size() == 1, "the terminator completes it");
    check(c[0].size() == 11, "and the whole frame is delivered");

    // Two frames in one datagram.
    r.reset();
    std::vector<std::uint8_t> two{0xFE, 0xFE, 0xA4, 0xE0, 0xFB, 0xFD,
                                  0xFE, 0xFE, 0xA4, 0xE0, 0xFB, 0xFD};
    check(r.feed(two).size() == 2, "batched frames are split");

    // 0xFC also terminates.
    r.reset();
    auto fc = r.feed(std::vector<std::uint8_t>{0xFE, 0xFE, 0xA4, 0xE0, 0x03, 0xFC});
    check(fc.size() == 1, "0xFC terminates a frame too");

    // Rule 3: a truncated frame must be abandoned, or it swallows every
    // subsequent byte forever and the radio appears to stop answering.
    r.reset();
    (void)r.feed(std::vector<std::uint8_t>{0xFE, 0xFE, 0xA4, 0xE0, 0x03});
    check(r.framePending(), "partial frame is pending");
    r.timeout();
    check(!r.framePending(), "timeout abandons it");
    auto after = r.feed(std::vector<std::uint8_t>{0xFE, 0xFE, 0xA4, 0xE0, 0xFB, 0xFD});
    check(after.size() == 1, "and the next frame decodes cleanly");

    // A full 475-point scope sweep must survive. The cap is NOT 80 — that is
    // the longest COMMAND frame, and an 80-byte cap silently discards every
    // panadapter frame while commands keep working perfectly.
    r.reset();
    std::vector<std::uint8_t> sweep{0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x00};
    sweep.insert(sweep.end(), 490, 0x40);
    sweep.push_back(0xFD);
    auto sweepOut = r.feed(sweep);
    check(sweepOut.size() == 1, "a ~496-byte scope sweep is NOT dropped by the frame cap");
    check(sweepOut[0].size() == sweep.size(), "and arrives whole");

    // A runaway frame is still capped rather than growing without bound.
    r.reset();
    std::vector<std::uint8_t> runaway{0xFE, 0xFE};
    runaway.insert(runaway.end(), kMaxFrameBytes + 50, 0x11);
    auto capped = r.feed(runaway);
    check(capped.empty(), "a frame past the cap is dropped, not emitted");
    check(!r.framePending(), "and the decoder resynchronises");
}

static void testModes()
{
    bool data = false;
    check(modeFromNeutral("USB", data) == CivMode::Usb && !data, "USB");
    check(modeFromNeutral("LSB", data) == CivMode::Lsb && !data, "LSB");
    // The data modes are the same sideband with the DATA flag — that flag is
    // what routes audio to the WLAN modulator instead of the microphone.
    check(modeFromNeutral("DIGU", data) == CivMode::Usb && data, "DIGU is USB + data");
    check(modeFromNeutral("DIGL", data) == CivMode::Lsb && data, "DIGL is LSB + data");
    // DFM is FM + DATA, and it was missing entirely. Selecting DFM fell through
    // to the "no equivalent" path, so setSliceMode() re-asserted the radio's
    // current mode and the control appeared to do nothing. The expensive half
    // was the DATA flag: a radio already in FM-D reported back as plain FM, so
    // the next mode write cleared the flag and the radio transmitted from the
    // MICROPHONE rather than the WLAN modulator — a 2 m AX.25 frame keyed the
    // rig and put room noise on the air. Found on an IC-9700, 2026-08-09.
    check(modeFromNeutral("DFM", data) == CivMode::Fm && data, "DFM is FM + data");
    check(modeFromNeutral("FM", data) == CivMode::Fm && !data, "plain FM has no data flag");
    check(modeFromNeutral("CW", data) == CivMode::Cw, "bare CW maps, not just CWU");
    check(modeFromNeutral("CWL", data) == CivMode::CwR, "CWL is CW-reverse");
    // No IC-705 equivalent: refuse rather than silently substituting USB.
    check(!modeFromNeutral("SAM", data).has_value(), "SAM has no equivalent and is refused");
    check(!modeFromNeutral("DRM", data).has_value(), "DRM has no equivalent and is refused");

    check(modeToNeutral(CivMode::Usb, false) == "USB", "reverse USB");
    check(modeToNeutral(CivMode::Usb, true) == "DIGU", "reverse DIGU");
    check(modeToNeutral(CivMode::Cw, false) == "CW", "normal CW is reported as CW");
    // The round trip has to survive too. Returning plain "FM" for a radio in
    // FM-D made the UI show FM, and the next mode write then sent FM with the
    // flag CLEAR — silently taking the radio out of data mode and back onto the
    // microphone without the operator touching anything.
    check(modeToNeutral(CivMode::Fm, false) == "FM", "reverse FM");
    check(modeToNeutral(CivMode::Fm, true) == "DFM", "reverse DFM keeps the data flag");
    check(modeToNeutral(CivMode::Dv, false).empty(), "D-STAR has no neutral mode");

    // THE LADDER IS PER MODE. FIL1 is 3.0 kHz in SSB, 1.2 kHz in CW, 9 kHz in
    // AM and 15 kHz in FM, so snapping every mode against the SSB thresholds —
    // which this used to do — put every AM width on FIL1 and every CW width on
    // FIL3. Three buttons, one filter, in both directions.
    check(filterForWidthHz("USB", 3000) == 1, "SSB 3.0 kHz selects FIL1");
    check(filterForWidthHz("USB", 2400) == 2, "SSB 2.4 kHz selects FIL2");
    check(filterForWidthHz("USB", 1800) == 3, "SSB 1.8 kHz selects FIL3");

    check(filterForWidthHz("CW", 1200) == 1, "CW 1.2 kHz selects FIL1");
    check(filterForWidthHz("CW", 500) == 2, "CW 500 Hz selects FIL2");
    check(filterForWidthHz("CW", 250) == 3, "CW 250 Hz selects FIL3");
    // The regression in one line: under the old SSB thresholds every one of
    // these was FIL3, because they are all below 2100.
    check(filterForWidthHz("CW", 1200) != filterForWidthHz("CW", 250),
          "and CW widths do not all collapse onto one slot");

    check(filterForWidthHz("AM", 9000) == 1, "AM 9 kHz selects FIL1");
    check(filterForWidthHz("AM", 6000) == 2, "AM 6 kHz selects FIL2");
    check(filterForWidthHz("AM", 3000) == 3, "AM 3 kHz selects FIL3");
    check(filterForWidthHz("AM", 9000) != filterForWidthHz("AM", 3000),
          "and AM widths do not all collapse onto one slot");

    check(filterForWidthHz("FM", 15000) == 1, "FM 15 kHz selects FIL1");
    check(filterForWidthHz("FM", 7000) == 3, "FM 7 kHz selects FIL3");

    // SAM is AM's ladder. It used to fall through to SSB, so a broadcast
    // station in synchronous AM was received through a 2.4 kHz filter.
    check(filterWidthsForMode("SAM") == filterWidthsForMode("AM"),
          "SAM takes AM's filter ladder, not SSB's");

    // NARROWEST FIRST, deliberately the REVERSE of the radio's FIL numbering
    // (FIL1 is the widest slot). Listing it in FIL order put 3.0k/2.4k/1.8k on
    // screen, reading wide-to-narrow while every other filter row in the app
    // runs narrow-to-wide — the buttons looked reversed because they were.
    check((filterWidthsForMode("USB") == std::vector<int>{1800, 2400, 3000}),
          "the published SSB ladder is narrow-to-wide, matching the rest of the UI");
    check((filterWidthsForMode("AM") == std::vector<int>{3000, 6000, 9000}),
          "and so is AM's");
    // The ORDER of the published list must not change which slot a width picks.
    check(filterForWidthHz("USB", 3000) == 1 && filterForWidthHz("USB", 1800) == 3,
          "and the FIL mapping is unaffected by the display order");
    // WFM has ONE filter; publishing it three times would give the operator
    // three identical buttons, two of which read as broken.
    check(filterWidthsForMode("WFM").size() == 1, "WFM publishes its single filter once");

    // A sideband passband sits off the carrier and carries its sideband in the
    // sign; AM and its relatives straddle it.
    {
        const auto [lo, hi] = passbandForModeAndFilter("USB", 2);
        check(lo == 300 && hi == 2700, "USB FIL2 is +300..+2700");
    }
    {
        const auto [lo, hi] = passbandForModeAndFilter("LSB", 2);
        check(lo == -2700 && hi == -300, "LSB FIL2 mirrors it");
    }
    {
        const auto [lo, hi] = passbandForModeAndFilter("AM", 2);
        check(lo == -3000 && hi == 3000, "AM FIL2 straddles the carrier at 6 kHz wide");
    }
    {
        // The reported bug, as a number: SAM used to come out at -1500..1500.
        const auto [lo, hi] = passbandForModeAndFilter("SAM", 2);
        check(hi - lo == 6000, "SAM FIL2 is 6 kHz wide, not the 3 kHz SSB fallback");
    }
}

static void testCommands()
{
    check(bytesAre(cmdSetPtt(kIc705, true), {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x01, 0xFD}),
          "PTT on is 1C 00 01");
    check(bytesAre(cmdSetTransmitFrequencyCheck(kIc705, true),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x02, 0x01, 0xFD}),
          "IC-705 XFC press is 1C 02 01");
    check(bytesAre(cmdSetTransmitFrequencyCheck(0xA2, false),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1C, 0x02, 0x00, 0xFD}),
          "IC-9700 XFC release uses the model address and 1C 02 00");
    check(bytesAre(cmdReadTransmitFrequencyCheck(0xA2),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1C, 0x02, 0xFD}),
          "IC-9700 XFC read is 1C 02 with no payload");
    check(bytesAre(cmdReadTransmitFrequency(0xA2),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1C, 0x03, 0xFD}),
          "IC-9700 transmit-frequency read is 1C 03");
    check(bytesAre(cmdReadMeter(kIc705, meter::kSMeter),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x15, 0x02, 0xFD}),
          "S-meter read is 15 02");
    check(bytesAre(cmdReadId(kIc705), {0xFE, 0xFE, 0xA4, 0xE0, 0x19, 0x00, 0xFD}),
          "model discovery is 19 00");
    check(bytesAre(cmdSetMode(kIc705, CivMode::Usb, 1),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x06, 0x01, 0x01, 0xFD}),
          "set mode USB FIL1");
    check(bytesAre(cmdSetLevel(kIc705, level::kRfPower, 255),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x14, 0x0A, 0x02, 0x55, 0xFD}),
          "RF power 100% is 14 0A 0255");
    check(bytesAre(cmdSendCwMessage(kIc705, "CQ TEST"),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x17, 0x43, 0x51, 0x20,
                    0x54, 0x45, 0x53, 0x54, 0xFD}),
          "CW text is command 17 followed by ASCII");
    check(bytesAre(cmdAbortCwMessage(kIc705),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x17, 0xFF, 0xFD}),
          "CW abort is 17 FF");
    check(bytesAre(cmdSetFunction(kIc705, func::kBreakIn, 1),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x16, 0x47, 0x01, 0xFD}),
          "semi break-in is 16 47 01");
    check(bytesAre(cmdSetTuner(kIc705, 0x02),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x01, 0x02, 0xFD}),
          "ATU start is 1C 01 02");
    check(bytesAre(cmdSetTuner(kIc705, 0x00),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x01, 0x00, 0xFD}),
          "ATU bypass is 1C 01 00");

    // BOTH scope switches exist and both are needed — enabling only the first
    // turns the scope on the radio's screen and sends us nothing.
    check(bytesAre(cmdScopeOnOff(kIc705, true), {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x10, 0x01, 0xFD}),
          "scope on is 27 10 01");
    check(bytesAre(cmdScopeDataOutput(kIc705, true),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x11, 0x01, 0xFD}),
          "scope DATA OUTPUT is a separate command, 27 11 01");

    check(bytesAre(cmdScopeMode(kIc705, true), {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x14, 0x00, 0x01, 0xFD}),
          "fixed mode is 27 14 0001 — two BCD bytes, not one");

    // The span snaps to the eight the radio has.
    check(nearestScopeSpanHz(37000) == 25000, "37 kHz snaps down to 25 kHz");
    check(nearestScopeSpanHz(1) == 2500, "below the floor snaps to the narrowest");
    check(nearestScopeSpanHz(9'999'999) == 500000, "above the ceiling snaps to the widest");
    // SIX data bytes. The leading 0x00 is not padding — omit it and the radio
    // ignores the frame silently, which is exactly how "zoom does nothing"
    // presented on a live IC-705.
    check(bytesAre(cmdScopeSpan(kIc705, 100000),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x15,
                    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0xFD}),
          "span is a LEADING 0x00 then a 5-byte little-endian BCD frequency");

    // The sign is a SEPARATE byte. Folding it into the magnitude puts the
    // reference level 20 dB out.
    auto refPlus = cmdScopeReference(kIc705, 10.5);
    check(bytesAre(refPlus,
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x19, 0x00, 0x10, 0x50, 0x00, 0xFD}),
          "+10.5 dB reference, behind the same leading 0x00");
    auto refMinus = cmdScopeReference(kIc705, -20.0);
    check(bytesAre(refMinus,
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x27, 0x19, 0x00, 0x20, 0x00, 0x01, 0xFD}),
          "-20.0 dB reference, sign in its own byte");
    auto refClamped = cmdScopeReference(kIc705, 99.0);
    // Indices shifted by one when the leading fixed 0x00 was added.
    check(refClamped[9] == 0x00 && refClamped[7] == 0x20,
          "out-of-range reference clamps to +20.0 rather than wrapping");

    check(bytesAre(cmdWriteSetting(0xA4, 118, 0x03),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1A, 0x05, 0x01, 0x18, 0x03, 0xFD}),
          "IC-705 PC Audio selects WLAN with SET 0118 value 03");
    check(bytesAre(cmdWriteSetting(0xB6, 84, 0x05),
                   {0xFE, 0xFE, 0xB6, 0xE0, 0x1A, 0x05, 0x00, 0x84, 0x05, 0xFD}),
          "IC-7300MK2 PC Audio selects LAN with SET 0084 value 05");
    check(bytesAre(cmdWriteSetting(0xB6, 84, 0x00),
                   {0xFE, 0xFE, 0xB6, 0xE0, 0x1A, 0x05, 0x00, 0x84, 0x00, 0xFD}),
          "IC-7300MK2 PC Audio off restores MIC without touching DATA MOD 0085");
    check(bytesAre(cmdReadSetting(0xA2, 114),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1A, 0x05, 0x01, 0x14, 0xFD}),
          "IC-9700 LAN MOD level read is SET 0114");
    check(bytesAre(cmdWriteSettingLevel(0xA2, 114, 26),
                   {0xFE, 0xFE, 0xA2, 0xE0, 0x1A, 0x05, 0x01, 0x14,
                    0x00, 0x26, 0xFD}),
          "IC-9700 LAN MOD 10% writes the documented two-byte 0026 level");
}

// DATA mode — command 26.
//
// The command that tells USB from USB-D. Every check here is about the fact
// that DATA is NOT in the mode byte: commands 01/04/06 cannot carry it, and 26
// carries it together with the mode and the filter slot so the three cannot
// clobber each other.
static void testDataMode()
{
    check(bytesAre(cmdReadVfoMode(kIc705), {0xFE, 0xFE, 0xA4, 0xE0, 0x26, 0x00, 0xFD}),
          "the mode/DATA/filter read is 26 00 with no payload");
    check(bytesAre(cmdSetVfoMode(kIc705, CivMode::Usb, true, 2),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x26, 0x00, 0x01, 0x01, 0x02, 0xFD}),
          "USB-D on FIL2 is 26 00 01 01 02");
    check(bytesAre(cmdSetVfoMode(kIc705, CivMode::Usb, false, 2),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x26, 0x00, 0x01, 0x00, 0x02, 0xFD}),
          "plain USB on FIL2 differs ONLY in the DATA byte — the mode byte is "
          "the same 0x01, which is why 06 alone can never express the pair");
    check(bytesAre(cmdSetVfoMode(kIc705, CivMode::Lsb, true, 9),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x26, 0x00, 0x00, 0x01, 0x03, 0xFD}),
          "an out-of-range slot clamps to 1..3 rather than reaching the radio");

    // Exercise the complete receive path, including parseFrame's per-command
    // sub-address classification. Direct payload tests cannot catch a selector
    // byte accidentally left at data[0].
    const std::array<std::uint8_t, 10> reply{
        0xFE, 0xFE, 0xE0, 0xA4, 0x26, 0x00, 0x01, 0x01, 0x02, 0xFD};
    const auto parsed = parseFrame(reply);
    check(parsed && parsed->cmd == cmd::kVfoMode && parsed->hasSub
              && parsed->sub == vfoMode::kSelected,
          "a full 26 00 reply parses the selected-VFO byte as a subcommand");
    check(parsed && parsed->data.size() == 3 && parsed->data[0] == 0x01
              && parsed->data[1] == 0x01 && parsed->data[2] == 0x02,
          "and presents exactly mode, DATA and filter to the decoder");

    // 0x06 is untouched. It is still what an unrecognised radio gets, and a
    // builder that had started emitting 26 for it would send a command that
    // model may not implement.
    check(bytesAre(cmdSetMode(kIc705, CivMode::Usb, 1),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x06, 0x01, 0x01, 0xFD}),
          "the plain mode command is still 06, unchanged");

    const std::array<std::uint8_t, 3> on{0x01, 0x01, 0x02};
    const auto dOn = decodeVfoMode(on);
    check(dOn && dOn->mode == CivMode::Usb && dOn->dataMode && dOn->filter == 2,
          "01 01 02 decodes as USB, DATA on, FIL2");

    const std::array<std::uint8_t, 3> off{0x01, 0x00, 0x03};
    const auto dOff = decodeVfoMode(off);
    check(dOff && dOff->mode == CivMode::Usb && !dOff->dataMode && dOff->filter == 3,
          "the same mode byte with DATA off decodes as plain USB");

    // The slot is the only thing an out-of-range byte may cost. Returning 0 —
    // "unspecified" — lets the caller keep the slot it already had rather than
    // dropping the operator's IF filter out of the 1..3 the ladder is defined
    // over, while the mode and DATA state are still adopted.
    const std::array<std::uint8_t, 3> badSlot{0x03, 0x00, 0x09};
    const auto dBad = decodeVfoMode(badSlot);
    check(dBad && dBad->mode == CivMode::Cw && dBad->filter == 0,
          "a slot outside 1..3 decodes as unspecified, not as slot 0");

    check(!decodeVfoMode({}), "an empty payload decodes to nothing");
    const std::array<std::uint8_t, 2> short2{0x01, 0x01};
    check(!decodeVfoMode(short2), "a payload short of all three fields is refused");
    const std::array<std::uint8_t, 4> long4{0x00, 0x01, 0x01, 0x02};
    check(!decodeVfoMode(long4),
          "a payload longer than the three documented fields is refused as mis-parsed");
    const std::array<std::uint8_t, 3> bogus{0x01, 0x07, 0x02};
    check(!decodeVfoMode(bogus),
          "a DATA byte that is neither 00 nor 01 is refused rather than coerced "
          "to a bool — an invented DATA state is the lie this command removes");
}

// commandHasSubcommand() is the ONE list of which commands are sub-addressed.
// parseFrame() decodes by it and the CI-V trace labels by it, so the property
// that matters is that the predicate and the parser cannot disagree — a drift
// between them is what produces a confidently mislabelled frame.
//
// Asserting the predicate against a hand-written list would just be a third
// copy. So this drives every one of the 256 command values through parseFrame()
// and checks the parser's own hasSub against the predicate.
static void testSubcommandPredicate()
{
    int subAddressed = 0;
    for (int c = 0; c <= 0xFF; ++c) {
        const auto command = static_cast<std::uint8_t>(c);
        // Two payload bytes, so there is something for either reading to claim:
        // if the command is sub-addressed the first is the subcommand, and if
        // it is not, both are data.
        const std::vector<std::uint8_t> wire{
            0xFE, 0xFE, kControllerAddress, kIc705, command, 0x01, 0x02, kCivEom};
        const auto parsed = parseFrame(wire);
        if (!parsed)
            continue;
        const bool predicate = commandHasSubcommand(command);
        check(parsed->hasSub == predicate,
              "parseFrame and commandHasSubcommand agree for every command byte");
        if (predicate) {
            ++subAddressed;
            check(parsed->sub == 0x01 && parsed->data.size() == 1,
                  "a sub-addressed command takes the subcommand out of the payload");
        } else {
            check(parsed->data.size() == 2,
                  "a bare command keeps both payload bytes");
        }
    }
    // The twelve that carry subcommands: 12 14 15 16 18 19 1A 1B 1C 21 26 27. A
    // change to the list is a deliberate protocol decision, so it should have
    // to come past this number rather than arrive as a silent side effect.
    check(subAddressed == 12, "exactly twelve CI-V commands are sub-addressed");
}

// ---------------------------------------------------------------------------
// IF filter width (1A 03), Twin PBT, and the TX passband decomposition
// ---------------------------------------------------------------------------

static void testFilterWidth()
{
    // THE CODE TABLES, from the IC-705 and IC-7300MK2 guides (identical).
    check(filterWidthHzFromCode("USB", 0) == 50, "SSB code 0 is 50 Hz");
    check(filterWidthHzFromCode("USB", 9) == 500, "SSB code 9 is 500 Hz");
    // THE OFF-BY-ONE wfview has: its decoder branches on `code <= 10` and
    // returns 550 Hz here. Code 10 is the first entry of the 100 Hz table.
    check(filterWidthHzFromCode("USB", 10) == 600, "SSB code 10 is 600 Hz, not 550");
    check(filterWidthHzFromCode("USB", 40) == 3600, "SSB tops out at 3.6 kHz");
    check(!filterWidthHzFromCode("USB", 41), "SSB code 41 is out of range");

    check(filterWidthHzFromCode("CW", 40) == 3600, "CW shares the SSB table");
    check(filterWidthHzFromCode("RTTY", 31) == 2700, "RTTY tops out at 2.7 kHz");
    check(!filterWidthHzFromCode("RTTY", 32), "RTTY has no code 32");
    check(filterWidthHzFromCode("AM", 0) == 200, "AM code 0 is 200 Hz");
    check(filterWidthHzFromCode("AM", 49) == 10000, "AM tops out at 10 kHz");
    check(!filterWidthHzFromCode("AM", 50), "AM has no code 50");

    // NO SETTABLE WIDTH in FM and friends — three fixed slots and no command.
    check(!filterWidthHzFromCode("FM", 10), "FM has no width code table");
    check(!filterWidthHzFromCode("WFM", 0), "WFM has no width code table");
    check(!filterWidthCodeFor("FM", 10000), "FM refuses a width request");
    check(filterWidthLimitsFor("FM").maxHz == 0, "FM advertises no width range");
    check(filterWidthLimitsFor("USB").maxHz == 3600, "SSB range tops at 3.6 kHz");
    check(filterWidthLimitsFor("AM").maxHz == 10000, "AM range tops at 10 kHz");
    check(filterWidthLimitsFor("RTTY").maxHz == 2700, "RTTY range tops at 2.7 kHz");

    // ROUND TRIP: every code the table defines must survive Hz and come back.
    for (int code = 0; code <= 49; ++code) {
        for (const char* mode : {"USB", "CW", "RTTY", "AM"}) {
            const auto hz = filterWidthHzFromCode(mode, static_cast<std::uint8_t>(code));
            if (!hz)
                continue;
            const auto back = filterWidthCodeFor(mode, *hz);
            check(back && *back == code, "width code survives a round trip");
        }
    }

    // THE GAP. 550 Hz is not a width this radio has; a request there must land
    // on a real neighbour rather than encode something the table cannot decode.
    const auto gap = filterWidthCodeFor("USB", 550);
    check(gap.has_value(), "a width in the 500-600 gap still encodes");
    const auto gapHz = filterWidthHzFromCode("USB", *gap);
    check(gapHz && (*gapHz == 500 || *gapHz == 600), "and lands on a real width");

    // Requests past the ends CLAMP to the ends rather than wrapping.
    const auto tooWide = filterWidthCodeFor("USB", 99999);
    check(tooWide && filterWidthHzFromCode("USB", *tooWide) == 3600, "over-wide clamps to 3.6 kHz");
    const auto rttyWide = filterWidthCodeFor("RTTY", 3600);
    check(rttyWide && filterWidthHzFromCode("RTTY", *rttyWide) == 2700,
          "RTTY clamps to its own 2.7 kHz ceiling, not SSB's");

    // ON THE WIRE the code is BCD, so code 40 is 0x40 and not 0x28. Sending the
    // binary value reaches a different, valid width and the radio accepts it.
    check(bytesAre(cmdSetFilterWidth(kIc705, 40),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1A, 0x03, 0x40, 0xFD}),
          "width code 40 goes out as BCD 0x40");
    check(bytesAre(cmdSetFilterWidth(kIc705, 49),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1A, 0x03, 0x49, 0xFD}),
          "width code 49 goes out as BCD 0x49");
    check(bytesAre(cmdReadFilterWidth(kIc705),
                   {0xFE, 0xFE, 0xA4, 0xE0, 0x1A, 0x03, 0xFD}),
          "the width read carries no payload");
}

static void testTwinPbt()
{
    // Centre is centre, whatever the width.
    check(pbtShiftHz(kPbtCentreCode, 2400) == 0, "code 128 is no shift");
    check(pbtCodeForShiftHz(0, 2400) == kPbtCentreCode, "no shift is code 128");

    // SCALED BY THE WIDTH: full deflection is one whole filter width, so the
    // same code means very different Hz in a wide and a narrow filter.
    check(pbtShiftHz(255, 2400) == 2400, "full clockwise is one width up");
    check(pbtShiftHz(255, 250) == 250, "the same code is 250 Hz in a narrow filter");
    check(pbtShiftHz(1, 2400) == -2400, "full anticlockwise is one width down");

    // A zero width must not divide by itself.
    check(pbtShiftHz(200, 0) == 0, "no width means no shift");
    check(pbtCodeForShiftHz(500, 0) == kPbtCentreCode, "no width means centre");

    // Codes clamp rather than wrapping through the far end of the passband.
    check(pbtCodeForShiftHz(99999, 2400) == 255, "an over-large shift clamps high");
    check(pbtCodeForShiftHz(-99999, 2400) == 0, "an over-large negative clamps low");

    // TOGETHER SLIDES. Both PBTs at the same code move the window and keep it
    // the same width — which is the decomposition setSliceFilter relies on.
    const int code = pbtCodeForShiftHz(300, 2400);
    const auto slid = passbandFromWidthAndPbt(1500, 2400, code, code);
    check(slid.highHz - slid.lowHz == 2400, "a matched pair keeps the width");
    check(std::abs((slid.lowHz + slid.highHz) / 2 - 1800) <= 20,
          "a matched pair moves the centre by the shift");

    // APART NARROWS, from the inside, which is the only asymmetric response an
    // Icom can produce and the reason width alone cannot describe one.
    const auto narrowed = passbandFromWidthAndPbt(1500, 2400,
                                                  pbtCodeForShiftHz(-300, 2400),
                                                  pbtCodeForShiftHz(300, 2400));
    check(narrowed.highHz - narrowed.lowHz < 2400, "a split pair narrows the passband");
    check(std::abs((narrowed.lowHz + narrowed.highHz) / 2 - 1500) <= 20,
          "and a symmetric split leaves the centre alone");

    // Centred, unshifted, an SSB passband sits where the radio puts it: around
    // 1500 Hz, NOT with its low edge pinned at 300.
    const auto ssb = passbandFromWidthAndPbt(passbandCentreHz("USB", 2400), 2400,
                                             kPbtCentreCode, kPbtCentreCode);
    check(ssb.lowHz == 300 && ssb.highHz == 2700, "2.4 kHz USB is 300..2700");
    const auto narrowSsb = passbandFromWidthAndPbt(passbandCentreHz("USB", 1800), 1800,
                                                   kPbtCentreCode, kPbtCentreCode);
    check(narrowSsb.lowHz == 600 && narrowSsb.highHz == 2400,
          "1.8 kHz USB narrows SYMMETRICALLY to 600..2400, not 300..2100");

    // LSB mirrors, sign carrying the sideband.
    const auto lsb = passbandFromWidthAndPbt(passbandCentreHz("LSB", 2400), 2400,
                                             kPbtCentreCode, kPbtCentreCode);
    check(lsb.lowHz == -2700 && lsb.highHz == -300, "2.4 kHz LSB mirrors to -2700..-300");

    // CW is centred on the tone, and AetherSDR's CW slice frequency IS the tone.
    check(passbandCentreHz("CW", 500) == 0, "CW has no carrier offset");
    check(passbandCentreHz("AM", 6000) == 0, "AM straddles the carrier");
    check(passbandCentreHz("RTTY", 500) == -400,
          "RTTY keeps the conservative 150 Hz carrier-side edge until SET 0050 is read");
    const auto cw = passbandFromWidthAndPbt(0, 500, kPbtCentreCode, kPbtCentreCode);
    check(cw.lowHz == -250 && cw.highHz == 250, "500 Hz CW is +/-250 about the tone");
}

static void testNetworkSettingDecoding()
{
    const std::array<std::uint8_t, 8> address{
        0x01, 0x92, 0x01, 0x68, 0x00, 0x01, 0x00, 0x42};
    const auto decoded = decodeNetworkAddress(address);
    check(decoded && *decoded == std::array<std::uint8_t, 4>{192, 168, 1, 42},
          "SET-menu BCD network address decodes four decimal octets");
    const auto mask = subnetMaskFromBcdPrefix(0x24);
    check(mask && *mask == std::array<std::uint8_t, 4>{255, 255, 255, 0},
          "SET-menu /24 prefix decodes to 255.255.255.0");

    const std::array<std::uint8_t, 8> outOfRange{
        0x02, 0x99, 0x01, 0x68, 0x00, 0x01, 0x00, 0x42};
    const std::array<std::uint8_t, 8> malformedBcd{
        0x01, 0x9a, 0x01, 0x68, 0x00, 0x01, 0x00, 0x42};
    check(!decodeNetworkAddress(outOfRange),
          "SET-menu network octets above 255 are rejected");
    check(!decodeNetworkAddress(malformedBcd),
          "SET-menu network values with non-BCD nibbles are rejected");
    check(!subnetMaskFromBcdPrefix(0x00) && !subnetMaskFromBcdPrefix(0x31),
          "SET-menu subnet prefixes outside the documented /1-/30 range are rejected");
    const std::array<std::uint8_t, 9> networkName{
        'S', 'h', 'a', 'c', 'k', 'R', 'a', 'd', 'i'};
    const auto decodedName = decodeNetworkName(networkName);
    check(decodedName && *decodedName == "ShackRadi",
          "SET-menu Network Name decodes bounded printable ASCII");
    const std::array<std::uint8_t, 2> invalidName{'A', 0x1f};
    check(!decodeNetworkName(invalidName),
          "SET-menu Network Name rejects control characters");
}

int main()
{
    testFilterWidth();
    testTwinPbt();
    testFraming();
    testBcd();
    testFmRepeaterCommands();
    testReassembler();
    testModes();
    testCommands();
    testDataMode();
    testSubcommandPredicate();
    testNetworkSettingDecoding();

    if (g_failures == 0)
        std::printf("icom_civ_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
