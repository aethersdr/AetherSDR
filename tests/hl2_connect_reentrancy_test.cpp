// What happens to a connect that arrives while the previous one's WDSP chains
// are still opening — and to that connect when the operator then disconnects.
//
// The connect stopped being one straight-line call: connectRadio() seeds its
// members, hands the channel opens to the I/O thread and returns, and
// finishDspSetup() picks the session back up an event-loop turn (or twenty
// seconds) later. That opened a window nothing could previously land in, and
// this test is about what lands in it.
//
// NO RADIO IS NEEDED. Everything under test happens before the wire matters:
// the DSP build runs against nothing, and the socket start at the end of
// finishDspSetup() binds locally whether or not anything answers. Each case is
// observed through dspSetupProgress/dspSetupFinished, which bracket exactly one
// build each.

#include "core/backends/hl2/Hl2Backend.h"

#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", condition ? " OK " : "FAIL", what);
    if (!condition)
        ++failures;
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

RadioConnectRequest request()
{
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    // A port nothing is on. The connect's unicast discovery probe times out
    // (400 ms) and the session comes up on the conservative defaults, which is
    // all this test needs — it never asserts anything about the wire.
    req.port = 1024;
    return req;
}

// Counts complete DSP builds. dspSetupFinished is emitted once per build on
// every exit from finishDspSetup(), including the superseded one.
struct BuildCounter {
    int finished = 0;
    explicit BuildCounter(Hl2Backend& backend)
    {
        QObject::connect(&backend, &Hl2Backend::dspSetupFinished, &backend,
                         [this] { ++finished; });
    }
};

// A disconnect during the opens must take the QUEUED connect with it.
//
// The defect: disconnectRadio() bumped the generation, so the in-flight build
// was correctly superseded and released — but the connect parked behind it in
// m_queuedConnect survived, and the supersede branch re-drove it. The radio came
// back up, wire and all, after the operator asked it to go down.
void disconnectCancelsTheQueuedConnect()
{
    TestSettingsProfile profile(QStringLiteral("hl2-reentrancy-disconnect"));
    Hl2Backend backend;
    BuildCounter builds(backend);

    backend.connectRadio(request());   // build 1 starts on the I/O thread
    backend.connectRadio(request());   // cannot be served inline — queued
    backend.disconnectRadio();         // and the operator leaves

    // finishDspSetup() runs on THIS thread, so nothing above can have completed
    // yet; the first build lands when the event loop turns.
    test::awaitDspBuild("hl2_connect_reentrancy_test",
                        [&] { return builds.finished >= 1; });
    check(builds.finished == 1, "the superseded build reported once");

    // A re-driven connect would start its own build here. Generous, because on
    // a cold FFTW cache the first build has already paid the planning and a
    // second one is fast — it would land well inside this window.
    spin(3000);
    check(builds.finished == 1,
          "a connect queued before the disconnect is NOT re-driven after it");
    check(!backend.isConnected(), "the backend stayed disconnected");
}

// The other half of the same guard: WITHOUT a disconnect, a connect that
// arrives mid-build is still owed. It is held rather than served inline
// (serving it inline would blocking-hop into the busy I/O thread, which is the
// GUI stall the split exists to remove), and finishDspSetup() re-drives it.
void aQueuedConnectIsStillHonoured()
{
    TestSettingsProfile profile(QStringLiteral("hl2-reentrancy-queued"));
    Hl2Backend backend;
    BuildCounter builds(backend);

    backend.connectRadio(request());
    backend.connectRadio(request());

    test::awaitDspBuild("hl2_connect_reentrancy_test",
                        [&] { return builds.finished >= 2; });
    check(builds.finished == 2,
          "the queued connect ran its own build once the first was released");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    disconnectCancelsTheQueuedConnect();
    aQueuedConnectIsStillHonoured();

    if (failures) {
        std::fprintf(stderr, "hl2_connect_reentrancy_test: %d check(s) failed\n",
                     failures);
        return 1;
    }
    std::fprintf(stderr, "hl2_connect_reentrancy_test: all checks passed\n");
    return 0;
}
