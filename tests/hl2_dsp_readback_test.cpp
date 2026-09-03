// The DSP read-back: does it report the DSP, or does it report the request?
//
// §8's first "still missing" item exists because `get_state` answers from the
// MODEL, so a control that moves the model and never reaches the DSP reads as
// "the control does nothing" — the hardest symptom to act on, because it is
// indistinguishable from the operator having misunderstood the control.
//
// A read-back is only worth having if it can FAIL to agree with the request.
// So these cases are not "the accessor returns something": they are that the
// value MOVES when the DSP is reconfigured, and that it does NOT move when
// only the request does. A read-back that returned its own input would pass a
// test written the first way and be worthless.

#include "core/backends/hl2/Hl2TxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>
#include <string>

using AetherSDR::hl2::Hl2TxDsp;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("\n  DSP read-back — reports the DSP, not the request\n\n");

    Hl2TxDsp dsp;

    // 1. What it was configured with is what it reports.
    Hl2TxDsp::Config a;
    a.inputSampleRateHz = 24000;
    a.outputSampleRateHz = 48000;
    a.dspBlockSize = 512;
    a.filterLowHz = 300.0;
    a.filterHighHz = 2700.0;
    a.alcTargetPeak = 0.85;
    a.alcMaxGainDb = 40.0;
    std::string err;
    check(dsp.configure(a, &err), "the modulator accepts a configuration");
    check(dsp.config().filterLowHz == 300.0 && dsp.config().filterHighHz == 2700.0,
          "the read-back reports the passband it was configured with");
    check(near(dsp.config().alcTargetPeak, 0.85)
              && near(dsp.config().alcMaxGainDb, 40.0),
          "the read-back reports the ALC quartet it was configured with");
    check(dsp.config().dspBlockSize == 512 && dsp.config().outputSampleRateHz == 48000,
          "the read-back reports rates and block size");

    // 2. IT MOVES. A read-back that returns a constant would satisfy case 1
    //    and prove nothing, so reconfigure and require the value to change.
    Hl2TxDsp::Config b = a;
    b.filterLowHz = 150.0;
    b.filterHighHz = 3000.0;
    b.alcMaxGainDb = 0.0;
    check(dsp.configure(b, &err), "the modulator accepts a second configuration");
    check(dsp.config().filterLowHz == 150.0 && dsp.config().filterHighHz == 3000.0,
          "the read-back MOVED when the DSP was reconfigured");
    check(near(dsp.config().alcMaxGainDb, 0.0),
          "the ALC ceiling moved with it");

    // 3. THE DEAD-SLIDER CASE, which is the whole reason the verb exists.
    //    Change the REQUEST without reaching the DSP, and the read-back must
    //    keep reporting what the DSP actually has. Here the request is a local
    //    Config the caller has edited and not applied — exactly the shape of a
    //    control that updates the model and never crosses the seam.
    Hl2TxDsp::Config requested = b;
    requested.filterLowHz = 100.0;
    requested.filterHighHz = 3900.0;
    check(requested.filterLowHz != dsp.config().filterLowHz,
          "the request and the DSP now disagree, which is the defect's shape");
    check(dsp.config().filterLowHz == 150.0,
          "the read-back still reports the DSP, NOT the unapplied request");

    // 4. And once the request IS applied, the disagreement closes. Without
    //    this, case 3 could pass on a read-back that had simply stuck.
    check(dsp.configure(requested, &err), "applying the request succeeds");
    check(dsp.config().filterLowHz == 100.0 && dsp.config().filterHighHz == 3900.0,
          "applying it closes the disagreement");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
