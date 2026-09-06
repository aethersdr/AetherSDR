// Manual J16 filter-board table — pure encode/lookup, standalone (Qt::Core
// only; no sockets, no aethercore, no other Qt module). Pins the "seed from
// automatic" contract and the per-band/per-direction lookup, alongside the
// existing frequency-to-filter-byte coverage in hl2_metis_protocol_test.

#include "core/backends/hl2/Hl2FilterBoard.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- lookup on an empty table ----
    {
        ManualFilterTable empty;
        check(manualFilterByte(empty, QStringLiteral("40m"), FilterDirection::Receive) == kOcNone,
              "an unconfigured band releases every relay (kOcNone), not a guess");
        check(manualFilterByte(empty, QStringLiteral("40m"), FilterDirection::Transmit) == kOcNone,
              "same for transmit");
    }

    // ---- independent RX/TX masks ----
    {
        ManualFilterTable table;
        table.insert(QStringLiteral("20m"), ManualFilterBand{kOcLpf30_20, kOcHpfAmBc | kOcLpf30_20});
        check(manualFilterByte(table, QStringLiteral("20m"), FilterDirection::Receive) == kOcLpf30_20,
              "receive mask read back independently of transmit");
        check(manualFilterByte(table, QStringLiteral("20m"), FilterDirection::Transmit)
                  == (kOcHpfAmBc | kOcLpf30_20),
              "transmit mask read back independently of receive");
    }

    // ---- masking to 7 bits ----
    {
        ManualFilterTable table;
        table.insert(QStringLiteral("15m"), ManualFilterBand{0xFF, 0x00});
        check(manualFilterByte(table, QStringLiteral("15m"), FilterDirection::Receive) == 0x7F,
              "a stray bit 7 is masked off, matching ocFilterByteForHz()'s own bit-7 invariant");
    }

    // ---- seeding from the automatic mapping ----
    {
        const ManualFilterTable seeded = seedManualFilterTableFromAutomatic();
        check(seeded.size() == kFilterBoardBandCount,
              "seeding produces exactly the ten filter-board bands, no more, no fewer");
        for (const char* band : kFilterBoardBands) {
            check(seeded.contains(QString::fromLatin1(band)),
                  "every filter-board band is present after seeding");
        }
        // 40m and 60m share one physical filter (kOcLpf60_40) on the stock
        // N2ADR board — seeding from the same automatic function must
        // reproduce that grouping, not invent independent values.
        const auto oc40 = manualFilterByte(seeded, QStringLiteral("40m"), FilterDirection::Receive);
        const auto oc60 = manualFilterByte(seeded, QStringLiteral("60m"), FilterDirection::Receive);
        check(oc40 == oc60, "seeded 40m/60m share the stock board's grouped filter");
        check((oc40 & kOcLpf60_40) != 0, "the grouped filter is actually the 60/40m LPF");
        // RX and TX seed identically — the automatic path does not
        // distinguish them outside a spanned+keyed override.
        const auto oc40tx = manualFilterByte(seeded, QStringLiteral("40m"), FilterDirection::Transmit);
        check(oc40 == oc40tx, "seeding assigns the same value to RX and TX");
        // 20m and 17m must NOT be forced equal — they sit in different
        // automatic groups (30/20m vs 17/15m) and manual mode must be able to
        // diverge from the stock grouping, so seeding must not collapse them.
        const auto oc20 = manualFilterByte(seeded, QStringLiteral("20m"), FilterDirection::Receive);
        const auto oc17 = manualFilterByte(seeded, QStringLiteral("17m"), FilterDirection::Receive);
        check(oc20 != oc17, "20m and 17m seed to different groups' filters");
    }

    // ---- representative frequencies land in the band ocFilterByteForHz() expects ----
    {
        check(ocFilterByteForHz(representativeHzForFilterBoardBand(QStringLiteral("160m")))
                  == kOcLpf160,
              "160m representative frequency encodes the 160m filter");
        check(ocFilterByteForHz(representativeHzForFilterBoardBand(QStringLiteral("10m")))
                  == (kOcHpfAmBc | kOcLpf12_10),
              "10m representative frequency encodes the 12/10m filter");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_filter_board_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
