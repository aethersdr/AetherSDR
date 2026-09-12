// RadioCapabilities::widebandConverterView — the declaration, and the verb it
// names.
//
// WHAT THIS IS DEFENDING. The wideband bandscope window must not ask "is this
// an HL2". A family string test above the seam is what docs/HERMES.md's
// "For coding agents — keep bring-up inside the family backend" section
// forbids, and the alternative it names is a declared capability. So the window
// reads this record and invokes the namespace and verb the record carries,
// knowing nothing about who answered.
//
// That buys a new failure mode. If either string drifts from what
// Hl2Backend::invokeExtension() actually matches, the capability advertises a
// verb that answers "hl2: no extension verbs implemented" — and nothing would
// notice until an operator opened the window on a real radio. This test is the
// thing that notices.
//
// SOCKET-FREE. No bind, no peer, no radio, no event loop beyond what
// QCoreApplication needs to exist. The fake-radio fixture that used to cover
// Hl2Backend's connected paths is retired (tests/tests.cmake, the `#[==[`
// block), so the CONNECTED half of the declaration — that a live HL2 engages
// the record at all — is NOT asserted anywhere that runs. Said plainly rather
// than left to be discovered: what is proved here is the record's content and
// the verb's existence, not that a connected backend publishes it.

#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVariant>

#include <cstdio>

using namespace AetherSDR;

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

    // ---- 1 · the record describes the CONVERTER, not a display ----
    {
        const WidebandConverterView wide = hl2::widebandConverterViewRecord();
        check(wide.sampleRateHz == hl2::kAdcSampleRateHz,
              "the record carries the converter's own sample rate");
        check(wide.sampleRateHz == 76.8e6,
              "...which is hermeslite_core.v's CLK_FREQ — a DC..38.4 MHz span");
        check(wide.blockSamples == hl2::kEp4BlockSamples,
              "and the record length the gateware delivers");
        check(wide.blockSamples == 2048,
              "...which is the capture FIFO's depth, four datagrams' worth");
        check(!wide.frameNamespace.isEmpty() && !wide.frameVerb.isEmpty(),
              "and names a verb, which is what lets a consumer skip the family");
    }

    // ---- 2 · the namespace it names is one this backend declares ----
    //
    // extensionNamespaces is the handshake a client pre-checks before it issues
    // any extension call. A record naming a namespace outside that list would
    // advertise a verb the backend does not claim to answer.
    {
        hl2::Hl2Backend backend;
        const WidebandConverterView wide = hl2::widebandConverterViewRecord();
        check(backend.capabilities().extensionNamespaces.contains(wide.frameNamespace),
              "the record's namespace is one the backend declares it answers");
    }

    // ---- 3 · the VERB EXISTS: it is refused for a reason, not as unknown ----
    //
    // The load-bearing case. Hl2Backend's fallthrough answers any verb it does
    // not recognise with one specific sentence; a typo in either half of the
    // record lands there. Being told "not connected" instead proves the string
    // reached the branch that implements it.
    {
        hl2::Hl2Backend backend;   // never connected
        const WidebandConverterView wide = hl2::widebandConverterViewRecord();

        check(!backend.capabilities().widebandConverterView.has_value(),
              "a backend with no radio declares NO converter view — there is none to look at");

        QSignalSpy res(&backend, &IRadioBackend::extensionResult);
        QSignalSpy err(&backend, &IRadioBackend::extensionError);
        backend.invokeExtension(wide.frameNamespace, wide.frameVerb, 4242, QVariant{});

        check(res.count() == 0,
              "a frame is never answered locally: the radio has to be asked for a block");
        check(err.count() == 1, "and a request that cannot be served is ANSWERED");
        check(err.first().at(0).toULongLong() == 4242u,
              "carrying back the caller's own requestId");
        const QString reason = err.first().at(1).toString();
        check(reason != QStringLiteral("hl2: no extension verbs implemented"),
              "and NOT with the unknown-verb sentence — the record names a real verb");
        check(!reason.isEmpty(), "with a reason a status line can show");
    }

    // ---- 4 · requestId 0 is refused silently ----
    //
    // Every other verb in this namespace tolerates a fire-and-forget call. This
    // one cannot do anything useful with it: the whole point is 2048 samples
    // coming back, and there is nowhere to send them. Putting a block on the
    // wire and discarding it is the one behaviour that would be wrong.
    {
        hl2::Hl2Backend backend;
        const WidebandConverterView wide = hl2::widebandConverterViewRecord();
        QSignalSpy res(&backend, &IRadioBackend::extensionResult);
        QSignalSpy err(&backend, &IRadioBackend::extensionError);
        backend.invokeExtension(wide.frameNamespace, wide.frameVerb, 0, QVariant{});
        check(res.count() == 0 && err.count() == 0,
              "requestId 0 asks for a frame with nowhere to put it, and is dropped");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "wideband_converter_view_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
