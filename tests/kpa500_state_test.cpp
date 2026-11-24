// kpa500_state_test.cpp — socket-free priority-mapping test for the KPA500
// main-window status indicator (RFC #7092, maintainer ruling 2026-11-14).
//
// Exercises every priority edge, all-nullopt status, and the "–" fallback that
// fires when required fields are absent after expireTelemetry().

#include "gui/Kpa500IndicatorState.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::Kpa500;

namespace {

int g_failed = 0;

void check(const char* description, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", description);
    if (!ok)
        ++g_failed;
}

// Build a fully-populated OPERATE-state status.
Status operateStatus()
{
    Status s;
    s.powerOn       = true;
    s.operate       = true;
    s.forwardPowerW = 0.0f;
    s.faultCode     = 0;
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Not live ─────────────────────────────────────────────────────────────
    {
        Status s;
        auto r = kpa500IndicatorState(s, /*live=*/false, /*liveTxMeters=*/false);
        check("Not live → label is –",         r.label == "–");
        check("Not live → grey token",          r.colorToken == "color.text.secondary");
    }

    // ── All-nullopt status while live (fields absent after expireTelemetry) ──
    {
        Status s; // all std::optional fields default to nullopt
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("Nullopt status while live → label is –",  r.label == "–");
        check("Nullopt status while live → grey token",  r.colorToken == "color.text.secondary");
    }

    // ── FAULT: highest priority ───────────────────────────────────────────────
    {
        // FAULT even when operate and TX power are set.
        Status s = operateStatus();
        s.faultCode     = 7;
        s.forwardPowerW = 150.0f;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/true);
        check("FAULT > TX > OPERATE: label is FAULT",  r.label == "FAULT");
        check("FAULT → danger token",                  r.colorToken == "color.accent.danger");
    }
    {
        // faultCode == 0 is not a fault.
        Status s = operateStatus();
        s.faultCode = 0;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("faultCode 0 is not FAULT",  r.label != "FAULT");
    }

    // ── TX: second priority ───────────────────────────────────────────────────
    {
        // Exactly 5 W with liveTxMeters — threshold is ≥ 5.
        Status s = operateStatus();
        s.forwardPowerW = 5.0f;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/true);
        check("5 W forward + liveTxMeters → TX",    r.label == "TX");
        check("TX → danger token",                  r.colorToken == "color.accent.danger");
    }
    {
        // 4.9 W — below threshold → OPERATE, not TX.
        Status s = operateStatus();
        s.forwardPowerW = 4.9f;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/true);
        check("4.9 W forward → OPERATE (below TX threshold)",  r.label == "OPERATE");
    }
    {
        // liveTxMeters == false — high power reading doesn't trigger TX.
        Status s = operateStatus();
        s.forwardPowerW = 150.0f;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("High power but !liveTxMeters → OPERATE, not TX",  r.label == "OPERATE");
    }

    // ── OPERATE ───────────────────────────────────────────────────────────────
    {
        Status s = operateStatus();
        s.forwardPowerW = 0.0f; // no meaningful forward power
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("Operate, no TX power → OPERATE",    r.label == "OPERATE");
        check("OPERATE → indicator operate token",
              r.colorToken == "color.accessory.indicator.operate.foreground");
    }
    {
        // OPERATE needs no power reading — forwardPowerW may be nullopt.
        Status s;
        s.powerOn   = true;
        s.operate   = true;
        s.faultCode = 0;
        // forwardPowerW intentionally left nullopt
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("OPERATE with nullopt forwardPowerW → OPERATE",  r.label == "OPERATE");
    }
    {
        // powerOn == true, operate == nullopt — must NOT resolve to STANDBY or OPERATE.
        Status s;
        s.powerOn   = true;
        s.faultCode = 0;
        // operate intentionally left nullopt
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("powerOn=true, operate=nullopt → – (not STANDBY or OPERATE)",
              r.label == "–");
    }

    // ── STANDBY ───────────────────────────────────────────────────────────────
    {
        Status s;
        s.powerOn   = true;
        s.operate   = false;
        s.faultCode = 0;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("powerOn && !operate → STANDBY",         r.label == "STANDBY");
        check("STANDBY → standby amber token",
              r.colorToken == "color.accessory.key.standby.foreground");
    }

    // ── OFF ───────────────────────────────────────────────────────────────────
    {
        Status s;
        s.powerOn   = false;
        s.faultCode = 0;
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("powerOn == false → OFF",   r.label == "OFF");
        check("OFF → grey token",         r.colorToken == "color.text.secondary");
    }

    // ── "–" fallback: powerOn nullopt (expireTelemetry cleared it) ───────────
    {
        Status s;
        s.faultCode = 0;
        // powerOn is nullopt — none of the boolean arms fire
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);
        check("nullopt powerOn while live → –",  r.label == "–");
    }

    // ── FAULT > TX edge ───────────────────────────────────────────────────────
    {
        Status s = operateStatus();
        s.faultCode     = 1;
        s.forwardPowerW = 200.0f; // heavy TX
        auto r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/true);
        check("FAULT + heavy TX → FAULT (not TX)",  r.label == "FAULT");
    }

    // ── FAULT and TX share the danger token ──────────────────────────────────
    {
        Status s = operateStatus();
        s.faultCode     = 1;
        s.forwardPowerW = 0.0f;
        auto fault_r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/false);

        s.faultCode     = 0;
        s.forwardPowerW = 100.0f;
        auto tx_r = kpa500IndicatorState(s, /*live=*/true, /*liveTxMeters=*/true);

        check("FAULT and TX share color.accent.danger",
              fault_r.colorToken == "color.accent.danger"
              && tx_r.colorToken  == "color.accent.danger");
    }

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "SOME TESTS FAILED.");
    return g_failed;
}
