// The noise-floor auto-adjust GATE, pinned as a truth table and as five real
// backends' declarations.
//
// One flag, RadioCapabilities::radioOwnsDbmScale, was answering two questions:
//
//   Q1  can the radio be commanded a display dBm range and echo it back?
//   Q2  can the auto-floor loop converge?
//
// On a Flex the answers coincide. On a raw-IQ radio they do not: a Hermes-Lite 2
// owns no dBm scale at all, yet bench run d101 measured its auto-floor SETTLING
// — 0.307 dB of drift over 74 s quiescent, 0.0000 dB/s over the second half,
// and a re-settle within ~30 s after a 12 dB LNA step — because its bins are
// computed on this host and hold still while the reference level moves. So Q2
// has its own capability now, PanAmplitudeModel::binsAbsolute — read through
// RadioCapabilities::panBinsAbsolute() — and the gate is the OR.
//
// ⚠ What this file can and cannot see. It calls noiseFloorAutoAdjustAllowed()
// — the SAME function SpectrumWidget::applyNoiseFloorAutoAdjust branches on, not
// a copy — so inverting or weakening the gate fails here. It reads
// capabilities() off real backend instances, so a declaration that disappears
// fails here too. What it does NOT see is the widget wiring: that
// MainWindow::applyCapabilitiesToUi and the late-pane path in
// MainWindow_Wiring.cpp actually push both values in. Those need a MainWindow
// and are left to the GUI tests.
//
// Deliberately no radio was measured for this file. The d101 numbers above are
// quoted from that run's record; nothing here talks to hardware.

#include "TestSettingsProfile.h"
#include "core/backends/NoiseFloorAutoAdjustGate.h"
#include "core/backends/RadioCapabilities.h"

#include "core/backends/anan/AnanBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif

#include <QCoreApplication>

#include <cstdio>

using AetherSDR::noiseFloorAutoAdjustAllowed;
using AetherSDR::RadioCapabilities;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// The gate as the widget reaches it: the two capability values, composed.
static bool gateOpen(const RadioCapabilities& c)
{
    return noiseFloorAutoAdjustAllowed(c.radioOwnsDbmScale, c.panBinsAbsolute());
}

