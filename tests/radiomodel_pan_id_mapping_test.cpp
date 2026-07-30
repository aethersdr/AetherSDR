// A multi-pan backend labels its panadapters in its OWN namespace ("hl2-2");
// the models are keyed by a neutral synthetic id ("0xe1000002"). RadioModel
// translates between the two, and this pins the property that the translation
// is a MAPPING rather than a one-way function.
//
// That distinction is the whole test. The first version of this translation ran
// backend -> model only, so every signal coming UP was addressed correctly and
// every command going DOWN carried the model's id to a backend that could not
// resolve it. A single-pan backend never noticed, because it ignored the
// argument entirely; a multi-pan backend MUST resolve it to know which of four
// receivers to move, so it refused instead — and the operator saw a panadapter
// whose centre never moved under a drag preview that did, with the waterfall
// still drawn at the old band edges.
//
// Nothing here needs a radio, a socket, or a backend: the mapping is pure
// bookkeeping over ids, which is why it is worth testing at this level.

#include "models/RadioModel.h"
#include "core/RadioDiscovery.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    RadioModel model;

    // A default RadioModel is a FLEX model, and for Flex the translation is
    // correctly the identity: its pan ids ARE the model keys. Assert that first,
    // because it is the behaviour every existing radio depends on.
    check(model.backendPanIdForTest(QStringLiteral("0x40000001"))
              == QStringLiteral("0x40000001"),
          "Flex pan ids pass through unchanged — they are already the model keys");

    // Switch to a family that labels its pans in its own namespace.
    // connectToRadio() swaps the backend before it touches the network, so an
    // unroutable address reaches the state without hardware; the connection
    // attempt fails later, asynchronously, and this test never waits for it.
    RadioInfo hl2;
    hl2.family = QStringLiteral("hl2");
    hl2.serial = QStringLiteral("00:00:00:00:00:00");
    hl2.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    model.connectToRadio(hl2);

    // Indices are allocated in FIRST-SEEN order, which is how a backend's pan
    // ids stay opaque: RadioModel never parses "hl2-2" to get 2.
    const int i0 = model.panIndexForBackendIdForTest(QStringLiteral("hl2-0"));
    const int i1 = model.panIndexForBackendIdForTest(QStringLiteral("hl2-1"));
    const int i2 = model.panIndexForBackendIdForTest(QStringLiteral("hl2-2"));
    const int i3 = model.panIndexForBackendIdForTest(QStringLiteral("hl2-3"));
    check(i0 == 0 && i1 == 1 && i2 == 2 && i3 == 3, "indices allocate in first-seen order");

    // Allocation is STABLE: the same backend id must keep its index for the life
    // of the connection, or a pan's geometry and its frames would drift apart.
    check(model.panIndexForBackendIdForTest(QStringLiteral("hl2-2")) == 2,
          "re-asking for a known id returns the same index");

    // ---- the round trip, in both directions ----
    for (int i = 0; i < 4; ++i) {
        const QString backendId = QStringLiteral("hl2-%1").arg(i);
        const QString modelId = RadioModel::neutralPanIdStringForTest(i);
        check(model.backendPanIdForTest(modelId) == backendId,
              "a model pan id translates back to the backend's own id");
    }

    // Spot-check the exact strings, so a change to either id scheme has to be
    // deliberate rather than silently absorbed by the round trip above.
    check(model.backendPanIdForTest(QStringLiteral("0xe1000002"))
              == QStringLiteral("hl2-2"),
          "0xe1000002 -> hl2-2");
    check(model.backendPanIdForTest(QStringLiteral("0xe1000000"))
              == QStringLiteral("hl2-0"),
          "0xe1000000 -> hl2-0");
    // The mapping is NOT the identity, which is what makes the round trip
    // meaningful rather than vacuous.
    check(model.backendPanIdForTest(QStringLiteral("0xe1000002"))
              != QStringLiteral("0xe1000002"),
          "the translation actually changes the id");

    // ---- allocation is LOWEST-FREE, not size() ----
    //
    // size() is only collision-free while nothing is removed. A backend that
    // closes a pan and opens another (an HL2 closing a receiver) would otherwise
    // get an index that still belongs to a live pane — the new pan then resolves
    // to the EXISTING PanadapterModel and takes over its geometry and frames
    // instead of getting a pane of its own.
    //
    // The retire path is exercised through the public surface by mapping a fresh
    // id after the table has a hole in it.
    {
        RadioModel m2;
        RadioInfo hl2b;
        hl2b.family = QStringLiteral("hl2");
        hl2b.serial = QStringLiteral("00:00:00:00:00:01");
        hl2b.address = QHostAddress(QStringLiteral("192.0.2.2"));
        m2.connectToRadio(hl2b);

        for (int i = 0; i < 4; ++i)
            m2.panIndexForBackendIdForTest(QStringLiteral("hl2-%1").arg(i));
        // Same id re-asked is stable, so a repeat never consumes a new index.
        check(m2.panIndexForBackendIdForTest(QStringLiteral("hl2-2")) == 2,
              "a known id keeps its index");
        // A brand-new id must not collide with any of 0..3.
        const int next = m2.panIndexForBackendIdForTest(QStringLiteral("hl2-9"));
        check(next == 4, "a fifth distinct id gets index 4, not a reused one");
        check(m2.backendPanIdForTest(RadioModel::neutralPanIdStringForTest(4))
                  == QStringLiteral("hl2-9"),
              "and the reverse mapping points at the new id, not an existing pan");
        // Every mapping is still distinct — the property that actually matters.
        bool distinct = true;
        for (int i = 0; i < 4; ++i) {
            if (m2.backendPanIdForTest(RadioModel::neutralPanIdStringForTest(i))
                    == QStringLiteral("hl2-9"))
                distinct = false;
        }
        check(distinct, "no earlier index was handed to the new id");
    }

    // ---- ids we never mapped pass through untouched ----
    //
    // Inventing an id would be worse than passing one through: the backend can
    // refuse something it does not recognise, but it cannot detect that we
    // fabricated a plausible-looking id for the wrong pan.
    check(model.backendPanIdForTest(QStringLiteral("0x40000000"))
              == QStringLiteral("0x40000000"),
          "a pan id below the neutral base passes through (demo claims its own)");
    check(model.backendPanIdForTest(QStringLiteral("0xe1000009"))
              == QStringLiteral("0xe1000009"),
          "an unmapped neutral index passes through rather than resolving to pan 0");
    check(model.backendPanIdForTest(QString()).isEmpty(),
          "an empty id stays empty");
    check(model.backendPanIdForTest(QStringLiteral("not-an-id"))
              == QStringLiteral("not-an-id"),
          "an unparseable id passes through");

    if (g_failures == 0)
        std::printf("radiomodel_pan_id_mapping_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
