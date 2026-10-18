// Shipped IARU Region 1 and SSA (Sweden) band-plan rows match their charts,
// including the optional per-segment `usage` annotation. (#4667)
//
// The IARU R1 HF chart (effective 16 Oct 2020) has two columns per row:
// preferred mode and usage. Before #4667 the plan JSON had only `label`, so
// every row reading "All modes" + "Digimodes" was transcribed as digi-only
// and painted narrow-band red. #4610 reported one such row (80 m 3600-3620)
// and #4635 fixed exactly that one; six more survived because nothing in the
// suite read a shipped plan. This test reads them through BandPlanManager
// itself, so the parser and the resource are checked together.
//
// Deliberately NOT asserted: whether these rows are voice-detected.
// isVoiceSegmentLabel() keys the S-History voice mask off `label`, and no
// all-modes row in either plan matches a voice token today. Pinning that
// would freeze a separate defect. The test asserts only that a corrected row
// is treated the same as the plan's other all-modes rows.

#include "core/VoiceSignalDetector.h"
#include "models/BandPlanManager.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;
using Segment = BandPlanManager::Segment;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const QString& label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", qPrintable(label));
    if (!ok)
        ++g_failed;
}

const Segment* findSegment(const QVector<Segment>& segs, double low, double high)
{
    for (const auto& s : segs) {
        if (std::abs(s.lowMhz - low) < 1e-6 && std::abs(s.highMhz - high) < 1e-6)
            return &s;
    }
    return nullptr;
}

struct Expected {
    double low;
    double high;
    const char* label;
    const char* color;
    const char* usage;
};

const char* const kR1Unattended =
    "Digimodes, automatically controlled data stations (unattended)";
const char* const kSsaDigiAuto =
    "Digimoder och automatiska obevakade digimoder";

// IARU R1 HF chart, effective 16 Oct 2020.
const Expected kRegion1[] = {
    {1.840,  1.843,  "ALL",  "#ff8000", "Digimodes"},
    {3.600,  3.620,  "ALL",  "#ff8000", kR1Unattended},
    {7.050,  7.053,  "ALL",  "#ff8000", kR1Unattended},
    {7.053,  7.060,  "ALL",  "#ff8000", "Digimodes"},
    {14.101, 14.112, "ALL",  "#ff8000", kR1Unattended},
    {18.111, 18.120, "ALL",  "#ff8000", kR1Unattended},
    {21.110, 21.120, "DIGI", "#c03030",
     "All modes except SSB. Digimodes, automatically controlled data stations (unattended)"},
    {24.931, 24.940, "ALL",  "#ff8000", kR1Unattended},
    {29.200, 29.300, "ALL",  "#ff8000", kR1Unattended},
};

// SSA Bandplan Kortvåg v2.61 — only rows where it agrees with the R1 chart.
const Expected kSweden[] = {
    {1.840,  1.843,  "ALLA", "#ff8000", "Digimoder"},
    {3.600,  3.620,  "ALLA", "#ff8000", "Digimoder, automatiska obevakade digimoder"},
    {7.050,  7.060,  "ALLA", "#ff8000",
     "Digimoder; automatiska obevakade digimoder inom 7050–7053 kHz"},
    {14.101, 14.112, "ALLA", "#ff8000", kSsaDigiAuto},
    {18.111, 18.120, "ALLA", "#ff8000", "Digimoder"},
    {21.110, 21.120, "DIGI", "#c03030", "Alla moder utom SSB. Digimoder och automatiska obevakade digimoder"},
    {24.931, 24.940, "ALLA", "#ff8000", "Automatiska obevakade digimoder"},
    {29.200, 29.300, "ALLA", "#ff8000", kSsaDigiAuto},
};

template <size_t N>
void checkPlan(BandPlanManager& mgr, const QString& planName, const QString& allLabel,
               const Expected (&rows)[N])
{
    mgr.setActivePlan(planName);
    report(QStringLiteral("%1 is active").arg(planName), mgr.activePlanName() == planName);
    const auto& segs = mgr.segments();
    report(QStringLiteral("%1 has segments").arg(planName), !segs.isEmpty());

    // Reference voice-mask result for this plan's ordinary all-modes rows.
    // Corrected rows must match it, whatever it is.
    int allRows = 0;
    bool allVoice = false;
    for (const auto& s : segs) {
        if (s.label == allLabel && s.usage.isEmpty()) {
            if (allRows++ == 0)
                allVoice = isVoiceSegmentLabel(s.label);
        }
    }
    report(QStringLiteral("%1 has un-annotated %2 rows to compare against (got %3)")
               .arg(planName, allLabel).arg(allRows),
           allRows > 0);

    for (const auto& e : rows) {
        const QString where = QStringLiteral("%1 %2-%3")
                                  .arg(planName)
                                  .arg(e.low, 0, 'f', 3)
                                  .arg(e.high, 0, 'f', 3);
        const Segment* s = findSegment(segs, e.low, e.high);
        report(where + QStringLiteral(" exists"), s != nullptr);
        if (!s)
            continue;
        report(where + QStringLiteral(" label is \"%1\" (got \"%2\")").arg(e.label, s->label),
               s->label == QString::fromUtf8(e.label));
        report(where + QStringLiteral(" color is %1 (got %2)").arg(e.color, s->color.name()),
               s->color == QColor(QString::fromLatin1(e.color)));
        report(where + QStringLiteral(" usage matches the chart (got \"%1\")").arg(s->usage),
               s->usage == QString::fromUtf8(e.usage));
        if (s->label == allLabel) {
            report(where + QStringLiteral(" voice mask matches the plan's other %1 rows")
                       .arg(allLabel),
                   isVoiceSegmentLabel(s->label) == allVoice);
        }
    }

    // The defect shape itself: a red `DIGI` row in these plans must be the
    // 21.110 exception, which carries its restriction in `usage`.
    for (const auto& s : segs) {
        if (s.label != QStringLiteral("DIGI"))
            continue;
        const bool exception = std::abs(s.lowMhz - 21.110) < 1e-6;
        report(QStringLiteral("%1 DIGI row %2 is the 21.110 not-SSB exception")
                   .arg(planName)
                   .arg(s.lowMhz, 0, 'f', 3),
               exception && s.usage.contains(QStringLiteral("SSB")));
    }
}

} // namespace

int main(int argc, char** argv)
{
    // loadPlans() reads the saved plan name from AppSettings. Keep that off
    // the developer's real profile.
    QTemporaryDir profile;
    if (!profile.isValid()) {
        std::printf("[FAIL] cannot create a private settings dir\n");
        return 1;
    }
    qputenv("AETHER_SETTINGS_DIR", profile.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);

    BandPlanManager mgr;
    mgr.loadPlans();

    checkPlan(mgr, QStringLiteral("IARU Region 1"), QStringLiteral("ALL"), kRegion1);
    checkPlan(mgr, QStringLiteral("SSA (Sweden)"), QStringLiteral("ALLA"), kSweden);

    // `usage` is optional: a plan that never sets it parses to empty usage.
    mgr.setActivePlan(QStringLiteral("ARRL (US)"));
    bool anyUsage = false;
    for (const auto& s : mgr.segments())
        anyUsage = anyUsage || !s.usage.isEmpty();
    report(QStringLiteral("ARRL (US) loads with no usage annotations"),
           !mgr.segments().isEmpty() && !anyUsage);

    if (g_failed == 0) {
        std::printf("\nAll %d band-plan usage tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d band-plan usage tests failed.\n", g_failed, g_total);
    return 1;
}
