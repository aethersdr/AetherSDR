// A backend that carries its own IQ (Hermes-Lite 2, KiwiSDR) has no
// PanadapterStream, so RadioModel::panStream() returns null. Five call sites in
// the RADE and DAX-bridge paths dereferenced it bare, which made activating
// either one a segfault rather than a decline -- the same crash already fixed
// once at the startDax() entry, still reachable by four paths behind it.
//
// Those sites now go through RadioModel's null-safe seam. This asserts the
// property the seam exists to guarantee: with no stream, the DAX verbs decline
// and return, and nothing dereferences null.

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

    // A default RadioModel is a Flex model and DOES own a PanadapterStream --
    // which is exactly why these call sites looked safe for years.
    check(model.panStream() != nullptr,
          "the Flex default has a PanadapterStream (why the bare deref looked safe)");

    // Switch to a family that carries its own IQ. connectToRadio() swaps the
    // backend before it touches the network, so an unroutable address is enough
    // to reach the state without needing hardware; the connection attempt itself
    // just fails later, asynchronously, and this test never waits for it.
    RadioInfo hl2;
    hl2.family = QStringLiteral("hl2");
    hl2.serial = QStringLiteral("00:00:00:00:00:00");
    hl2.address = QHostAddress(QStringLiteral("192.0.2.1"));   // TEST-NET-1, unroutable
    model.connectToRadio(hl2);

    check(model.panStream() == nullptr,
          "an HL2 backend has no PanadapterStream");

    // Each of these dereferenced null before the fix.
    check(!model.acquireDaxChannel(1, PanadapterStream::DaxConsumer::Rade),
          "acquire declines (RADE) instead of crashing");
    check(!model.acquireDaxChannel(3, PanadapterStream::DaxConsumer::Bridge),
          "acquire declines (DAX bridge) instead of crashing");

    // Release is a no-op rather than a decline: nothing was ever held, and a
    // teardown path must not care whether the acquire succeeded.
    model.releaseDaxChannel(1, PanadapterStream::DaxConsumer::Rade);
    model.releaseDaxChannel(3, PanadapterStream::DaxConsumer::Bridge);
    model.releaseAllDaxChannels(PanadapterStream::DaxConsumer::Bridge);

    // Range is the caller's business (the RADE path gates on 1..4); the seam's
    // one job is that a missing stream is not a crash.
    check(!model.acquireDaxChannel(0, PanadapterStream::DaxConsumer::Rade),
          "channel 0 declines with no stream");

    if (g_failures == 0)
        std::fprintf(stderr, "radiomodel_dax_null_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
