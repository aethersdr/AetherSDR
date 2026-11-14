// A receive-only HL2 backend builds no transmit chain on connect.
//
// aetherd constructs the HL2 receive only (setHostOffersTransmit(false)), and
// that decision is taken once, in the constructor, so a transmit modulator
// opened at connect could never be keyed. The control case is the
// transmit-capable backend, which must still build it.
//
// Socket-free, with hl2_gain_restore_test's harness: boardMaxRx skips the
// unicast discovery socket and no event loop is pumped, so finishDspSetup()
// never runs and MetisClient never binds. The build itself runs on the I/O
// thread; a blocking call queued behind it reads the result there.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2TxDsp.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <cstdio>

namespace AetherSDR::hl2 {
struct Hl2RxOnlyTxChainTestAccess {
    // Queued behind the connect's build on the same thread, so it answers
    // only once that build has run.
    static bool txChainConfigured(Hl2Backend& backend)
    {
        bool configured = false;
        QMetaObject::invokeMethod(backend.m_txDsp, [&backend, &configured] {
            configured = backend.m_txDsp->isConfigured();
        }, Qt::BlockingQueuedConnection);
        return configured;
    }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::Hl2RxOnlyTxChainTestAccess;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what);
    if (!condition) {
        ++failures;
    }
}

struct ConnectResult {
    bool canTransmit = true;
    bool txConfigured = true;
    int setupTotal = -1;
};

ConnectResult connectOnce(bool hostOffersTransmit)
{
    Hl2Backend::setHostOffersTransmit(hostOffersTransmit);
    ConnectResult out;
    {
        Hl2Backend backend;
        out.canTransmit = backend.capabilities().canTransmit;
        // beginDspSetup() announces its step count synchronously, on this thread.
        QObject::connect(&backend, &Hl2Backend::dspSetupProgress, &backend,
                         [&out](const QString&, int, int total) {
            if (out.setupTotal < 0) {
                out.setupTotal = total;
            }
        });
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");
        request.serial = QStringLiteral("AA:BB:CC:DD:EE:07");
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        backend.connectRadio(request);
        out.txConfigured = Hl2RxOnlyTxChainTestAccess::txChainConfigured(backend);
        backend.disconnectRadio();
    }
    Hl2Backend::setHostOffersTransmit(true);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-rx-only-tx-chain"));
    // The automation bridge's TX gate would close transmit on its own; this
    // test is about the host's declaration only.
    qunsetenv("AETHER_AUTOMATION");
    QCoreApplication app(argc, argv);

    const ConnectResult tx = connectOnce(true);
    check(tx.canTransmit, "control: the transmit-capable backend declares canTransmit");
    check(tx.txConfigured, "control: its connect builds the transmit chain");

    const ConnectResult rx = connectOnce(false);
    check(!rx.canTransmit, "the receive-only backend declares canTransmit false");
    check(!rx.txConfigured, "its connect builds no transmit chain");
    check(tx.setupTotal >= 1 && rx.setupTotal >= 1,
          "both connects announced their DSP setup steps");
    check(rx.setupTotal == tx.setupTotal - (AETHER_HL2_TX_TXA ? 1 : 0),
          "the receive-only setup counts no transmit step");

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
