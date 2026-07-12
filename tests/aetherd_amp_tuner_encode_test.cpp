// aetherd #4092 / #4094 — ENCODE side. FlexBackend::invokeExtension translates
// the neutral amp/tuner intents (operate/bypass/autotune) into the SmartSDR
// relay wire, advertises the "flex" extension namespace, and honors the async
// contract (an awaited call always gets exactly one extensionResult/Error).
// Companion to the decode tests (aetherd_amp_decode_test / aetherd_tuner_decode_test).

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/RadioCapabilities.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

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

// The vendor arg RadioModel supplies: the Flex object handle + the on/off value.
static QVariant arg(const QString& handle, bool on)
{
    QVariantMap m;
    m[QStringLiteral("handle")] = handle;
    m[QStringLiteral("on")] = on;
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- capabilities advertises the flex namespace now that verbs route ----
    {
        FlexBackend b;
        check(b.capabilities().extensionNamespaces.contains(QStringLiteral("flex")),
              "capabilities() does not advertise the flex extension namespace");
    }

    // ---- each verb translates to the exact SmartSDR relay string ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });

        b.invokeExtension("flex", "amp.operate", 0, arg("0x1000", true));
        b.invokeExtension("flex", "amp.operate", 0, arg("0x1000", false));
        b.invokeExtension("flex", "tuner.operate", 0, arg("0x2000", true));
        b.invokeExtension("flex", "tuner.operate", 0, arg("0x2000", false));
        b.invokeExtension("flex", "tuner.bypass", 0, arg("0x2000", true));
        b.invokeExtension("flex", "tuner.autotune", 0, arg("0x2000", false));

        check(sent.size() == 6, "expected 6 wire commands");
        check(sent.value(0) == "amplifier set 0x1000 operate=1", "amp.operate=1 wire");
        check(sent.value(1) == "amplifier set 0x1000 operate=0", "amp.operate=0 wire");
        check(sent.value(2) == "tgxl set handle=0x2000 mode=1", "tuner.operate=1 wire (mode=)");
        check(sent.value(3) == "tgxl set handle=0x2000 mode=0", "tuner.operate=0 wire");
        check(sent.value(4) == "tgxl set handle=0x2000 bypass=1", "tuner.bypass wire");
        check(sent.value(5) == "tgxl autotune handle=0x2000", "tuner.autotune wire");
    }

    // ---- async contract: an awaited call gets exactly one reply ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        QSignalSpy okSpy(&b, &FlexBackend::extensionResult);
        QSignalSpy errSpy(&b, &FlexBackend::extensionError);

        // success + requestId != 0 → one extensionResult carrying the id, wire sent
        b.invokeExtension("flex", "amp.operate", 42, arg("0x1000", true));
        check(sent.size() == 1, "awaited success still sends the wire command");
        check(okSpy.count() == 1 && errSpy.count() == 0,
              "awaited success emits exactly one extensionResult");
        check(okSpy.takeFirst().at(0).toULongLong() == 42u,
              "extensionResult correlates the requestId");

        // fire-and-forget (requestId 0) success → no reply signal at all
        b.invokeExtension("flex", "amp.operate", 0, arg("0x1000", true));
        check(okSpy.count() == 0 && errSpy.count() == 0,
              "requestId 0 success emits no reply");
    }

    // ---- error paths: unknown ns / verb / missing handle → error, no wire ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        QSignalSpy errSpy(&b, &FlexBackend::extensionError);

        b.invokeExtension("bogus", "amp.operate", 7, arg("0x1000", true));   // unknown ns
        b.invokeExtension("flex", "amp.bogus", 8, arg("0x1000", true));       // unknown verb
        b.invokeExtension("flex", "amp.operate", 9, arg("", true));           // no handle
        b.invokeExtension("flex", "tuner.autotune", 10, arg("", false));      // no handle
        check(sent.isEmpty(), "error paths send no wire command");
        check(errSpy.count() == 4, "each awaited error emits exactly one extensionError");

        // the same failures, fire-and-forget → silent (no hang, no stray reply)
        errSpy.clear();
        b.invokeExtension("flex", "amp.bogus", 0, arg("0x1000", true));
        b.invokeExtension("flex", "amp.operate", 0, arg("", true));
        check(errSpy.count() == 0, "requestId 0 errors stay silent");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "aetherd_amp_tuner_encode_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
