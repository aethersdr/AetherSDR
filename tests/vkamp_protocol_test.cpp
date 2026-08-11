#include "core/VkampProtocol.h"

#include <QByteArray>
#include <QList>

#include <cmath>
#include <cstdio>

using namespace AetherSDR::Vkamp;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool nearlyEqual(float a, float b, float tolerance = 0.01f)
{
    return std::fabs(a - b) <= tolerance;
}

}  // namespace

int main()
{
    // ── Status parsing (TCP) ────────────────────────────────────────────────
    // "23,577,3,1,0,0,0,0" is a real capture example from the companion
    // project's own module docstring: temp=23C, volts=57.7 (high rail),
    // band code 3 ("40"), antenna 1, no fault, not transmitting, cooling off,
    // not bypassed.
    {
        const auto s = parseStatus("23,577,3,1,0,0,0,0");
        report("parseStatus decodes a real capture example", s.has_value());
        if (s) {
            report("temp_c decodes correctly", s->temp_c == 23);
            report("volts decodes as x10 field / 10.0", nearlyEqual(s->volts, 57.7f));
            report("band field decodes to bandName '40'", bandName(s->band) == "40");
            report("antenna decodes correctly", s->antenna == 1);
            report("error_code decodes correctly (0 = no fault)", s->error_code == 0);
            report("tx/cooling/bypass all decode false", !s->tx && !s->cooling_override && !s->bypass);
        }
    }

    report("parseStatus rejects malformed input", !parseStatus("not,a,status,string").has_value());
    report("parseStatus rejects a short field list", !parseStatus("23,577,3,1,0").has_value());

    // ── bandName() — all 8 confirmed codes, no gaps (design doc Section 3.1) ──
    report("bandName(1) == 160", bandName(1) == "160");
    report("bandName(2) == 80", bandName(2) == "80");
    report("bandName(3) == 40", bandName(3) == "40");
    report("bandName(4) == 30", bandName(4) == "30");
    report("bandName(5) == 20", bandName(5) == "20");
    report("bandName(6) == 17-15 (shared relay group)", bandName(6) == "17-15");
    report("bandName(7) == 12-10 (shared relay group)", bandName(7) == "12-10");
    report("bandName(8) == 6", bandName(8) == "6");
    report("bandName(0)/bandName(9) are empty -- not a guess for an unconfirmed code",
           bandName(0).isEmpty() && bandName(9).isEmpty());

    // ── voltageLow() -- inferred from the live volts reading against the
    //    confirmed rail midpoint (design doc Section 3.1/Section 5) ──────────
    {
        Status high;
        high.volts = 57.7f;
        report("voltageLow() is false at the confirmed high-rail reading", !high.voltageLow());

        Status low;
        low.volts = 41.6f;
        report("voltageLow() is true at the confirmed low-rail reading", low.voltageLow());

        // The bypass standby reading (~6.3V) is neither rail target, but this
        // raw inference has no bypass-awareness of its own -- it naively
        // reads as "low" here. This is exactly why design doc Section 5
        // requires every CALLER to gate this on !bypass before trusting it;
        // this test documents the raw behavior, not a claim that it's safe
        // to use unconditionally.
        Status bypassed;
        bypassed.volts = 6.3f;
        bypassed.bypass = true;
        report("voltageLow() naively reads true at the bypass standby voltage -- "
               "callers MUST gate on !bypass (design doc Section 5), this is not "
               "a bug in voltageLow() itself",
               bypassed.voltageLow());
    }

    // ── StatusStreamParser: split across two feed() calls ───────────────────
    {
        QList<Status> received;
        StatusStreamParser parser;
        parser.setStatusCallback([&](const Status& s) { received.append(s); });

        const QByteArray full = "19,577,3,1,0,0,0,0";
        parser.feed(full.left(10));  // "19,577,3,1" -- mid-field boundary
        report("no status emitted until the full line has arrived", received.isEmpty());
        parser.feed(full.mid(10));
        report("status split across feed() calls still decodes",
               received.size() == 1 && received.at(0).temp_c == 19 && received.at(0).antenna == 1);
    }

    // ── StatusStreamParser: two broadcasts glued with no separator, in one
    //    feed() call -- the amp is known to glue 2-3 together on the wire
    //    (design doc Section 6 / the companion project's own comment on
    //    this). The trailing 5 fields are all single-digit, so the boundary
    //    is unambiguous even with no delimiter between messages. ─────────────
    {
        QList<Status> received;
        StatusStreamParser parser;
        parser.setStatusCallback([&](const Status& s) { received.append(s); });

        parser.feed(QByteArray("19,577,3,1,0,0,0,0") + "20,580,3,1,0,0,0,0");
        report("two glued broadcasts in one feed() both decode, in order",
               received.size() == 2
                   && received.at(0).temp_c == 19 && nearlyEqual(received.at(0).volts, 57.7f)
                   && received.at(1).temp_c == 20 && nearlyEqual(received.at(1).volts, 58.0f));
    }

    // ── StatusStreamParser: buffer-trim mirrors the companion project's own
    //    hard-won fix -- only bytes after the LAST match are kept, so a
    //    trailing partial line at the end of a feed() is preserved and
    //    completed by the next feed() rather than being lost or re-scanned
    //    from scratch. ─────────────────────────────────────────────────────
    {
        QList<Status> received;
        StatusStreamParser parser;
        parser.setStatusCallback([&](const Status& s) { received.append(s); });

        parser.feed(QByteArray("19,577,3,1,0,0,0,0") + "20,580,3,1");  // second line partial
        report("a trailing partial line after a full match isn't lost",
               received.size() == 1 && received.at(0).temp_c == 19);
        parser.feed(",0,0,0,0");  // complete the second line
        report("the preserved partial line completes on the next feed()",
               received.size() == 2 && received.at(1).temp_c == 20);
    }

    // ── StatusStreamParser: a non-matching garbage flood doesn't wedge the
    //    parser -- it must still recover once a real status arrives. ────────
    {
        QList<Status> received;
        StatusStreamParser parser;
        parser.setStatusCallback([&](const Status& s) { received.append(s); });

        parser.feed(QByteArray(6000, 'x'));  // no digits at all, over the internal cap
        report("a non-matching garbage flood emits nothing", received.isEmpty());
        parser.feed("19,577,3,1,0,0,0,0");
        report("the parser still decodes a real status after a garbage flood",
               received.size() == 1 && received.at(0).temp_c == 19);
    }

    // ── Telemetry parsing (UDP) ──────────────────────────────────────────────
    {
        const auto t = parseTelemetry("450,8,94,50");
        report("parseTelemetry decodes a well-formed 4-field frame", t.has_value());
        if (t) {
            report("output/reflected/current/input_raw all round-trip",
                   t->output == 450 && t->reflected == 8 && t->current == 94 && t->input_raw == 50);
        }
    }
    {
        // The companion project's own framing: payload may carry trailing
        // NUL-padded garbage after the CSV text -- strip at the first NUL.
        QByteArray padded = "450,8,94,50";
        padded.append('\0');
        padded.append("garbage");
        const auto t = parseTelemetry(padded);
        report("parseTelemetry strips trailing bytes after the first NUL",
               t.has_value() && t->output == 450 && t->input_raw == 50);
    }
    report("parseTelemetry rejects a wrong field count", !parseTelemetry("450,8,94").has_value());
    report("parseTelemetry rejects a non-numeric field", !parseTelemetry("450,x,94,50").has_value());

    // ── Per-variant power scaling (§ meter full-scale = rated * 1.25) ──────
    report("VKAMP 600W variant: 600 W rated, 750 W meter full-scale",
           nearlyEqual(ratedWatts(Variant::W600), 600.0f) &&
           nearlyEqual(meterFullScaleWatts(Variant::W600), 750.0f));
    report("VKAMP 1000W variant: 1000 W rated, 1250 W meter full-scale",
           nearlyEqual(ratedWatts(Variant::W1000), 1000.0f) &&
           nearlyEqual(meterFullScaleWatts(Variant::W1000), 1250.0f));
    report("VKAMP 2000W variant: 2000 W rated, 2500 W meter full-scale",
           nearlyEqual(ratedWatts(Variant::W2000), 2000.0f) &&
           nearlyEqual(meterFullScaleWatts(Variant::W2000), 2500.0f));

    // ── Calibration curves -- ported verbatim from the companion project's
    //    own fitted constants (design doc Section 3.2). Expected values below
    //    are computed independently from those same published constants, not
    //    read back from this port's own output. ─────────────────────────────
    {
        Telemetry t;
        t.output = 100;
        report("outputWatts() is 0 below the ramp start (raw 100 < 154.0)",
               nearlyEqual(t.outputWatts(), 0.0f));

        // Floor lowered from the companion project's original 175.6 to 157.0
        // (kOutputCalMinRaw's own doc comment) after a live capture on real
        // hardware: raw~157-166, steady, paired with a confirmed ~111W true
        // reading. Anchored at the LOW edge of that cluster (157), not its
        // mode (165) -- an earlier version of this fix anchored at the mode
        // and put most of the real jitter inside the ramp, where a 1-count
        // ADC blip swung the reading by ~40W (looked like flicker on a
        // perfectly steady transmission). raw 156 below exercises the ramp
        // zone, which now only covers the rare sub-157 case.
        t.output = 156;
        report("outputWatts() tapers continuously inside the (lowered) ramp zone (raw 156)",
               nearlyEqual(t.outputWatts(), 72.1666f, 0.05f));

        // Regression guard for the mode-vs-low-edge bug above: the real
        // observed cluster (157-166) must resolve through the smooth
        // quadratic, not the steep ramp -- so a 1-count ADC blip anywhere
        // in that range should swing the reading by low single-digit watts
        // (matching the curve's own ~1.3W/count local slope here), not the
        // ~40W/count the ramp produces.
        t.output = 157;
        const float lowEdge = t.outputWatts();
        t.output = 166;
        const float highEdge = t.outputWatts();
        report("outputWatts() swings by only a few W/count across the confirmed "
               "157-166 cluster, not ~40W/count (the flicker bug)",
               (highEdge - lowEdge) / 9.0f < 5.0f);

        t.output = 176;
        report("outputWatts() follows the full quadratic above the ramp (raw 176)",
               nearlyEqual(t.outputWatts(), 134.7195f, 0.05f));

        t.output = 401;
        report("outputWatts() at raw 401", nearlyEqual(t.outputWatts(), 681.2708f, 0.05f));

        t.output = 669;
        // Comfortably close to the companion project's own highest confirmed
        // real-hardware reading (raw~669 -> true 1920W) -- a useful sanity
        // check that this isn't just internally self-consistent but actually
        // in the right ballpark of the real calibration data.
        report("outputWatts() at raw 669 lands near the confirmed ~1920W reading",
               nearlyEqual(t.outputWatts(), 1928.5255f, 0.05f));
    }
    {
        Telemetry t;
        t.reflected = 8;
        report("reflectedWatts() at raw 8", nearlyEqual(t.reflectedWatts(), 2.0125f, 0.01f));
    }
    {
        Telemetry t;
        t.current = 94;
        report("currentAmps() at raw 94", nearlyEqual(t.currentAmps(), 38.4028f, 0.01f));
    }
    {
        Telemetry t;
        t.input_raw = 50;
        report("inputWatts() at raw 50", nearlyEqual(t.inputWatts(), 10.9847f, 0.01f));
    }
    {
        Telemetry t;
        t.output = 669;
        t.reflected = 30;
        report("swr() computes from calibrated watts, not a raw wire field",
               nearlyEqual(t.swr(), 1.0976f, 0.01f));

        Telemetry idle;
        report("swr() defaults to 1.0 with no carrier (matches the amp's own idle display)",
               nearlyEqual(idle.swr(), 1.0f));
    }

    // ── Command builders -- bare 2-digit ASCII, no terminator ───────────────
    report("buildBypass(true) == \"21\"", buildBypass(true) == "21");
    report("buildBypass(false) == \"22\"", buildBypass(false) == "22");
    report("buildCooling(true) == \"45\"", buildCooling(true) == "45");
    report("buildCooling(false) == \"46\"", buildCooling(false) == "46");
    report("buildVoltage(low=true) == \"41\"", buildVoltage(true) == "41");
    report("buildVoltage(low=false) == \"42\"", buildVoltage(false) == "42");
    report("buildSelectAntenna(2) == \"32\"", buildSelectAntenna(2) == "32");
    report("buildPoll() == \"11\"", buildPoll() == "11");
    report("buildResetHold() == \"23\"", buildResetHold() == "23");

    std::printf("\n%d VK3AMP protocol test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
