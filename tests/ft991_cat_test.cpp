#include "core/backends/ft991/Ft991Cat.h"

#include <QByteArray>
#include <QString>

#include <cstdio>

using namespace AetherSDR::ft991;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool parsesTo(const char* frame, Ft991Cat::Response::Kind kind, int raw)
{
    const auto r = Ft991Cat::parse(QByteArray(frame));
    return r && r->kind == kind && r->raw == raw;
}

}  // namespace

int main()
{
    // ── Frame builders. Every literal here is the CAT Operation Reference
    //    Manual's own format for the FT-991; hamlib's ft991/newcat driver
    //    encodes the same bytes and served as the cross-check. These are
    //    written out rather than derived, so a wrong field width or a
    //    dropped VFO digit fails loudly instead of being "consistent".
    report("FA takes 9 zero-padded digits",
           Ft991Cat::setFrequency(14'074'000.0) == QByteArray("FA014074000;"));
    report("FA rounds fractional Hz to the nearest whole hertz",
           Ft991Cat::setFrequency(14'074'000.6) == QByteArray("FA014074001;"));
    report("FA refuses a frequency below the tuning floor",
           Ft991Cat::setFrequency(1000.0).isEmpty());
    report("FA refuses a frequency above the tuning ceiling",
           Ft991Cat::setFrequency(9.0e8).isEmpty());

    report("MD0 encodes USB as 2", Ft991Cat::setMode("USB") == QByteArray("MD02;"));
    report("MD0 encodes LSB as 1", Ft991Cat::setMode("LSB") == QByteArray("MD01;"));
    report("MD0 encodes DIGU as C (DATA-USB)",
           Ft991Cat::setMode("DIGU") == QByteArray("MD0C;"));
    report("MD0 encodes DIGL as 8 (DATA-LSB)",
           Ft991Cat::setMode("DIGL") == QByteArray("MD08;"));
    report("MD0 encodes CWL as 7 (CW-R)",
           Ft991Cat::setMode("CWL") == QByteArray("MD07;"));
    report("CWU folds onto CW (the radio has no separate CW-U code)",
           Ft991Cat::setMode("CWU") == Ft991Cat::setMode("CW"));
    report("an unmappable mode yields no frame rather than a wrong one",
           Ft991Cat::setMode("PACTOR").isEmpty());

    report("TX1 keys, TX0 unkeys",
           Ft991Cat::setPtt(true) == QByteArray("TX1;")
               && Ft991Cat::setPtt(false) == QByteArray("TX0;"));
    report("PC takes 3 digits and clamps to the radio's 5..100 W range",
           Ft991Cat::setPowerWatts(5) == QByteArray("PC005;")
               && Ft991Cat::setPowerWatts(100) == QByteArray("PC100;")
               && Ft991Cat::setPowerWatts(0) == QByteArray("PC005;")
               && Ft991Cat::setPowerWatts(250) == QByteArray("PC100;"));
    report("GT0 maps the neutral AGC vocabulary",
           Ft991Cat::setAgc("off") == QByteArray("GT00;")
               && Ft991Cat::setAgc("fast") == QByteArray("GT01;")
               && Ft991Cat::setAgc("med") == QByteArray("GT02;")
               && Ft991Cat::setAgc("slow") == QByteArray("GT03;"));

    // ── Radio-side DSP builders ──────────────────────────────────────────
    report("NB0/NR0/BC0 take one digit",
           Ft991Cat::setNoiseBlanker(true) == QByteArray("NB01;")
               && Ft991Cat::setNoiseReduction(false) == QByteArray("NR00;")
               && Ft991Cat::setAutoNotch(true) == QByteArray("BC01;"));
    report("NL level is 2 digits, clamped 0..10",
           Ft991Cat::setNoiseBlankerLevel(7) == QByteArray("NL0007;")
               && Ft991Cat::setNoiseBlankerLevel(99) == QByteArray("NL0010;")
               && Ft991Cat::setNoiseBlankerLevel(-3) == QByteArray("NL0000;"));
    report("RL (DNR level) is 2 digits, clamped 1..15 — never 0",
           Ft991Cat::setNoiseReductionLevel(9) == QByteArray("RL009;")
               && Ft991Cat::setNoiseReductionLevel(99) == QByteArray("RL015;"));
    report("RL clamps below 1 to 1 (0 is not a level the radio accepts)",
           Ft991Cat::setNoiseReductionLevel(0).endsWith("01;"));
    report("BP01 carries the notch position in 10 Hz steps",
           Ft991Cat::setManualNotchHz(1000) == QByteArray("BP01100;")
               && Ft991Cat::setManualNotchHz(50) == QByteArray("BP01005;"));
    report("BP00 carries the notch on/off flag",
           Ft991Cat::setManualNotch(true) == QByteArray("BP00001;")
               && Ft991Cat::setManualNotch(false) == QByteArray("BP00000;"));
    report("SH takes a 2-digit index",
           Ft991Cat::setWidthIndex(7) == QByteArray("SH007;")
               && Ft991Cat::setWidthIndex(21) == QByteArray("SH021;"));
    report("NA0 carries the narrow flag",
           Ft991Cat::setNarrow(true) == QByteArray("NA01;")
               && Ft991Cat::setNarrow(false) == QByteArray("NA00;"));

    // ── Clarifier. There is no absolute-offset command on the wire, so the
    //    setter is "clear then step" — and the FT-991 has ONE shared knob,
    //    which is why RIT and XIT enable separately but cannot hold
    //    different offsets.
    report("RT/XT carry the two enable flags independently",
           Ft991Cat::setRitEnabled(true) == QByteArray("RT1;")
               && Ft991Cat::setRitEnabled(false) == QByteArray("RT0;")
               && Ft991Cat::setXitEnabled(true) == QByteArray("XT1;")
               && Ft991Cat::setXitEnabled(false) == QByteArray("XT0;"));
    report("a positive offset clears then steps UP, 4 digits",
           Ft991Cat::setClarifierOffset(1200) == QByteArray("RC;RU1200;"));
    report("a negative offset clears then steps DOWN",
           Ft991Cat::setClarifierOffset(-350) == QByteArray("RC;RD0350;"));
    report("zero is a bare clear — no pointless step",
           Ft991Cat::setClarifierOffset(0) == QByteArray("RC;"));
    report("the offset clamps to the 4-digit field",
           Ft991Cat::setClarifierOffset(50000) == QByteArray("RC;RU9999;")
               && Ft991Cat::setClarifierOffset(-50000) == QByteArray("RC;RD9999;"));

    // ── IF response. Field offsets are the whole content of this parse, and
    //    every one of them is an opportunity for an off-by-one that silently
    //    reads the neighbouring field. 27 chars with the ';' stripped:
    //    IF|ch |frequency|clar |R|X|M|V|C|00|S
    //           IF|ch |frequency|clar |RXM VC00S
    {
        const QByteArray f("IF001014074000+000000200000");
        const auto r = Ft991Cat::parse(f);
        report("the IF fixture is the 27 characters the radio really sends",
               f.size() == 27);
        report("IF decodes the 9-digit VFO-A frequency",
               r && r->kind == Ft991Cat::Response::Kind::Info
                   && r->frequencyHz == 14'074'000.0);
    }
    {
        // clarifier +1200, RIT on, XIT off, mode 2 (USB)
        const auto r = Ft991Cat::parse(QByteArray("IF001014074000+120010200000"));
        report("IF decodes a positive clarifier with RIT on and XIT off",
               r && r->kind == Ft991Cat::Response::Kind::Info
                   && r->clarifierHz == 1200 && r->ritOn && !r->xitOn
                   && r->mode == "USB");
    }
    {
        // clarifier -0350, RIT off, XIT on, mode 1 (LSB)
        const auto r = Ft991Cat::parse(QByteArray("IF001007074000-035001100000"));
        report("IF decodes a negative clarifier with XIT on and RIT off",
               r && r->clarifierHz == -350 && !r->ritOn && r->xitOn
                   && r->mode == "LSB");
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("IF00114074000+0000002000000"));
        report("a short IF (8-digit frequency, not ours) is Unknown",
               r && r->kind == Ft991Cat::Response::Kind::Unknown);
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("IF0010140740000000000200000"));
        report("an IF with no clarifier sign is Unknown, not a zero offset",
               r && r->kind == Ft991Cat::Response::Kind::Unknown);
    }

    // ── Width tables. The values are the manual's SH tables (shared with the
    //    FT-891); the snapping is ours. Index 0 is the per-mode DEFAULT and
    //    must never be chosen by the snap — picking it would command "the
    //    radio's idea of default" for every requested width.
    report("SSB snaps 2400 Hz onto its own table entry",
           Ft991Cat::widthForIndex("USB", Ft991Cat::nearestWidthIndex("USB", 2400),
                                   false) == 2400);
    report("SSB snaps an off-table 2450 Hz to a neighbour, not to default",
           Ft991Cat::nearestWidthIndex("USB", 2450) > 0);
    report("SSB's widest entry is 3200 Hz",
           Ft991Cat::widthForIndex("USB", Ft991Cat::nearestWidthIndex("USB", 9000),
                                   false) == 3200);
    report("CW snaps 500 Hz onto its own table entry",
           Ft991Cat::widthForIndex("CW", Ft991Cat::nearestWidthIndex("CW", 500),
                                   false) == 500);
    report("CW's narrowest entry is 50 Hz",
           Ft991Cat::widthForIndex("CW", Ft991Cat::nearestWidthIndex("CW", 10),
                                   false) == 50);
    report("DATA modes use the CW table, not the SSB one",
           Ft991Cat::widthFamilyForMode("DIGU") == Ft991Cat::WidthFamily::CwData
               && Ft991Cat::widthFamilyForMode("DIGL") == Ft991Cat::WidthFamily::CwData);
    report("AM/FM are fixed-width: no SH index exists for them",
           Ft991Cat::nearestWidthIndex("AM", 6000) < 0
               && Ft991Cat::nearestWidthIndex("FM", 12000) < 0);
    report("the snap never returns index 0 (the radio's default slot)",
           Ft991Cat::nearestWidthIndex("USB", 1) != 0
               && Ft991Cat::nearestWidthIndex("CW", 1) != 0);
    report("index 0 decodes to the per-mode default, and NA changes it",
           Ft991Cat::widthForIndex("USB", 0, true) == 1500
               && Ft991Cat::widthForIndex("USB", 0, false) == 2400
               && Ft991Cat::widthForIndex("CW", 0, true) == 500
               && Ft991Cat::widthForIndex("DIGU", 0, true) == 300);
    report("NA follows the table's narrow_max split point",
           Ft991Cat::narrowForWidth("USB", 1800)
               && !Ft991Cat::narrowForWidth("USB", 1950)
               && Ft991Cat::narrowForWidth("CW", 500)
               && !Ft991Cat::narrowForWidth("CW", 800));

    // ── Sideband mapping. This is what decides which half of the pan the
    //    audio bins land on; getting it wrong draws the spectrum mirrored.
    report("USB-side modes report the high sideband",
           !Ft991Cat::isLowSideband("USB") && !Ft991Cat::isLowSideband("DIGU")
               && !Ft991Cat::isLowSideband("CW"));
    report("LSB-side modes report the low sideband",
           Ft991Cat::isLowSideband("LSB") && Ft991Cat::isLowSideband("DIGL")
               && Ft991Cat::isLowSideband("CWL"));

    // ── Response parsing ─────────────────────────────────────────────────
    {
        const auto r = Ft991Cat::parse(QByteArray("FA014074000"));
        report("FA response decodes to Hz",
               r && r->kind == Ft991Cat::Response::Kind::Frequency
                   && r->frequencyHz == 14'074'000.0);
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("MD02"));
        report("MD0 response decodes to the neutral mode name",
               r && r->kind == Ft991Cat::Response::Kind::Mode && r->mode == "USB");
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("MD06"));
        report("RTTY-LSB displays as DIGL (documented lossy mapping)",
               r && r->mode == "DIGL");
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("ID0670"));
        report("ID response carries the FT-991 identity",
               r && r->kind == Ft991Cat::Response::Kind::Id
                   && r->id == QLatin1String(Ft991Cat::kRadioId));
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("?"));
        report("a refused command is reported as Rejected, not as data",
               r && r->kind == Ft991Cat::Response::Kind::Rejected);
    }
    {
        const auto r = Ft991Cat::parse(QByteArray("FA0140740xx"));
        report("a malformed FA is Unknown — it must never tune anything",
               r && r->kind == Ft991Cat::Response::Kind::Unknown);
    }
    report("empty input yields no response at all",
           !Ft991Cat::parse(QByteArray()).has_value());

    report("SM0 decodes the S-meter raw count",
           parsesTo("SM0128", Ft991Cat::Response::Kind::SMeter, 128));
    {
        const auto keyed = Ft991Cat::parse(QByteArray("TX1"));
        const auto idle = Ft991Cat::parse(QByteArray("TX0"));
        report("TX decodes the keying state",
               keyed && keyed->kind == Ft991Cat::Response::Kind::Tx
                   && keyed->txState == 1
                   && idle && idle->txState == 0);
    }
    report("NB0/NR0/BC0 responses decode as flags",
           parsesTo("NB01", Ft991Cat::Response::Kind::NoiseBlanker, 1)
               && parsesTo("NR00", Ft991Cat::Response::Kind::NoiseReduction, 0)
               && parsesTo("BC01", Ft991Cat::Response::Kind::AutoNotch, 1));
    report("NL/RL responses decode as levels",
           parsesTo("NL0007", Ft991Cat::Response::Kind::NoiseBlankerLevel, 7)
               && parsesTo("RL009", Ft991Cat::Response::Kind::NoiseReductionLevel, 9));
    report("SH0 response decodes the width index",
           parsesTo("SH017", Ft991Cat::Response::Kind::Width, 17));
    report("NA0 response decodes the narrow flag",
           parsesTo("NA01", Ft991Cat::Response::Kind::Narrow, 1));
    report("BP00 vs BP01 are distinguished, and BP01 scales to Hz",
           parsesTo("BP00001", Ft991Cat::Response::Kind::ManualNotch, 1)
               && parsesTo("BP01100", Ft991Cat::Response::Kind::ManualNotchFreq, 1000));
    report("RM5/RM6 decode as the TX power and SWR meters",
           parsesTo("RM5123", Ft991Cat::Response::Kind::TxPowerMeter, 123)
               && parsesTo("RM6045", Ft991Cat::Response::Kind::TxSwrMeter, 45));
    {
        // A meter this backend does not poll must not be mistaken for one it
        // does — the RM family shares a prefix across every meter.
        const auto r = Ft991Cat::parse(QByteArray("RM1099"));
        report("an unpolled RM meter is Unknown, not a mis-attributed reading",
               r && r->kind == Ft991Cat::Response::Kind::Unknown);
    }

    if (g_failed == 0) {
        std::printf("\nAll FT-991 CAT tests passed.\n");
    } else {
        std::printf("\n%d FT-991 CAT test(s) FAILED.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