int main(int argc, char** argv)
{
    // TWO OF THE BACKENDS CONSTRUCTED BELOW TOUCH AppSettings BEFORE ANY
    // ASSERTION RUNS. Hl2Backend's constructor calls
    // AutomationBridgeSettings::txAllowed(); FlexBackend's starts the
    // PanadapterStream thread, whose init() reads
    // AppSettings::instance().value("AudioPacketLossConcealment", ...) and
    // NetworkSettings::vitaReceiveBufferBytes() on that worker.
    //
    // So without this a run of the suite reads, and can write, the operator's
    // live configuration. hl2_pan_limits_declaration_test -- added in the same
    // PR as this file -- opens with exactly this guard and says so; this one
    // was missed. Four of the five existing tests that construct Hl2Backend use
    // the profile too. Reported by aethersdr-agent on #5726.
    //
    // Before QCoreApplication, deliberately: the redirect has to be in place
    // before anything can resolve a settings path.
    TestSettingsProfile settingsProfile(QStringLiteral("noise-floor-gate-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    // ── 1. The truth table, all four rows ────────────────────────────────────
    //
    // Every row is stated, including the two that an OR makes "obvious", because
    // the mutations this exists to catch are exactly the ones that keep three
    // rows right: an AND passes the true/true row, dropping the second term
    // passes both rows where the echo is present, and inverting the sense
    // passes neither but only if the false/false row is asserted.
    check(noiseFloorAutoAdjustAllowed(true, true),
          "echo AND absolute bins: the loop runs (a hypothetical radio; nothing "
          "declares both today)");
    check(noiseFloorAutoAdjustAllowed(true, false),
          "echo alone opens the gate — the Flex case, and the default");
    check(noiseFloorAutoAdjustAllowed(false, true),
          "ABSOLUTE BINS ALONE open the gate — the whole point of the split. A "
          "radio that echoes nothing still converges when its bins hold still");
    check(!noiseFloorAutoAdjustAllowed(false, false),
          "neither: the early return fires. This is the ONLY closed row, and it "
          "is the 24 dB/s ratchet the gate exists to stop");

    // The gate is symmetric in its two arguments — neither term is privileged,
    // because either one alone terminates the loop. Pin it, so a change that
    // makes one term merely a modifier of the other has to say so.
    for (int i = 0; i < 4; ++i) {
        const bool a = (i & 1) != 0;
        const bool b = (i & 2) != 0;
        check(noiseFloorAutoAdjustAllowed(a, b) == noiseFloorAutoAdjustAllowed(b, a),
              "the gate is symmetric in echo and absolute-bins");
    }

    // ── 2. The defaults, and the claim that they change nothing ──────────────
    //
    // The load-bearing argument for panBinsAbsolute() answering FALSE on an
    // absent record is that radioOwnsDbmScale still defaults TRUE and the gate
    // is an OR, so a backend nobody has read is unaffected. That is an
    // arithmetic claim; assert it rather than trusting it.
    //
    // ABSENT IS NOT THE SAME AS FALSE, which is the whole reason this is an
    // optional record rather than a bool, so the absence is asserted separately
    // from what the accessor makes of it. An accessor is exactly the kind of
    // indirection that turns a sharp test blunt: pin both halves.
    {
        const RadioCapabilities d;
        check(d.radioOwnsDbmScale, "radioOwnsDbmScale still defaults TRUE");
        check(!d.panAmplitude.has_value(),
              "a descriptor nobody has written declares NO amplitude model — "
              "absent, not false");
        check(!d.panBinsAbsolute(),
              "and panBinsAbsolute() reads that absence as NOT absolute");
        check(d.dbmAxisIsCalibrated(),
              "while dbmAxisIsCalibrated() reads the SAME absence the other way, "
              "keeping the legacy claim. Two opposite falls on one absent "
              "record is why neither is unwrapped at a call site");
        check(gateOpen(d),
              "an UNREAD backend's default capabilities still open the gate — "
              "adding the second flag changes nothing for anyone who did not "
              "declare it");
    }

    // ── 3. Each family, from its own backend, declaration and outcome ────────
    //
    // The table in the brief, asserted as behaviour rather than as a list of
    // flags: for each family, what it declares AND whether the gate opens.
    {
        AetherSDR::FlexBackend flex;
        const RadioCapabilities c = flex.capabilities();
        check(c.radioOwnsDbmScale,
              "Flex: the radio owns the scale (inherited default — `display pan "
              "set min_dbm=...` is a real command it adopts and reports)");
        check(!c.panAmplitude.has_value(),
              "Flex: no amplitude model declared at all — nobody has read it");
        check(!c.panBinsAbsolute(),
              "Flex: bins are NOT declared absolute — the radio renders them "
              "against the range it was given, and nobody has claimed otherwise");
        check(gateOpen(c), "Flex: loop runs — UNCHANGED by this split");
    }
    {
        AetherSDR::hl2::Hl2Backend hl2;
        const RadioCapabilities c = hl2.capabilities();
        check(c.panAmplitude && c.panAmplitude->binsAbsolute,
              "HL2: the amplitude record is PRESENT and declares absolute bins — "
              "host-computed dBFS, passed through onBackendSpectrumFrame "
              "untouched. Asserted on the field, not only through the accessor, "
              "so an accessor that answered true for everyone would still fail "
              "the Flex and Icom rows");
        check(c.panBinsAbsolute(), "HL2: and the accessor agrees");
        check(gateOpen(c),
              "HL2: loop runs — UNCHANGED today (radioOwnsDbmScale is still true "
              "by omission), and this is what makes #5725's other half safe: the "
              "gate stays open on the second term when the first goes false");
        check(noiseFloorAutoAdjustAllowed(false, c.panBinsAbsolute()),
              "HL2: and it stays open with the echo declared FALSE — the "
              "measurement d101 made, held as a standing assertion");
    }
    {
        AetherSDR::anan::AnanBackend anan;
        const RadioCapabilities c = anan.capabilities();
        check(!c.radioOwnsDbmScale,
              "ANAN: the client computes the dBm scale, so no echo");
        check(c.panAmplitude && c.panAmplitude->binsAbsolute,
              "ANAN: bins are absolute — binsDbfs[i] + a 0.0f offset is the dBFS "
              "relabelled");
        check(!c.dbmAxisIsCalibrated(),
              "ANAN: and the same record says the axis is NOT calibrated — the "
              "two questions travel together and answer differently");
        check(gateOpen(c),
              "ANAN: loop RUNS. This is the one real behaviour change in the "
              "split — it was off, switched off by the conflation and not by a "
              "decision about this radio");
    }
    {
        AetherSDR::icom::IcomCivBackend icom;
        const RadioCapabilities c = icom.capabilities();
        check(!c.radioOwnsDbmScale,
              "Icom: the scope scale comes from ScopeCalibration, no range "
              "command, no echo");
        check(!c.panBinsAbsolute(),
              "Icom: NOT declared absolute. Its scale is shifted by the radio's "
              "own reference level, there is no Icom on this bench, and the "
              "24 dB/s ratchet on an IC-9700 stands unrefuted. Leaving it out is "
              "the conservative choice, not a conclusion — this check is here to "
              "make flipping it a deliberate act with a measurement behind it");
        check(!gateOpen(c), "Icom: loop stays OFF — UNCHANGED");
    }
#ifdef AETHER_BACKEND_RTL
    {
        AetherSDR::rtl::RtlSdrBackend rtl;
        const RadioCapabilities c = rtl.capabilities();
        check(c.panAmplitude && c.panAmplitude->binsAbsolute,
              "RTL-SDR: bins are absolute — 20*log10(mag/kFftSize) on raw ADC "
              "magnitudes, emitted directly");
        check(gateOpen(c), "RTL-SDR: loop runs — UNCHANGED");
    }
#else
    std::fprintf(stderr,
                 "noise_floor_auto_adjust_gate_test: RTL-SDR backend not built "
                 "(AETHER_BACKEND_RTL undefined); its row is NOT checked here\n");
#endif

    if (g_failures == 0)
        std::fprintf(stderr, "noise_floor_auto_adjust_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
