#include "core/SpeProtocol.h"

#include <QByteArray>
#include <QList>

#include <cstdio>

using namespace AetherSDR::Spe;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// Wraps a Status payload the way the amplifier does (spec §5): 0xAA x3,
// CNT, data, 16-bit checksum (low then high), CR LF. Built independently
// of FrameParser's own math so the two check each other.
QByteArray buildAmpStatusFrame(const QByteArray& payload)
{
    quint16 sum = 0;
    for (char c : payload)
        sum = static_cast<quint16>(sum + static_cast<quint8>(c));

    QByteArray frame;
    frame.append(3, static_cast<char>(0xAA));
    frame.append(static_cast<char>(payload.size()));
    frame.append(payload);
    frame.append(static_cast<char>(sum & 0xFF));
    frame.append(static_cast<char>((sum >> 8) & 0xFF));
    frame.append('\r');
    frame.append('\n');
    return frame;
}

// The spec's own literal Status example (§5, verbatim): 67 characters, 19
// fields behind a leading marker character.
const char* kSpecExample =
    "C,20K,S,R,x,1,00,1a,0r,L,0000, 0.00, 0.00, 0.0, 0.0, 33,  0,  0,N,N";

}  // namespace

int main()
{
    // ── Host->amp framing, verified against the spec's literal OPERATE
    //    example (§3: 0x55 0x55 0x55 0x01 0x0D 0x0D) and the Status request
    //    (§5: 0x55 0x55 0x55 0x01 0x90 0x90) — checksum of a single byte is
    //    the byte itself. The trailing CR LF is the documented real-hardware
    //    requirement (see SpeProtocol.h).
    report("OPERATE keystroke matches the spec's literal byte sequence + CR LF",
           buildKeyCommand(Key::Operate) == QByteArray::fromHex("555555010d0d0d0a"));
    report("Status request matches the spec's literal byte sequence + CR LF",
           buildStatusRequest() == QByteArray::fromHex("5555550190900d0a"));
    report("SWITCH OFF keystroke frames 0x0A with itself as checksum",
           buildKeyCommand(Key::SwitchOff) == QByteArray::fromHex("555555010a0a0d0a"));
    report("backlight-on frames 0x82 with itself as checksum",
           buildBacklightCommand(true) == QByteArray::fromHex("5555550182820d0a"));
    report("backlight-off frames 0x83 with itself as checksum",
           buildBacklightCommand(false) == QByteArray::fromHex("5555550183830d0a"));

    // ── FrameParser: the spec's ACK reply (§3: 0xAA x3, CNT=1, echoed
    //    command, 1-byte checksum).
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        parser.feed(QByteArray::fromHex("aaaaaa010d0d"));
        report("ACK reply decodes with its 1-byte checksum",
               received.size() == 1 && received.at(0).isAck()
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x0D);
    }

    // ── FrameParser: the spec's own 67-character Status example round-trips
    //    through the 16-bit-checksum path.
    {
        const QByteArray payload(kSpecExample);
        report("the spec's literal Status example is exactly 67 characters",
               payload.size() == kStatusDataLength);

        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });
        parser.feed(buildAmpStatusFrame(payload));
        report("Status frame decodes with its 2-byte checksum",
               received.size() == 1 && !received.at(0).isAck()
                   && received.at(0).data == payload);
    }

    // ── FrameParser: self-heals past noise, a lone 0xAA inside garbage, and
    //    a corrupted candidate frame.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        QByteArray stream;
        stream.append(QByteArray::fromHex("0000aa41ff"));   // noise incl. a lone 0xAA
        stream.append(QByteArray::fromHex("aaaaaa010d0e")); // full sync, but wrong checksum
        stream.append(QByteArray::fromHex("aaaaaa010909")); // genuine ACK (TUNE echo)
        parser.feed(stream);

        report("parser skips noise and a bad-checksum candidate, still finds the real frame",
               received.size() == 1
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x09);
    }

    // ── FrameParser: an implausible CNT byte is discarded as noise
    //    immediately rather than buffered forever waiting for a frame that
    //    large — which would silently absorb every subsequent real frame.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        QByteArray stream;
        stream.append(QByteArray::fromHex("aaaaaaff"));     // CNT=255 > spec max
        stream.append(QByteArray(100, '\0'));
        stream.append(QByteArray::fromHex("aaaaaa010d0d")); // genuine ACK
        parser.feed(stream);

        report("CNT over the spec ceiling is rejected as noise, not buffered",
               received.size() == 1
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x0D);
    }

    // ── FrameParser: split across feed() calls at a mid-frame boundary —
    //    exactly what a TCP segment boundary through ser2net produces.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        const QByteArray frame = buildAmpStatusFrame(QByteArray(kSpecExample));
        parser.feed(frame.left(10));
        report("no frame emitted until the full Status length has arrived",
               received.isEmpty());
        parser.feed(frame.mid(10));
        report("Status frame split across feed() calls still decodes",
               received.size() == 1 && received.at(0).data.size() == kStatusDataLength);
    }

    // ── Status decode: the spec's literal RX/standby example.
    {
        const auto s = parseStatus(QByteArray(kSpecExample));
        report("parseStatus succeeds on the spec's literal example", s.has_value());
        if (s) {
            report("ID decodes as 20K (2K-FA)", s->id == "20K");
            report("S decodes as STANDBY", !s->operate);
            report("R decodes as RECEIVE", !s->transmitting);
            report("2K-FA bank reads x", s->bank == u'x');
            report("input port decodes", s->input == 1);
            report("band 00 decodes as index 0 (160m)", s->bandIndex == 0);
            report("TX antenna digit splits from the ATU letter", s->txAntenna == 1);
            report("ATU state letter a = ATU enabled", s->atuState == u'a');
            report("RX antenna reads 0r when none is set", s->rxAntenna == "0r");
            report("power level L decodes", s->powerLevel == u'L');
            report("output power is 0 in RX", s->outputPowerW == 0.0f);
            report("heatsink temperature decodes", s->tempUpper == 33);
            report("no warnings", s->warning == u'N');
            report("no alarms", s->alarm == u'N');
        }
    }

    // ── Status decode: a transmitting 1.5K-FA with warning + fractional
    //    values in every numeric field the RX example leaves at zero.
    {
        const QByteArray payload(
            "C,15K,O,T,A,2,05,2b,0r,H,1350, 1.10, 1.25, 47.5, 32.0, 45, 40, 38,S,N");
        const auto s = parseStatus(payload);
        report("parseStatus succeeds on a TX-state string", s.has_value());
        if (s) {
            report("ID decodes as 15K (1.5K-FA)", s->id == "15K");
            report("O decodes as OPERATE", s->operate);
            report("T decodes as TRANSMIT", s->transmitting);
            report("memory bank A decodes", s->bank == u'A');
            report("input port 2 decodes", s->input == 2);
            report("band 05 decodes as index 5 (20m)", s->bandIndex == 5);
            report("ATU state letter b = bypassed", s->atuState == u'b');
            report("power level H decodes", s->powerLevel == u'H');
            report("output power decodes", s->outputPowerW == 1350.0f);
            report("SWR ATU decodes", s->swrAtu == 1.10f);
            report("SWR ANT decodes", s->swrAnt == 1.25f);
            report("PA voltage decodes", s->paVoltageV == 47.5f);
            report("PA current decodes", s->paCurrentA == 32.0f);
            report("all three temperatures decode",
                   s->tempUpper == 45 && s->tempLower == 40 && s->tempCombiner == 38);
            report("warning S decodes", s->warning == u'S');
        }
    }

    // ── Status decode: tolerates the marker character being absent (19
    //    tokens instead of 20) and rejects a short field count.
    report("a marker-less 19-field string still parses",
           parseStatus(QByteArray(
               "20K,S,R,x,1,00,1a,0r,L,0000, 0.00, 0.00, 0.0, 0.0, 33,  0,  0,N,N"))
               .has_value());
    report("a truncated string is rejected rather than misindexed",
           !parseStatus(QByteArray("C,20K,S,R,x,1,00")).has_value());

    // ── Lookup tables.
    report("bandName(0) is 160m per the spec's band table", bandName(0) == "160m");
    report("bandName(11) is 4m per the spec's band table", bandName(11) == "4m");
    report("bandName rejects an out-of-range index safely", bandName(12) == "?m");
    report("warningText('N') is empty so banners can hide", warningText(u'N').isEmpty());
    report("warningText('P') matches the spec's warnings table",
           warningText(u'P') == "Power limit exceeded");
    report("warningText echoes an unknown code rather than guessing",
           warningText(u'Z').contains(u'Z'));
    report("alarmText('N') is empty so banners can hide", alarmText(u'N').isEmpty());
    report("alarmText('D') matches the spec's alarms table",
           alarmText(u'D') == "Input overdriving");
    report("powerLevelName expands L/M/H",
           powerLevelName(u'L') == "LOW" && powerLevelName(u'M') == "MID"
               && powerLevelName(u'H') == "HIGH");

    // ── Per-model display scaling.
    const auto& s15 = modelSpec("15K");
    report("1.5K-FA thresholds match the hardware-validated values "
           "(nominal 1500, warn 1450, max 1600)",
           s15.nominalPowerW == 1500.0f && s15.warnPowerW == 1450.0f
               && s15.maxPowerW == 1600.0f);
    report("1.5K-FA has memory banks, no combiner",
           s15.hasMemoryBanks && !s15.hasCombiner);
    report("2K-FA has a combiner, no memory banks",
           modelSpec("20K").hasCombiner && !modelSpec("20K").hasMemoryBanks);
    report("1.3K-FA display name resolves",
           modelSpec("13K").displayName == "1.3K-FA");
    report("an unknown ID falls back to the hardware-validated 1.5K-FA entry",
           modelSpec("99K").displayName == "1.5K-FA");
    report("modelIds() lists all three documented models", modelIds().size() == 3);
    report("per-level nominals: 1.5K-FA LOW/MID/HIGH = 500/1000/1500 "
           "(hardware-validated — the reference app's bar rescaled to these)",
           levelNominalW(s15, u'L') == 500.0f && levelNominalW(s15, u'M') == 1000.0f
               && levelNominalW(s15, u'H') == 1500.0f);
    report("an unknown level letter falls back to the full HIGH scale",
           levelNominalW(s15, u'?') == 1500.0f);

    // The GUI wiring takes the whole axis from levelGaugeRange(), so the model
    // table is the only place the HIGH thresholds live. Asserted against the
    // spec's own fields rather than against literals: that is what makes an
    // edit to modelTable() reach the bar instead of being silently overridden
    // by a duplicate derivation in MainWindow_Wiring (PR #4531 review).
    for (const QString& id : modelIds()) {
        const auto& spec = modelSpec(id);
        const auto high = levelGaugeRange(spec, u'H');
        report(qPrintable(QStringLiteral("%1: the HIGH gauge axis is the model row verbatim")
                              .arg(spec.displayName)),
               high.nominalW == spec.nominalPowerW && high.warnW == spec.warnPowerW
                   && high.maxW == spec.maxPowerW);
        // ...and the tabulated rows themselves follow the hardware-validated
        // shape the LOW/MID levels derive, so the family stays coherent.
        report(qPrintable(QStringLiteral("%1: the tabulated HIGH row follows nominal-50/+100")
                              .arg(spec.displayName)),
               spec.warnPowerW == spec.nominalPowerW - 50.0f
                   && spec.maxPowerW == spec.nominalPowerW + 100.0f);
    }
    {
        const auto low = levelGaugeRange(s15, u'L');
        const auto mid = levelGaugeRange(s15, u'M');
        report("1.5K-FA LOW axis is 450/500/600 (the reference app's own bar)",
               low.warnW == 450.0f && low.nominalW == 500.0f && low.maxW == 600.0f);
        report("1.5K-FA MID axis is 950/1000/1100",
               mid.warnW == 950.0f && mid.nominalW == 1000.0f && mid.maxW == 1100.0f);
        const auto unknown = levelGaugeRange(s15, u'?');
        report("an unknown level letter takes the full HIGH axis",
               unknown.nominalW == 1500.0f && unknown.warnW == 1450.0f
                   && unknown.maxW == 1600.0f);
    }

    // ── RFC 2217 power-ON framing (design note §4) — literal byte
    //    sequences per RFC 2217's COM-PORT-OPTION subnegotiation.
    report("WILL COM-PORT-OPTION is IAC WILL 0x2C",
           Rfc2217::buildWillComPortOption() == QByteArray::fromHex("fffb2c"));
    report("SET-CONTROL RTS-on frames as IAC SB 2C 05 0B IAC SE",
           Rfc2217::buildSetControl(Rfc2217::kRtsOn)
               == QByteArray::fromHex("fffa2c050bfff0"));
    report("SET-CONTROL DTR-off frames as IAC SB 2C 05 09 IAC SE",
           Rfc2217::buildSetControl(Rfc2217::kDtrOff)
               == QByteArray::fromHex("fffa2c0509fff0"));

    // ── COM-PORT-OPTION reply scan. powerOn() gates the pulse on this:
    //    without it a raw-mode proxy silently swallows every SET-CONTROL and
    //    the client still reports success (PR #4531 review, finding 2).
    report("IAC DO COM-PORT-OPTION reads as accepted",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("fffd2c"))
               == Rfc2217::OptionReply::Accepted);
    report("IAC DONT COM-PORT-OPTION reads as refused — a plain-telnet port",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("fffe2c"))
               == Rfc2217::OptionReply::Refused);
    report("a raw-mode proxy answers nothing, and nothing is what we report",
           Rfc2217::scanComPortOptionReply(QByteArray(kSpecExample))
               == Rfc2217::OptionReply::None);
    report("negotiation for an unrelated option is not mistaken for ours",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("fffd01fffd03"))
               == Rfc2217::OptionReply::None);
    report("the reply is found when embedded in surrounding traffic",
           Rfc2217::scanComPortOptionReply(
               QByteArray::fromHex("aaaaaa010d0d") + QByteArray::fromHex("fffd2c")
                   + QByteArray::fromHex("aaaaaa010909"))
               == Rfc2217::OptionReply::Accepted);
    report("a later reply wins over an earlier one in the same chunk",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("fffd2cfffe2c"))
               == Rfc2217::OptionReply::Refused);
    report("an escaped 0xFF pair is not decoded as a negotiation verb",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("ffff2c"))
               == Rfc2217::OptionReply::None);
    report("a truncated reply at the end of a chunk is not guessed at",
           Rfc2217::scanComPortOptionReply(QByteArray::fromHex("fffd"))
               == Rfc2217::OptionReply::None);

    std::printf("\n%d SPE protocol test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
