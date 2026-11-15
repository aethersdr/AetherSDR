#include "core/AcomProtocol.h"

#include <QByteArray>
#include <QList>

#include <cstdio>

using namespace AetherSDR::Acom;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

void setLe16(QByteArray& buf, int offset, quint16 value)
{
    buf[offset] = static_cast<char>(value & 0xFF);
    buf[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

}  // namespace

int main()
{
    // ── Checksum / framing, verified against literal examples from the
    //    manufacturer's own spec (§3.1, §5.6, §5.7, §5.8, §5.9) — not derived
    //    from our own formula, so these catch a wrong-sign or off-by-one bug.
    report("TelemetryEnable (0x92) matches spec's literal checksum 0x15",
           buildTelemetryEnable() == QByteArray::fromHex("55920415"));
    report("TelemetryDisable (0x91) matches spec's literal checksum 0x16",
           buildTelemetryDisable() == QByteArray::fromHex("55910416"));
    report("obsolete ASCII-switch frame (0x90) matches spec's literal checksum 0x17",
           buildFrame(0x90) == QByteArray::fromHex("55900417"));
    report("bootloader-activation frame (0x93) matches spec's literal checksum 0x14",
           buildFrame(0x93) == QByteArray::fromHex("55930414"));
    report("'reply to wrong message' frame (0x01) matches spec's literal checksum 0xA6",
           buildFrame(0x01) == QByteArray::fromHex("550104a6"));

    // ── FrameParser: round-trips a built command frame back to itself.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        parser.feed(buildModeCommand(ModeCommand::Operate));
        report("mode-change command round-trips through the parser",
               received.size() == 1
                   && received.at(0).address == static_cast<quint8>(Address::Command)
                   && received.at(0).data.size() == 4
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x02
                   && static_cast<quint8>(received.at(0).data.at(2))
                          == static_cast<quint8>(ModeCommand::Operate));
    }

    // ── FrameParser: self-heals past noise and a corrupted candidate frame.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        QByteArray stream;
        stream.append(QByteArray::fromHex("0000FF"));       // leading noise, no start byte
        stream.append(QByteArray::fromHex("550104FF"));     // 0x55 start, but wrong checksum
        stream.append(buildClearFaultsCommand());           // a genuine valid frame
        parser.feed(stream);

        report("parser ignores noise and a bad-checksum candidate, still finds the real frame",
               received.size() == 1
                   && received.at(0).address == static_cast<quint8>(Address::Command)
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x08);
    }

    // ── FrameParser: an implausible length byte (e.g. a bit-flip) is
    //    discarded as noise immediately, rather than buffered forever
    //    waiting for a frame that large to arrive — which would silently
    //    absorb every subsequent real frame into the dead buffer.
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        QByteArray stream;
        QByteArray bogus = QByteArray::fromHex("55FF00");  // 0x55, addr, length=255
        bogus.append(QByteArray(250, '\0'));               // plausible-looking trailing bytes
        stream.append(bogus);
        stream.append(buildClearFaultsCommand());          // a genuine valid frame

        parser.feed(stream);
        report("length byte over the 72-byte spec max is rejected as noise, not buffered",
               received.size() == 1
                   && received.at(0).address == static_cast<quint8>(Address::Command)
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x08);
    }

    // ── FrameParser: a false 0x55 followed by a long non-0x55 garbage span
    //    resyncs to the trailing valid frame in one shift (the linear-resync
    //    path — the whole span is dropped at once rather than a byte at a time).
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        QByteArray stream;
        stream.append(QByteArray::fromHex("5500FF"));      // 0x55, then length 0xFF = invalid
        stream.append(QByteArray(300, '\0'));              // long non-0x55 garbage span
        stream.append(buildClearFaultsCommand());          // a genuine valid frame
        parser.feed(stream);

        report("a false start byte + long garbage span resyncs to the trailing frame",
               received.size() == 1
                   && received.at(0).address == static_cast<quint8>(Address::Command)
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x08);
    }

    // ── FrameParser: split across two feed() calls (mid-frame boundary).
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });

        const QByteArray frame = buildModeCommand(ModeCommand::Standby);
        parser.feed(frame.left(3));   // start+address+length only
        report("no frame emitted until the full length has arrived", received.isEmpty());
        parser.feed(frame.mid(3));    // rest of the frame
        report("frame split across feed() calls still decodes",
               received.size() == 1
                   && static_cast<quint8>(received.at(0).data.at(2))
                          == static_cast<quint8>(ModeCommand::Standby));
    }

    // ── Telemetry decode (0x2F payload) — synthetic frame with known values
    //    at every offset the decoder reads, built independently of the
    //    decoder's own offset math (hand-placed, not looped).
    {
        QByteArray payload(68, '\0');
        payload[0] = static_cast<char>(0x70);  // mode nibble = OperateTx (0x7)
        setLe16(payload, 5, 1234);             // dcPowerPam1_x10W
        setLe16(payload, 9, 1);                // system clock high word
        setLe16(payload, 11, 5000);            // system clock low word -> 70536 total
        setLe16(payload, 13, 305);             // paTempRaw
        setLe16(payload, 17, 50);              // inputPower_x10W
        setLe16(payload, 19, 450);             // forwardPowerW
        setLe16(payload, 21, 8);               // reflectedPowerW
        setLe16(payload, 23, 130);             // swr_x100 (1.30)
        setLe16(payload, 25, 200);             // dissipationPam1_x10W
        setLe16(payload, 33, 5000);            // vcc5_mV
        setLe16(payload, 35, 261);             // vcc26_x10V (26.1 V)
        setLe16(payload, 37, 502);             // hv1_x10V (50.2 V)
        setLe16(payload, 41, 9400);            // id1_mA (9.4 A)
        setLe16(payload, 45, 14245);           // carrierFreqKHz
        payload[63] = static_cast<char>(0xFF); // errorCode: no fault
        setLe16(payload, 64, 0);               // errorParam
        payload[66] = static_cast<char>((2 << 4) | 5);  // fanSpeed=2, activeBand=5 (20m)

        const QByteArray fullFrame = buildFrame(static_cast<quint8>(Address::Telemetry), payload);
        report("hand-built telemetry frame is exactly 72 bytes", fullFrame.size() == 72);

        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });
        parser.feed(fullFrame);
        report("telemetry frame parses with a valid checksum", received.size() == 1);

        const auto decoded = decodeTelemetry(received.at(0).data);
        report("decodeTelemetry succeeds on a full-length payload", decoded.has_value());
        if (decoded) {
            report("mode decodes to OperateTx", decoded->mode == Mode::OperateTx);
            report("dcPowerPam1_x10W round-trips", decoded->dcPowerPam1_x10W == 1234);
            report("systemClockSec combines high+low words correctly",
                   decoded->systemClockSec == 70536);
            report("paTempRaw round-trips", decoded->paTempRaw == 305);
            report("forwardPowerW round-trips", decoded->forwardPowerW == 450);
            report("reflectedPowerW round-trips", decoded->reflectedPowerW == 8);
            report("swr_x100 round-trips", decoded->swr_x100 == 130);
            report("vcc26_x10V round-trips", decoded->vcc26_x10V == 261);
            report("hv1_x10V round-trips", decoded->hv1_x10V == 502);
            report("id1_mA round-trips", decoded->id1_mA == 9400);
            report("carrierFreqKHz round-trips", decoded->carrierFreqKHz == 14245);
            report("errorCode round-trips (0xFF = no fault)", decoded->errorCode == 0xFF);
            report("fanSpeed extracted from high nibble", decoded->fanSpeed == 2);
            report("activeBand extracted from low nibble", decoded->activeBand == 5);
        }
        report("short payload is rejected rather than reading out of bounds",
               !decodeTelemetry(payload.left(67)).has_value());
    }

    // ── Lookup tables.
    report("bandName(5) is 20m per the spec's LPF channel table", bandName(5) == "20m");
    report("bandName(0) is the reserved placeholder", bandName(0) == "?m");
    report("bandName rejects an out-of-range index safely", bandName(99) == "?m");
    report("errorCodeName(0xFF) reads as no fault", errorCodeName(0xFF) == "No fault");
    report("errorCodeName(0x39) matches the PAM-overcurrent group",
           errorCodeName(0x39) == "Excessive PAM current");

    // ── Per-model scaling table (design doc §6). 600S is hardware-confirmed;
    //    the rest are derived from ACOM's own published rated-output specs,
    //    not the model name and not the (wrong, for the big models) public
    //    reference client constants — see the modelTable() comment.
    const auto& s600 = modelSpec("600S");
    report("600S nominal/max forward power matches the design doc table",
           s600.nominalForwardW == 600.0f && s600.maxForwardW == 700.0f);
    report("600S has no PAM2", !s600.hasPam2);
    report("500S max is a round number derived from the 600S ratio, not the model name",
           modelSpec("500S").nominalForwardW == 500.0f && modelSpec("500S").maxForwardW == 600.0f);
    report("700S max is a round number derived from the 600S ratio, not the model name",
           modelSpec("700S").nominalForwardW == 700.0f && modelSpec("700S").maxForwardW == 800.0f);
    report("1200S is rated 1000W (NOT 1200W — the model name is not the rated wattage), "
           "max uses the name itself (1200W) since that fits the +200W pattern",
           modelSpec("1200S").nominalForwardW == 1000.0f && modelSpec("1200S").maxForwardW == 1200.0f);
    report("1400S is rated 1200W, max 1400W (name fits the +200W pattern)",
           modelSpec("1400S").nominalForwardW == 1200.0f && modelSpec("1400S").maxForwardW == 1400.0f);
    report("2020S is rated 1500W (name doesn't fit any wattage pattern), "
           "max falls back to the ratio-derived value, not '2020'",
           modelSpec("2020S").nominalForwardW == 1500.0f && modelSpec("2020S").maxForwardW == 1750.0f);
    const auto& s1200 = modelSpec("1200S");
    report("1200S has PAM2", s1200.hasPam2);
    report("1400S has PAM2 (inferred from power class, like 1200S/2020S)",
           modelSpec("1400S").hasPam2);
    report("unknown model name falls back to 600S rather than crashing",
           modelSpec("NoSuchModel").name == "600S");
    report("modelNames() lists all six S-series entries (500/600/700/1200/1400/2020)",
           modelNames().size() == 6);

    // ── Model tier ordering + auto-ranging (design doc §6).
    const QStringList tiers = orderedModelTiers();
    report("tier order is ascending by power: 500S,600S,700S,1200S,1400S,2020S",
           tiers == QStringList({"500S", "600S", "700S", "1200S", "1400S", "2020S"}));
    report("a reading that exceeds 500S's ceiling but fits 600S's resolves to 600S",
           tierForForwardPower(650.0f) == "600S");
    report("a reading that fits 500S still returns 500S, not something smaller nonexistent",
           tierForForwardPower(400.0f) == "500S");
    report("a reading between 600S and 700S ceilings picks 700S",
           tierForForwardPower(750.0f) == "700S");
    report("a reading that only a 1200S-tier amp could produce picks 1200S, "
           "skipping 700S directly rather than requiring incremental steps",
           tierForForwardPower(1100.0f) == "1200S");
    report("a reading beyond every tier's max clamps to the top tier (2020S) rather than erroring",
           tierForForwardPower(5000.0f) == "2020S");

    // ── SystemConfig (address 0x11) — request framing + decode.
    report("SystemConfig request (0x02, asking for 0x11) has the spec's 5-byte shape",
           buildRequestMessage(0x11).size() == 5);
    {
        QList<Frame> received;
        FrameParser parser;
        parser.setFrameCallback([&](const Frame& f) { received.append(f); });
        parser.feed(buildRequestMessage(0x11));
        report("SystemConfig request round-trips through the parser",
               received.size() == 1
                   && received.at(0).address == 0x02
                   && static_cast<quint8>(received.at(0).data.at(0)) == 0x11);
    }
    {
        // Hand-built SystemConfig payload (26 bytes) — amplifierType=1 (the
        // spec's only documented value), a fabricated fw/serial/fault-count
        // for round-trip verification, not real hardware data.
        QByteArray payload(26, '\0');
        payload[0] = static_cast<char>(0x01);  // amplifierType = 1 (A600S)
        payload[1] = static_cast<char>(0x02);  // fwVersion
        payload[2] = static_cast<char>(0x09);  // fwSubVersion
        for (int i = 0; i < 12; ++i)
            payload[13 + i] = static_cast<char>(0xA0 + i);  // serial number bytes
        payload[25] = static_cast<char>(0x03);  // hardFaultCount

        const auto cfg = decodeSystemConfig(payload);
        report("decodeSystemConfig succeeds on a full-length payload", cfg.has_value());
        if (cfg) {
            report("amplifierType round-trips", cfg->amplifierType == 1);
            report("fwVersion/fwSubVersion round-trip", cfg->fwVersion == 2 && cfg->fwSubVersion == 9);
            report("serial number decodes as a 24-char hex dump of the 12 raw bytes",
                   cfg->serialNumberHex == QStringLiteral("a0a1a2a3a4a5a6a7a8a9aaab"));
            report("hardFaultCount round-trips", cfg->hardFaultCount == 3);
        }
        report("short SystemConfig payload is rejected rather than reading out of bounds",
               !decodeSystemConfig(payload.left(25)).has_value());
    }
    report("modelNameForAmplifierType(1) confidently resolves to 600S — the only "
           "value the spec actually documents",
           modelNameForAmplifierType(1) == "600S");
    report("modelNameForAmplifierType(anything else) returns empty — a genuine "
           "unknown, not a guess (design doc §6)",
           modelNameForAmplifierType(2).isEmpty() && modelNameForAmplifierType(0).isEmpty());

    std::printf("\n%d ACOM protocol test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
