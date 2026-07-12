// aetherd #4092 / #4094 / #4198 — ENCODE side. FlexBackend::invokeExtension
// translates the neutral amp/tuner intents (operate/bypass/autotune) into the
// SmartSDR relay wire. Per #4198 the device object handle is sourced from the
// backend's OWN decode-side state (captured in decodeAmplifierStatus /
// decodeTunerStatus) — the intent no longer carries a Flex handle. Also covers
// the "flex" namespace advertisement and the async reply contract.
// Companion to the decode tests (aetherd_amp_decode_test / aetherd_tuner_decode_test).

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/RadioCapabilities.h"

#include <QCoreApplication>
#include <QMap>
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

// The vendor arg now carries only the on/off value; the handle comes from decode.
static QVariant argOn(bool v)
{
    QVariantMap m;
    m[QStringLiteral("on")] = v;
    return m;
}

// Seed the backend's decode-side handle state the way a live status would — this
// is what invokeExtension now resolves the wire handle from.
static void seedAmp(FlexBackend& b, const QString& handle)
{
    b.decodeAmplifierStatus(handle, QStringLiteral("PowerGeniusXL"), {}, /*removed=*/false);
}
static void seedTuner(FlexBackend& b, const QString& handle)
{
    b.decodeTunerStatus(handle, {});
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

    // ---- each verb translates to the exact wire string, using the DECODED handle ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        seedAmp(b, "0x1000");     // handle now lives in the backend, not the arg
        seedTuner(b, "0x2000");

        b.invokeExtension("flex", "amp.operate", 0, argOn(true));
        b.invokeExtension("flex", "amp.operate", 0, argOn(false));
        b.invokeExtension("flex", "tuner.operate", 0, argOn(true));
        b.invokeExtension("flex", "tuner.operate", 0, argOn(false));
        b.invokeExtension("flex", "tuner.bypass", 0, argOn(true));
        b.invokeExtension("flex", "tuner.autotune", 0);   // no arg needed for autotune

        check(sent.size() == 6, "expected 6 wire commands");
        check(sent.value(0) == "amplifier set 0x1000 operate=1", "amp.operate=1 wire");
        check(sent.value(1) == "amplifier set 0x1000 operate=0", "amp.operate=0 wire");
        check(sent.value(2) == "tgxl set handle=0x2000 mode=1", "tuner.operate=1 wire (mode=)");
        check(sent.value(3) == "tgxl set handle=0x2000 mode=0", "tuner.operate=0 wire");
        check(sent.value(4) == "tgxl set handle=0x2000 bypass=1", "tuner.bypass wire");
        check(sent.value(5) == "tgxl autotune handle=0x2000", "tuner.autotune wire");
    }

    // ---- #4198: handle is resolved from decode state; re-decode updates it;
    //      removal (incl. TGXL removal on the amplifier wire) clears it ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        QSignalSpy errSpy(&b, &FlexBackend::extensionError);

        // no decode yet → no cached handle → error, no wire
        b.invokeExtension("flex", "amp.operate", 1, argOn(true));
        check(sent.isEmpty() && errSpy.count() == 1,
              "amp.operate before any decode → error, no wire");

        // after decode, the intent uses the decoded handle
        seedAmp(b, "0x1000");
        b.invokeExtension("flex", "amp.operate", 0, argOn(true));
        check(sent.value(0) == "amplifier set 0x1000 operate=1",
              "amp.operate after decode uses the decoded handle");

        // a later status with a new handle re-points the encode
        seedAmp(b, "0x1abc");
        b.invokeExtension("flex", "amp.operate", 0, argOn(false));
        check(sent.value(1) == "amplifier set 0x1abc operate=0",
              "re-decode updates the cached amp handle");

        // removal clears it → back to error
        errSpy.clear();
        sent.clear();
        b.decodeAmplifierStatus("0x1abc", QString(), {}, /*removed=*/true);
        b.invokeExtension("flex", "amp.operate", 2, argOn(true));
        check(sent.isEmpty() && errSpy.count() == 1,
              "after removal the amp handle is cleared → error");

        // TGXL: its removal arrives on the amplifier-removed wire (RadioModel
        // routes it to decodeAmplifierStatus) — must clear the tuner handle too.
        seedTuner(b, "0x2000");
        errSpy.clear();
        sent.clear();
        b.invokeExtension("flex", "tuner.bypass", 0, argOn(true));
        check(sent.value(0) == "tgxl set handle=0x2000 bypass=1",
              "tuner intent uses the decoded tuner handle");
        b.decodeAmplifierStatus("0x2000", QString(), {}, /*removed=*/true);   // TGXL removed
        sent.clear();
        b.invokeExtension("flex", "tuner.bypass", 3, argOn(true));
        check(sent.isEmpty() && errSpy.count() == 1,
              "TGXL removal on the amp wire clears the tuner handle → error");

        // clearExtensionHandles() (the disconnect/reset path) drops both at once
        seedAmp(b, "0x1000");
        seedTuner(b, "0x2000");
        b.clearExtensionHandles();
        errSpy.clear();
        sent.clear();
        b.invokeExtension("flex", "amp.operate", 4, argOn(true));
        b.invokeExtension("flex", "tuner.operate", 5, argOn(true));
        check(sent.isEmpty() && errSpy.count() == 2,
              "clearExtensionHandles() drops both cached handles → errors, no wire");
    }

    // ---- async contract: an awaited call gets exactly one reply ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        seedAmp(b, "0x1000");
        QSignalSpy okSpy(&b, &FlexBackend::extensionResult);
        QSignalSpy errSpy(&b, &FlexBackend::extensionError);

        // success + requestId != 0 → one extensionResult carrying the id, wire sent
        b.invokeExtension("flex", "amp.operate", 42, argOn(true));
        check(sent.size() == 1, "awaited success still sends the wire command");
        check(okSpy.count() == 1 && errSpy.count() == 0,
              "awaited success emits exactly one extensionResult");
        check(okSpy.takeFirst().at(0).toULongLong() == 42u,
              "extensionResult correlates the requestId");

        // fire-and-forget (requestId 0) success → no reply signal at all
        b.invokeExtension("flex", "amp.operate", 0, argOn(true));
        check(okSpy.count() == 0 && errSpy.count() == 0,
              "requestId 0 success emits no reply");
    }

    // ---- error paths: unknown ns / verb / missing handle → error, no wire ----
    {
        FlexBackend b;
        QStringList sent;
        b.setCommandSink([&](const QString& c) { sent << c; });
        QSignalSpy errSpy(&b, &FlexBackend::extensionError);
        seedAmp(b, "0x1000");   // amp has a handle, so ns/verb errors aren't masked

        b.invokeExtension("bogus", "amp.operate", 7, argOn(true));   // unknown ns
        b.invokeExtension("flex", "amp.bogus", 8, argOn(true));       // unknown verb
        b.invokeExtension("flex", "tuner.operate", 9, argOn(true));   // no tuner decode → no handle
        check(sent.isEmpty(), "error paths send no wire command");
        check(errSpy.count() == 3, "each awaited error emits exactly one extensionError");

        // the same failures, fire-and-forget → silent (no hang, no stray reply)
        errSpy.clear();
        b.invokeExtension("flex", "amp.bogus", 0, argOn(true));
        b.invokeExtension("flex", "tuner.operate", 0, argOn(true));
        check(errSpy.count() == 0, "requestId 0 errors stay silent");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "aetherd_amp_tuner_encode_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
