// Standalone tests for the client-side transverter RF<->IF translation used by
// non-Flex backends. Build target: client_transverter_test.

#include "models/ClientTransverter.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-72s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

// QO-100 narrowband through one 28.5 MHz IF: separate down and up converters.
ClientTransverter qo100()
{
    ClientTransverter t;
    t.name = QStringLiteral("QO-100");
    t.ifLowMhz = 28.5;
    t.ifHighMhz = 29.0;
    t.rxLoMhz = 10461.0;
    t.txLoMhz = 2371.5;
    return t;
}

ClientTransverter twentyThreeCm()
{
    ClientTransverter t;
    t.name = QStringLiteral("23cm");
    t.ifLowMhz = 144.0;
    t.ifHighMhz = 146.0;
    t.rxLoMhz = 1152.0;
    return t;
}

void testTuneIntoRfWindowActivates()
{
    ClientTransverterSet set;
    set.setTransverters({qo100(), twentyThreeCm()});
    report("starts with no active transverter", set.active() == nullptr);

    const double radio = set.toRadioMhz(10489.540).value_or(-1.0);
    report("RF 10489.540 tunes the radio to IF 28.540", near(radio, 28.540, 1e-6));
    report("QO-100 is active", set.active() && set.active()->name == QStringLiteral("QO-100"));
    report("IF report shows RX RF", near(set.toDisplayMhz(28.540), 10489.540, 1e-6));
    report("TX frequency uses the TX LO", near(set.toDisplayTxMhz(28.540), 2400.040, 1e-6));

    report("second transverter by its own window",
           near(set.toRadioMhz(1296.200).value_or(-1.0), 144.200, 1e-6) && set.activeIndex() == 1);
    report("single-LO transverter: TX equals RX",
           near(set.toDisplayTxMhz(144.200), 1296.200, 1e-6));
}

void testLeavingWindowsFallsBackToNative()
{
    ClientTransverterSet set;
    set.setTransverters({qo100()});
    set.toRadioMhz(10489.540);
    report("native tune clears the transverter",
           near(set.toRadioMhz(14.074).value_or(-1.0), 14.074) && set.active() == nullptr);

    set.toRadioMhz(10489.540);
    report("radio dial leaving the IF window shows native",
           near(set.toDisplayMhz(28.074), 28.074) && set.active() == nullptr);
    report("no active transverter: IF report passes through",
           near(set.toDisplayMhz(28.540), 28.540));
}

void testStepOffTheEdgeIsHeld()
{
    ClientTransverterSet set;
    set.setTransverters({qo100()});
    set.setNativeRangeMhz(0.03, 470.0);   // IC-705
    set.toRadioMhz(10489.500);
    report("a wheel step below the RF window holds the IF at the edge",
           near(set.toRadioMhz(10489.4999).value_or(-1.0), 28.5, 1e-6) && set.active() != nullptr);
    report("above the window holds at the top edge",
           near(set.toRadioMhz(10490.2).value_or(-1.0), 29.0, 1e-6) && set.active() != nullptr);
    report("a native frequency still drops the transverter",
           near(set.toRadioMhz(144.174).value_or(-1.0), 144.174) && set.active() == nullptr);

    set.toRadioMhz(10489.500);
    report("a step up to the hold reach past the edge is still held",
           near(set.toRadioMhz(10490.9).value_or(-1.0), 29.0, 1e-6) && set.active() != nullptr);
    report("a tune to an unrelated band is refused, and the transverter stays active",
           !set.toRadioMhz(1296.200) && set.active() != nullptr
               && near(set.toDisplayMhz(28.540), 10489.540, 1e-6));
    set.toRadioMhz(14.074);
    report("with none active, a frequency the radio cannot reach is refused",
           !set.toRadioMhz(1296.200) && set.active() == nullptr);

    ClientTransverterSet unknown;
    unknown.setTransverters({qo100()});
    unknown.toRadioMhz(10489.5);
    report("unknown native range keeps the pass-through rule",
           near(unknown.toRadioMhz(10489.4999).value_or(-1.0), 10489.4999) && unknown.active() == nullptr);
}

void testGeometryShiftDoesNotChangeState()
{
    ClientTransverterSet set;
    set.setTransverters({qo100()});
    set.toRadioMhz(10489.600);
    report("scope centre outside the IF window still shifts",
           near(set.shiftToDisplayMhz(28.45), 10489.45, 1e-6) && set.active() != nullptr);
    report("pan centre request shifts back to IF",
           near(set.shiftToRadioMhz(10489.75), 28.75, 1e-6));
}

void testUnusableEntriesAreIgnored()
{
    ClientTransverter off = qo100();
    off.enabled = false;
    ClientTransverter bad = twentyThreeCm();
    bad.ifHighMhz = bad.ifLowMhz;
    ClientTransverterSet set;
    set.setTransverters({off, bad});
    report("disabled or malformed transverters never activate",
           near(set.toRadioMhz(10489.540).value_or(-1.0), 10489.540) && set.active() == nullptr);

    set.setTransverters({qo100()});
    set.setActiveIndex(0);
    set.setTransverters({off});
    report("disabling the active transverter clears it", set.active() == nullptr);
}

void testAbsurdFrequenciesAreUnusable()
{
    ClientTransverter wild = qo100();
    wild.rxLoMhz = 1.0e13;
    report("an RX LO above 300 GHz is unusable", !wild.isUsable());
    wild = qo100();
    wild.txLoMhz = 1.0e13;
    report("a TX LO above 300 GHz is unusable", !wild.isUsable());
    ClientTransverterSet set;
    set.setTransverters({wild});
    set.setNativeRangeMhz(0.03, 470.0);
    report("an unusable entry opens no RF window", !set.toRadioMhz(1.0e13 + 28.540));
    report("a 241 GHz transverter is still usable", [] {
        ClientTransverter t = qo100();
        t.rxLoMhz = 241000.0;
        t.txLoMhz = 0.0;
        return t.isUsable();
    }());
}

void testJsonRoundTrip()
{
    const ClientTransverter t = ClientTransverter::fromJson(qo100().toJson());
    report("JSON round trip keeps every field",
           t.name == QStringLiteral("QO-100") && near(t.ifLowMhz, 28.5) && near(t.ifHighMhz, 29.0)
               && near(t.rxLoMhz, 10461.0) && near(t.txLoMhz, 2371.5) && t.enabled);
}

} // namespace

int main()
{
    testTuneIntoRfWindowActivates();
    testLeavingWindowsFallsBackToNative();
    testStepOffTheEdgeIsHeld();
    testGeometryShiftDoesNotChangeState();
    testUnusableEntriesAreIgnored();
    testAbsurdFrequenciesAreUnusable();
    testJsonRoundTrip();
    return g_failed == 0 ? 0 : 1;
}
