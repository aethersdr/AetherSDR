#include "gui/FmRepeaterUiPolicy.h"

#include <cstdio>

using namespace AetherSDR;

int main()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };

    const FmRepeaterUiPolicy flex = fmRepeaterUiPolicy(
        FmRepeaterPresentation::Legacy, {}, false, false);
    check(flex.surfaceVisible && !flex.extendedControls,
          "Flex retains its established FM surface");
    check(flex.accessModes == QStringList({QStringLiteral("off"),
                                           QStringLiteral("ctcss_tx")}),
          "Flex retains exactly Off and CTCSS TX");
    check(flex.duplexVisible && flex.reverseVisible,
          "Flex retains its existing duplex and REV controls");
    check(vfoFmRepeaterSurfaceVisible(FmRepeaterPresentation::Legacy),
          "the established VFO FM surface remains visible for Legacy backends");
    check(fmRepeaterDirectionLabels()
              == QStringList({QString::fromUtf8("\xe2\x88\x92"),
                              QStringLiteral("Simplex"), QStringLiteral("+"),
                              QStringLiteral("REV")}),
          "the shared panel retains exact direction labels and order");
    check(flexFmRepeaterTxOffset(QStringLiteral("up"), 0.6, false) == 0.6
              && flexFmRepeaterTxOffset(QStringLiteral("up"), 0.6, true) == -0.6
              && flexFmRepeaterTxOffset(QStringLiteral("down"), 0.6, false) == -0.6
              && flexFmRepeaterTxOffset(QStringLiteral("down"), 0.6, true) == 0.6
              && !flexFmRepeaterTxOffset(QStringLiteral("simplex"), 0.6, true),
          "the production Flex REV helper retains exact sign-flip behavior");

    const FmRepeaterUiPolicy ic9700 = fmRepeaterUiPolicy(
        FmRepeaterPresentation::Extended,
        {QStringLiteral("off"), QStringLiteral("dtcs_txrx")}, true, true);
    check(ic9700.surfaceVisible && ic9700.extendedControls,
          "evidenced IC-9700 profile exposes the extended surface");
    check(ic9700.duplexVisible && ic9700.reverseVisible,
          "evidenced IC-9700 profile exposes duplex and XFC");
    check(!vfoFmRepeaterSurfaceVisible(FmRepeaterPresentation::Extended),
          "the incomplete duplicate VFO surface is hidden for extended controls");

    const FmRepeaterUiPolicy duplexOnly = fmRepeaterUiPolicy(
        FmRepeaterPresentation::Extended, {}, true, false);
    check(duplexOnly.surfaceVisible && duplexOnly.duplexVisible
              && !duplexOnly.reverseVisible && duplexOnly.accessModes.isEmpty(),
          "each optional extended control follows its own declared capability");
    check(!fmRepeaterReverseEnabled(true, true, QStringLiteral("simplex")),
          "IC-9700 XFC is disabled while simplex is authoritative");
    check(fmRepeaterReverseEnabled(true, true, QStringLiteral("down"))
              && fmRepeaterReverseEnabled(true, true, QStringLiteral("up")),
          "IC-9700 XFC is offered for either repeater duplex direction");
    check(fmRepeaterReverseEnabled(false, true, QStringLiteral("simplex")),
          "the established Flex REV surface is unchanged");

    const FmRepeaterUiPolicy unattested = fmRepeaterUiPolicy(
        FmRepeaterPresentation::Hidden, {}, false, false);
    check(!unattested.surfaceVisible && !unattested.extendedControls
              && !unattested.duplexVisible && !unattested.reverseVisible,
          "unattested Icom controls are hidden, not disabled");
    check(!vfoFmRepeaterSurfaceVisible(FmRepeaterPresentation::Hidden),
          "the VFO surface is hidden for unattested models");

    return failures == 0 ? 0 : 1;
}
