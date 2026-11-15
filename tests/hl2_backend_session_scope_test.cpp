// What a new connect seeds and forgets, observed without a wire. boardMaxRx in
// the request skips the unicast discovery probe, and disconnectRadio() in the
// same turn supersedes the DSP build, so MetisClient::start() never runs and
// nothing binds. The WDSP opens still run; the test waits for them to finish.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/AppSettings.h"

#include "TestDspBuildWait.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// Seed a session and abandon it before its wire could start.
static void connectAndAbandon(Hl2Backend& b)
{
    QSignalSpy finished(&b, &Hl2Backend::dspSetupFinished);
    RadioConnectRequest req;
    req.host = QStringLiteral("192.0.2.1");   // TEST-NET-1; never contacted
    req.port = 1024;
    req.params.insert(QStringLiteral("boardMaxRx"), 4);
    b.connectRadio(req);
    b.disconnectRadio();
    check(test::awaitDspBuild("hl2_backend_session_scope_test",
                              [&] { return !finished.isEmpty(); }),
          "the superseded DSP build finishes");
    check(!b.isConnected(), "an abandoned connect never comes up");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-backend-session-scope-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    qRegisterMetaType<NotchDelta>();

    // The span the operator last chose is the one a connect comes up on.
    Hl2Settings::setSpanMhz(0.096);

    Hl2Backend b;
    QSignalSpy changed(&b, &IRadioBackend::notchChanged);
    QSignalSpy removed(&b, &IRadioBackend::notchRemoved);
    b.createNotch(7'041'000.0, 200.0);
    const int previous = changed.isEmpty() ? -1 : changed.last().at(0).toInt();
    check(previous > 0, "a notch placed before the connect gets an id");

    connectAndAbandon(b);
    check(b.currentOperatingState().sampleRateHz == 96'000,
          "a connect seeds the remembered span, not the 48 kHz default");

    // #4780: notches are session state, and their ids keep counting.
    changed.clear();
    NotchDelta move;
    move.centerHz = 7'042'000.0;
    b.setNotch(previous, move);
    b.removeNotch(previous);
    check(changed.isEmpty() && removed.isEmpty(),
          "#4780: the previous session's notch is gone after a connect");
    b.createNotch(7'050'000.0, 200.0);
    check(changed.count() == 1 && changed.last().at(0).toInt() > previous,
          "#4780: the new session never reissues an id the old one used");

    std::fprintf(stderr, "hl2_backend_session_scope_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
