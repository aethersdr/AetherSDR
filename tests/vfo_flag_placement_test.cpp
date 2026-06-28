#include "gui/VfoWidget.h"

#include <cstdio>

using AetherSDR::VfoWidget;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

void expectDir(const char* name, VfoWidget::FlagDir actual, VfoWidget::FlagDir expected)
{
    report(name, actual == expected);
}

} // namespace

int main()
{
    constexpr int kSpectrumWidth = 1000;
    constexpr int kPanelWidth = 200;

    report("USB defaults to left",
           VfoWidget::defaultFlagOnLeftForMode(QStringLiteral("USB")));
    report("LSB defaults to right",
           !VfoWidget::defaultFlagOnLeftForMode(QStringLiteral("LSB")));
    report("CWL defaults to right",
           !VfoWidget::defaultFlagOnLeftForMode(QStringLiteral("CWL")));
    report("DIGL defaults to right",
           !VfoWidget::defaultFlagOnLeftForMode(QStringLiteral("DIGL")));

    expectDir("default-left stays left in middle",
              VfoWidget::autoDirectionForSingleFlag(
                  500, kPanelWidth, kSpectrumWidth, true, true),
              VfoWidget::ForceLeft);
    expectDir("default-left flips right before left pan-follow guard",
              VfoWidget::autoDirectionForSingleFlag(
                  240, kPanelWidth, kSpectrumWidth, true, true),
              VfoWidget::ForceRight);
    expectDir("default-left flips right at left pan-follow guard",
              VfoWidget::autoDirectionForSingleFlag(
                  250, kPanelWidth, kSpectrumWidth, true, true),
              VfoWidget::ForceRight);
    expectDir("default-left holds right through hysteresis",
              VfoWidget::autoDirectionForSingleFlag(
                  260, kPanelWidth, kSpectrumWidth, true, false),
              VfoWidget::ForceRight);
    expectDir("default-left returns left after hysteresis",
              VfoWidget::autoDirectionForSingleFlag(
                  280, kPanelWidth, kSpectrumWidth, true, false),
              VfoWidget::ForceLeft);

    expectDir("default-right stays right in middle",
              VfoWidget::autoDirectionForSingleFlag(
                  500, kPanelWidth, kSpectrumWidth, false, false),
              VfoWidget::ForceRight);
    expectDir("default-right flips left before right pan-follow guard",
              VfoWidget::autoDirectionForSingleFlag(
                  760, kPanelWidth, kSpectrumWidth, false, false),
              VfoWidget::ForceLeft);
    expectDir("default-right flips left at right pan-follow guard",
              VfoWidget::autoDirectionForSingleFlag(
                  750, kPanelWidth, kSpectrumWidth, false, false),
              VfoWidget::ForceLeft);
    expectDir("default-right holds left through hysteresis",
              VfoWidget::autoDirectionForSingleFlag(
                  740, kPanelWidth, kSpectrumWidth, false, true),
              VfoWidget::ForceLeft);
    expectDir("default-right returns right after hysteresis",
              VfoWidget::autoDirectionForSingleFlag(
                  720, kPanelWidth, kSpectrumWidth, false, true),
              VfoWidget::ForceRight);

    expectDir("two-vfo leftmost flips right at left edge",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  0, 2, 240, 0, 760, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceRight);
    expectDir("two-vfo leftmost flips right at pan-follow guard",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  0, 2, 250, 0, 760, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceRight);
    expectDir("two-vfo leftmost holds right through hysteresis",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  0, 2, 260, 0, 760, kPanelWidth, kSpectrumWidth, false),
              VfoWidget::ForceRight);
    expectDir("two-vfo leftmost returns left after hysteresis",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  0, 2, 280, 0, 760, kPanelWidth, kSpectrumWidth, false),
              VfoWidget::ForceLeft);
    expectDir("two-vfo rightmost flips left at right edge",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 2, 760, 240, 0, kPanelWidth, kSpectrumWidth, false),
              VfoWidget::ForceLeft);
    expectDir("two-vfo rightmost flips left at pan-follow guard",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 2, 750, 240, 0, kPanelWidth, kSpectrumWidth, false),
              VfoWidget::ForceLeft);
    expectDir("two-vfo rightmost holds left through hysteresis",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 2, 740, 240, 0, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceLeft);
    expectDir("two-vfo rightmost returns right after hysteresis",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 2, 720, 240, 0, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceRight);

    expectDir("three-vfo first flips right at left edge",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  0, 3, 240, 0, 500, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceRight);
    expectDir("three-vfo last flips left at right edge",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  2, 3, 760, 500, 0, kPanelWidth, kSpectrumWidth, false),
              VfoWidget::ForceLeft);
    expectDir("three-vfo interior uses larger left gap",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 3, 500, 300, 620, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceLeft);
    expectDir("three-vfo interior uses larger right gap",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  1, 3, 500, 380, 800, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::ForceRight);
    expectDir("invalid deconflicted index falls back to auto",
              VfoWidget::autoDirectionForDeconflictedFlag(
                  -1, 2, 0, 0, 0, kPanelWidth, kSpectrumWidth, true),
              VfoWidget::Auto);

    expectDir("invalid geometry preserves default left",
              VfoWidget::autoDirectionForSingleFlag(0, 0, 0, true, false),
              VfoWidget::ForceLeft);
    expectDir("invalid geometry preserves default right",
              VfoWidget::autoDirectionForSingleFlag(0, 0, 0, false, true),
              VfoWidget::ForceRight);

    const VfoWidget::FlagPlacement forcedAtLeftEdge =
        VfoWidget::placementForMarker(10, 3, 200, 80, 1000,
                                      VfoWidget::ForceLeft, true);
    report("force-left geometry flips right at left edge",
           forcedAtLeftEdge.rect.x() == 10 && !forcedAtLeftEdge.onLeft);

    const VfoWidget::FlagPlacement lockedAtLeftEdge =
        VfoWidget::placementForMarker(10, 3, 200, 80, 1000,
                                      VfoWidget::LockLeft, true);
    report("lock-left geometry stays left at left edge",
           lockedAtLeftEdge.rect.x() == -190 && lockedAtLeftEdge.onLeft);

    report("diversity index 0 orders before index 1",
           VfoWidget::diversityPairOrderKey(false, false, 0, 5)
               < VfoWidget::diversityPairOrderKey(false, false, 1, 4));
    report("diversity parent orders before child without index",
           VfoWidget::diversityPairOrderKey(true, false, -1, 5)
               < VfoWidget::diversityPairOrderKey(false, true, -1, 4));
    report("diversity unknown role orders by stable slice id",
           VfoWidget::diversityPairOrderKey(false, false, -1, 3)
               < VfoWidget::diversityPairOrderKey(false, false, -1, 4));

    std::printf("%s\n", g_failures == 0 ? "All tests passed." : "Test failures.");
    return g_failures == 0 ? 0 : 1;
}
