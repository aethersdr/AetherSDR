// #5594 item 3: the panadapter and slice capacity a Flex declares for itself.
//
// A radio states its capacity outright in the discovery packet. Observed on a
// FLEX-8600 running 4.2.20.41343, passively (the datagram is an unsolicited
// broadcast; nothing was connected):
//
//   max_panadapters=4  available_panadapters=4  max_slices=4  available_slices=4
//
// CAPACITY AND AVAILABILITY ARE DIFFERENT KEYS and this is the whole point of
// the change. `max_*` is what the hardware and licence allow and does not move;
// `available_*` — and the live `slices=`/`panadapters=` status — are the FREE
// counts, which fall as any client opens objects. FlexLib keeps all four apart
// (Discovery.cs:141/154/247/260, copied separately at API.cs:186-189).
//
// An earlier attempt derived capacity as (open objects + free ones) off the
// status plane. It failed three ways, all of which this route removes rather
// than fixes: the bounding helper was seeded at its own ceiling and returned it
// unconditionally; the open count came from a container holding only our own
// objects, so Multi-Flex undercounted; and `sub radio all` precedes `sub pan
// all`, so the first status arrives before any inventory exists at all. See
// #5603. Reading the declared capacity needs no inventory and no ordering.
//
// This pins the MODEL half — precedence, independence, fallback and radio swap.
// The parser half (that max_* is read and the adjacent available_* is not) lives
// in radio_discovery_test, which already has friend access to the parser.
//
// Socket-free: RadioModel is driven through connectToRadio() with a synthetic
// RadioInfo; nothing binds, listens or connects.

#include "core/RadioDiscovery.h"
#include "models/ModelCapabilities.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- the declared capacity beats the model table ----
    {
        // A FLEX-6700 is 8 in the FlexLib-sourced table. This one declares 3 —
        // the shape a reduced licence produces, and a value the table can never
        // express. Every assertion here fails if the table is consulted first.
        RadioModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.maxPanadapters = 3;
        info.maxSlices = 3;
        m.connectToRadio(info);

        check(capabilitiesFor(QStringLiteral("FLEX-6700")).maxSlices == 8,
              "the model table would have said 8");
        check(m.maxPanadapters() == 3,
              "the declared panadapter capacity wins over the model table");
        check(m.maxSlices() == 3,
              "the declared slice capacity wins over the model table");
    }

    // ---- pan and slice capacity are independent ----
    {
        // Nothing guarantees the two stay equal; the old code assumed they did.
        RadioModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.maxSlices = 8;
        info.maxPanadapters = 2;
        m.connectToRadio(info);
        check(m.maxSlices() == 8 && m.maxPanadapters() == 2,
              "a radio may declare different slice and panadapter capacities");
    }

    // ---- no declaration falls back to the table ----
    {
        RadioModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");   // maxSlices/maxPanadapters stay 0
        m.connectToRadio(info);
        check(m.maxPanadapters() == 8 && m.maxSlices() == 8,
              "a radio that declares nothing falls back to the model table");
    }

    // ---- a radio swap does not inherit the previous radio's capacity ----
    {
        RadioModel m;
        RadioInfo big;
        big.model = QStringLiteral("FLEX-6700");
        big.maxPanadapters = 8;
        big.maxSlices = 8;
        m.connectToRadio(big);
        check(m.maxPanadapters() == 8, "first radio declares 8");

        // Connecting by IP builds a RadioInfo with no discovery keys at all.
        // The previous radio's 8 must not survive into a 2-panadapter radio.
        RadioInfo byIp;
        byIp.model = QStringLiteral("FLEX-6400");
        m.connectToRadio(byIp);
        check(m.maxPanadapters() == capabilitiesFor(QStringLiteral("FLEX-6400")).maxSlices,
              "a connect that declares nothing falls back to the new radio's "
              "table rather than inheriting the previous radio's capacity");
    }

    if (g_failures == 0)
        std::printf("radio_capacity_declaration_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
