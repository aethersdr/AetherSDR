// The fifth path.
//
// radiomodel_dax_null_test fixed four bare m_panStream-> derefs behind the DAX
// verbs. The panRangeChanged handler in setupBackend() was a fifth, and it is
// worse than the DAX ones: DAX needs a user to activate something, whereas this
// one fires by itself the moment a backend reports its dBm range -- which for an
// Icom is during connect, from inside the CI-V frame handler.
//
// A backend that decodes its own scope (Icom CI-V, HL2, KiwiSDR) never gets a
// PanadapterStream: it is assigned only on the Flex and Sim paths. So the very
// first panRangeChanged from such a backend dereferenced null on the GUI thread.
// Observed as an instant, repeatable crash on connecting an IC-9700 -- four
// minidumps, all 0xC0000005 reading 0x30, all reaching PanadapterStream::
// setDbmRange from this lambda.
//
// This asserts the property directly: with no stream, delivering a pan range
// updates the model and does not dereference null.

#include "models/RadioModel.h"
#include "core/RadioDiscovery.h"

#include <QCoreApplication>
#include <QSignalSpy>

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

    // As in the DAX test: the Flex default DOES own a stream, which is why the
    // bare deref survived review.
    check(model.panStream() != nullptr,
          "the Flex default has a PanadapterStream (why the bare deref looked safe)");

    // A self-decoding backend. HL2 stands in for the Icom here -- same property
    // (no PanadapterStream), and it needs no radio to reach the state.
    RadioInfo hl2;
    hl2.family = QStringLiteral("hl2");
    hl2.serial = QStringLiteral("00:00:00:00:00:00");
    hl2.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    model.connectToRadio(hl2);

    check(model.panStream() == nullptr,
          "a self-decoding backend has no PanadapterStream");

    auto* backend = model.backend();
    check(backend != nullptr, "the backend swap happened");
    if (!backend) return 1;

    // A pan has to EXIST for this to test anything. resolveBackendPan() is a
    // lookup, not a create, so on a bare model it returns null and the handler
    // early-outs before the deref -- a test written without this would pass
    // against the unfixed code while proving nothing.
    // The backend's OWN pan id, in its own namespace -- exactly what an Icom
    // puts on the wire. Neutral indices are allocated in first-seen order, so
    // this id is unknown until the first signal names it. Do not substitute a
    // model-side id: it would resolve to a different pan (or none) and stop
    // exercising the path.
    const QString panId = QStringLiteral("hl2-0");

    // Geometry first, which is what MATERIALISES the pane -- createPanadapter()
    // on a self-decoding backend asks the backend, not the model. This is also
    // the real connect ordering: an Icom reports its geometry before its range.
    emit backend->panCenterBandwidthChanged(panId, 144.300, 0.5);
    check(!model.panadapters().isEmpty(), "a pan exists to report a range against");
    if (model.panadapters().isEmpty()) return 1;

    QSignalSpy levels(&model, &RadioModel::panadapterLevelChanged);

    // THE CRASH, REPRODUCED: before the guard this dereferenced null here.
    emit backend->panRangeChanged(panId, -140.0, -60.0);

    check(levels.count() == 1,
          "the range was delivered (the handler ran; it did not early-out)");

    // And the range must actually reach the model -- the guard skips only the
    // stream's copy, which a backend with no stream has no decoder to receive.
    if (levels.count() == 1) {
        const auto args = levels.takeFirst();
        check(qFuzzyCompare(args.at(0).toDouble(), -140.0), "min dBm reached the model");
        check(qFuzzyCompare(args.at(1).toDouble(), -60.0),  "max dBm reached the model");
    }

    // Idempotent: a second identical report is a no-op in the model, and still
    // must not crash.
    emit backend->panRangeChanged(panId, -140.0, -60.0);

    if (g_failures == 0)
        std::fprintf(stderr, "radiomodel_pan_range_null_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
