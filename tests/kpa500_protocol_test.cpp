#include "core/Kpa500Protocol.h"

#include <QByteArray>
#include <QList>
#include <QPair>

#include <cstdio>

using namespace AetherSDR::Kpa500;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

// Convenience: feed a string literal and collect (cmd, arg) pairs.
QList<QPair<QString, QString>> parse(const char* bytes)
{
    QList<QPair<QString, QString>> out;
    FrameParser parser;
    parser.setCallback([&](const QString& cmd, const QString& arg) {
        out.append({cmd, arg});
    });
    parser.feed(QByteArray(bytes));
    return out;
}

}  // namespace

int main()
{
    // ── Command builders — verified against KPA500 Programmer's Reference Rev. A2 ──

    report("buildOperate() emits ^OS1;",
           buildOperate() == QByteArrayLiteral("^OS1;"));
    report("buildStandby() emits ^OS0;",
           buildStandby() == QByteArrayLiteral("^OS0;"));
    report("buildClearFault() emits ^FLC; (§^FL: 'SET format: ^FLC;')",
           buildClearFault() == QByteArrayLiteral("^FLC;"));
    report("buildPowerOff() emits ^ON0; (§^ON: 'SET format: ^ON0; turns the KPA500 off')",
           buildPowerOff() == QByteArrayLiteral("^ON0;"));
    report("buildPowerOn() emits ^ON1; (§^ON, confirmed over serial by KPA500 Remote)",
           buildPowerOn() == QByteArrayLiteral("^ON1;"));
    report("buildNullCommand() emits bare ';'",
           buildNullCommand() == QByteArrayLiteral(";"));
    report("buildQuery(\"WS\") emits ^WS;",
           buildQuery("WS") == QByteArrayLiteral("^WS;"));
    report("buildQuery(\"RVM\") emits ^RVM; (3-letter cmd)",
           buildQuery("RVM") == QByteArrayLiteral("^RVM;"));

    // ── buildSetFanSpeed: qBound(0, n, 6) ────────────────────────────────────
    // Source: KPA500 Programmer's Reference Rev. A2, §^FC.
    report("buildSetFanSpeed(3) emits ^FC3;",
           buildSetFanSpeed(3) == QByteArrayLiteral("^FC3;"));
    report("buildSetFanSpeed(0) emits ^FC0; (minimum)",
           buildSetFanSpeed(0) == QByteArrayLiteral("^FC0;"));
    report("buildSetFanSpeed(6) emits ^FC6; (maximum)",
           buildSetFanSpeed(6) == QByteArrayLiteral("^FC6;"));
    report("buildSetFanSpeed(-1) clamps to ^FC0; (below minimum)",
           buildSetFanSpeed(-1) == QByteArrayLiteral("^FC0;"));
    report("buildSetFanSpeed(7) clamps to ^FC6; (above maximum)",
           buildSetFanSpeed(7) == QByteArrayLiteral("^FC6;"));

    // ── FrameParser: well-formed frames ──────────────────────────────────────

    {
        auto frames = parse("^OS1;");
        report("^OS1; → cmd=OS arg=1",
               frames.size() == 1
                   && frames[0].first == "OS"
                   && frames[0].second == "1");
    }
    {
        // ^WS — space-separated power + SWR (§^WS).
        auto frames = parse("^WS500 150;");
        report("^WS500 150; → cmd=WS arg='500 150'",
               frames.size() == 1
                   && frames[0].first == "WS"
                   && frames[0].second == "500 150");
    }
    {
        // ^VI — space-separated voltage + current (§^VI).
        auto frames = parse("^VI120 053;");
        report("^VI120 053; → cmd=VI arg='120 053'",
               frames.size() == 1
                   && frames[0].first == "VI"
                   && frames[0].second == "120 053");
    }
    {
        // Three-letter command token.
        auto frames = parse("^RVM01.04;");
        report("^RVM01.04; → cmd=RVM arg='01.04'",
               frames.size() == 1
                   && frames[0].first == "RVM"
                   && frames[0].second == "01.04");
    }
    {
        // Multiple frames in one feed.
        auto frames = parse("^OS1;^BN05;^TM045;");
        report("Three frames in one feed — all decoded",
               frames.size() == 3
                   && frames[0].first == "OS"
                   && frames[1].first == "BN"
                   && frames[2].first == "TM");
    }

    // ── FrameParser: frame split across two feeds ─────────────────────────────
    {
        QList<QPair<QString, QString>> frames;
        FrameParser parser;
        parser.setCallback([&](const QString& cmd, const QString& arg) {
            frames.append({cmd, arg});
        });
        parser.feed(QByteArray("^WS50"));
        report("Partial frame: no callback yet", frames.isEmpty());
        parser.feed(QByteArray("0 150;"));
        report("Partial frame completed on second feed → cmd=WS arg='500 150'",
               frames.size() == 1
                   && frames[0].first == "WS"
                   && frames[0].second == "500 150");
    }

    // ── FrameParser: resync on garbage ───────────────────────────────────────
    {
        // Garbage before '^' is discarded; valid frame still decodes.
        auto frames = parse("@#GARBAGE!^FL07;");
        report("Garbage before '^' discarded, valid frame decoded",
               frames.size() == 1
                   && frames[0].first == "FL"
                   && frames[0].second == "07");
    }
    {
        // Null-command echo (bare ';') is silently discarded.
        auto frames = parse(";^OS0;");
        report("Null-command echo ';' discarded before ^OS0;",
               frames.size() == 1 && frames[0].first == "OS");
    }

    // ── FrameParser: kMaxFrameBytes overflow cap ──────────────────────────────
    {
        // Feed more than kMaxFrameBytes bytes with no semicolon — the buffer
        // cap at the tail of feed() clears it rather than growing unboundedly.
        // A valid frame arriving immediately after must still decode.
        QList<QPair<QString, QString>> frames;
        FrameParser parser;
        parser.setCallback([&](const QString& cmd, const QString& arg) {
            frames.append({cmd, arg});
        });
        // kMaxFrameBytes == 64 (Kpa500Protocol.h); feed one byte over the cap.
        parser.feed(QByteArray(65, 'X'));
        report("kMaxFrameBytes+1 garbage bytes: no callback fired", frames.isEmpty());
        parser.feed(QByteArray("^OS1;"));
        report("kMaxFrameBytes cap: valid frame after overflow still decodes",
               frames.size() == 1 && frames[0].first == "OS");
    }

    // ── applyMessage: ^WS power + SWR ────────────────────────────────────────
    // Source: §^WS — ppp plain integer watts; sss implied decimal after 2nd
    // digit, so "015" → 1.5, "150" → 15.0.
    {
        Status s;
        report("^WS500 150; → 500 W, SWR 15.0",
               applyMessage("WS", "500 150", s)
                   && s.forwardPowerW && *s.forwardPowerW == 500.0f
                   && s.swr && *s.swr == 15.0f);
    }
    {
        // Vendor example: ^SW015 = 1.5:1 (adapted for ^WS format).
        Status s;
        report("^WS250 015; → 250 W, SWR 1.5",
               applyMessage("WS", "250 015", s)
                   && s.forwardPowerW && *s.forwardPowerW == 250.0f
                   && s.swr && *s.swr == 1.5f);
    }
    {
        // Not transmitting: both fields zero.
        Status s;
        report("^WS000 000; → 0 W, SWR 0.0 (not transmitting)",
               applyMessage("WS", "000 000", s)
                   && s.forwardPowerW && *s.forwardPowerW == 0.0f
                   && s.swr && *s.swr == 0.0f);
    }

    // ── applyMessage: ^VI voltage + current ──────────────────────────────────
    // Source: §^VI — implied decimal after 2nd digit for both fields:
    // "120" → 12.0 V, "053" → 5.3 A.
    {
        Status s;
        report("^VI120 053; → 12.0 V, 5.3 A",
               applyMessage("VI", "120 053", s)
                   && s.paVoltageV && *s.paVoltageV == 12.0f
                   && s.paCurrentA && *s.paCurrentA == 5.3f);
    }
    {
        Status s;
        report("^VI099 010; → 9.9 V, 1.0 A",
               applyMessage("VI", "099 010", s)
                   && s.paVoltageV && *s.paVoltageV == 9.9f
                   && s.paCurrentA && *s.paCurrentA == 1.0f);
    }

    // ── applyMessage: other telemetry ─────────────────────────────────────────
    {
        Status s;
        report("^TM045; → paTemperatureC 45 °C",
               applyMessage("TM", "045", s)
                   && s.paTemperatureC && *s.paTemperatureC == 45.0f);
    }
    {
        Status s;
        report("^TM150; → paTemperatureC 150 °C (spec max)",
               applyMessage("TM", "150", s)
                   && s.paTemperatureC && *s.paTemperatureC == 150.0f);
    }
    {
        // Out of range — field not set.
        Status s;
        report("^TM200; out of range → field unchanged",
               !applyMessage("TM", "200", s) && !s.paTemperatureC);
    }
    {
        Status s;
        report("^BN05; → band 5 (20m)",
               applyMessage("BN", "05", s)
                   && s.band && *s.band == 5);
    }
    {
        Status s;
        report("^BN10; → band 10 (6m, spec max)",
               applyMessage("BN", "10", s)
                   && s.band && *s.band == 10);
    }
    {
        Status s;
        report("^BN11; out of range → field unchanged",
               !applyMessage("BN", "11", s) && !s.band);
    }

    // ── applyMessage: ^FL fault code ─────────────────────────────────────────
    // Source: §^FL — "^FLnn; where nn = fault identifier; nn=00 = no faults."
    // Decimal (NOT hex — unlike KPA1500).
    {
        Status s;
        report("^FL00; → faultCode 0 (no fault)",
               applyMessage("FL", "00", s)
                   && s.faultCode && *s.faultCode == 0);
    }
    {
        Status s;
        report("^FL07; → faultCode 7 (decimal fault)",
               applyMessage("FL", "07", s)
                   && s.faultCode && *s.faultCode == 7);
    }
    {
        // Decimal 99 is in range.
        Status s;
        report("^FL99; → faultCode 99 (decimal max)",
               applyMessage("FL", "99", s)
                   && s.faultCode && *s.faultCode == 99);
    }
    {
        // The KPA1500 uses hex fault codes; the KPA500 does not. "B0" is not a
        // valid decimal integer — toInt() sets ok=false, the guard rejects it,
        // and faultCode stays unset. Correct: the KPA500 spec only defines
        // decimal codes (00–99), so a hex-formatted value is a malformed reply.
        Status s;
        report("^FL hex-notation 'B0' — toInt decimal fails, field unchanged",
               !applyMessage("FL", "B0", s) && !s.faultCode);
    }

    // ── applyMessage: ^OS operate/standby ────────────────────────────────────
    {
        Status s;
        report("^OS1; → operate=true",
               applyMessage("OS", "1", s)
                   && s.operate && *s.operate == true);
    }
    {
        Status s;
        report("^OS0; → operate=false",
               applyMessage("OS", "0", s)
                   && s.operate && *s.operate == false);
    }
    {
        Status s;
        report("^OS2; out of range → field unchanged",
               !applyMessage("OS", "2", s) && !s.operate);
    }

    // ── applyMessage: ^ON power status ───────────────────────────────────────
    {
        Status s;
        report("^ON1; → powerOn=true (amp is on)",
               applyMessage("ON", "1", s)
                   && s.powerOn && *s.powerOn == true);
    }

    // ── applyMessage: ^RVM / ^SN one-shot ────────────────────────────────────
    {
        Status s;
        report("^RVM01.04; → firmwareVersion '01.04'",
               applyMessage("RVM", "01.04", s)
                   && s.firmwareVersion && *s.firmwareVersion == "01.04");
    }
    {
        Status s;
        report("^SN12345; → serialNumber '12345'",
               applyMessage("SN", "12345", s)
                   && s.serialNumber && *s.serialNumber == "12345");
    }

    // ── assignIfChanged: no spurious updates ─────────────────────────────────
    {
        Status s;
        applyMessage("TM", "045", s);
        bool changed = applyMessage("TM", "045", s);
        report("Same value applied twice — second call returns false (no change)",
               !changed);
    }
    {
        Status s;
        applyMessage("TM", "045", s);
        bool changed = applyMessage("TM", "046", s);
        report("Different value — second call returns true (changed)",
               changed && s.paTemperatureC && *s.paTemperatureC == 46.0f);
    }

    // ── Band name table ───────────────────────────────────────────────────────
    report("bandName(0) == '160m'",  bandName(0)  == QStringLiteral("160m"));
    report("bandName(5) == '20m'",   bandName(5)  == QStringLiteral("20m"));
    report("bandName(10) == '6m'",   bandName(10) == QStringLiteral("6m"));
    report("bandName(-1) == '?m'",   bandName(-1) == QStringLiteral("?m"));
    report("bandName(11) == '?m'",   bandName(11) == QStringLiteral("?m"));

    std::printf("\n%s (%d failure(s))\n",
                g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
