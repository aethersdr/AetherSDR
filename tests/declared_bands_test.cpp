// Unit tests for parseDeclaredBands() — the Principle-VII boundary validation
// behind the radio-declared bands= key. Locks in: allow-list against BandDefs
// (unknown names dropped), dedup, case-fold, whitespace tolerance, and the
// empty/absent -> empty-list real-Flex path.

#include "models/DeclaredBands.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok)
        ++g_failed;
}

bool eq(const QStringList& got, const QStringList& want)
{
    return got == want;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Absent / empty -> empty list (real Flex radios never send the key; the
    // band UI must be unchanged, which relies on this being empty).
    report("empty string -> empty list",
           parseDeclaredBands(QString()).isEmpty());
    report("blank/commas-only -> empty list",
           parseDeclaredBands(QStringLiteral(" , ,")).isEmpty());

    // The happy path: a real gateway declaration.
    report("2m,440,23cm -> [2m,440,23cm]",
           eq(parseDeclaredBands(QStringLiteral("2m,440,23cm")),
              {QStringLiteral("2m"), QStringLiteral("440"), QStringLiteral("23cm")}));

    // Unknown names dropped (allow-list is BandDefs only) — the core
    // Principle-VII guarantee: junk can't reach the band UI.
    report("unknown name dropped (2m,junk,440 -> [2m,440])",
           eq(parseDeclaredBands(QStringLiteral("2m,junk,440")),
              {QStringLiteral("2m"), QStringLiteral("440")}));
    report("all-unknown -> empty list",
           parseDeclaredBands(QStringLiteral("999cm,banana,-1")).isEmpty());

    // LF/MF (2200m / 630m) are non-declarable per #4027's non-goals even
    // though they are kBands entries — a gateway can't render them as
    // hardware-band buttons (#4191, follow-up #1).
    report("LF/MF dropped (2200m,630m -> empty list)",
           parseDeclaredBands(QStringLiteral("2200m,630m")).isEmpty());
    report("LF/MF dropped, HF kept (2200m,40m,630m -> [40m])",
           eq(parseDeclaredBands(QStringLiteral("2200m,40m,630m")),
              {QStringLiteral("40m")}));

    // Dedup.
    report("dedup (440,440 -> [440])",
           eq(parseDeclaredBands(QStringLiteral("440,440")),
              {QStringLiteral("440")}));

    // Case-fold to the canonical BandDefs spelling.
    report("case-fold (2M,23CM -> [2m,23cm])",
           eq(parseDeclaredBands(QStringLiteral("2M,23CM")),
              {QStringLiteral("2m"), QStringLiteral("23cm")}));

    // Whitespace around tokens is tolerated.
    report("whitespace trimmed ( 2m , 440 -> [2m,440])",
           eq(parseDeclaredBands(QStringLiteral(" 2m , 440 ")),
              {QStringLiteral("2m"), QStringLiteral("440")}));

    // Output preserves input order; dedup keeps the first occurrence, so a
    // mixed/duplicated input yields a clean set.
    report("dedup keeps set, drops repeats (2m,440,2m -> [2m,440])",
           eq(parseDeclaredBands(QStringLiteral("2m,440,2m")),
              {QStringLiteral("2m"), QStringLiteral("440")}));

    if (g_failed == 0) {
        std::printf("\nAll %d declared-bands tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d declared-bands tests failed.\n", g_failed, g_total);
    return 1;
}
