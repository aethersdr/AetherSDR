// #5775 / #5776 — the receiver letters the UI offers (RX applet slice tabs, CAT
// VFO targets) must follow the capacity the connected backend declares, on the
// edge where it declares it.
//
// #5775: the slice tabs were sized once, from RadioModel::infoChanged, which
// fires at the START of a connect. A Hermes-Lite 2 honestly reports one
// receiver until its link is up and only then its real ceiling (four on a
// 4-DDC board), announced on capabilitiesChanged — which nothing sized the tabs
// from. The tab row stayed at "no tabs", and receivers B/C/D had nowhere to
// appear.
//
// #5776: the CAT letters were sized from RadioModel::maxSlicesForModel(), the
// Flex model table. It has no row for a radio outside that family, so it
// answered its 2-slice default: A and B only.
//
// ReceiverSlotCount is now the one number both surfaces take. These checks pin
// it against a backend whose ceiling moves AFTER connect, which is the case
// both defects missed, and against a ceiling that falls below the receivers
// already running.
//
// The capability edge is driven by emitting RadioModel::capabilitiesChanged
// with the backend's current capabilities — what RadioModel's own relay does
// when a backend announces a revision (setupBackend(); a test-injected backend
// is not wired through that relay). Socket-free: nothing is opened or keyed.
//
// NOT covered here: MainWindow's wiring of countChanged into
// AppletPanel::setMaxSlices and the CAT applet. That wiring is two calls in a
// lambda; MainWindow cannot be built in a unit test.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/SliceDelta.h"
#include "models/RadioModel.h"
#include "models/ReceiverSlotCount.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QList>
#include <QSignalSpy>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// A backend that declares its own receiver ceiling, and can revise it.
class CeilingBackend final : public IRadioBackend {
public:
    bool connected{false};
    int ceiling{1};

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.family = QStringLiteral("ceiling-test");
        caps.maxSlices = ceiling;
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAudioGain(int, int) override {}
    void setSliceAudioMute(int, bool) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setPanBandwidth(const QString&, double) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    CeilingBackend* backend{nullptr};

    Fixture()
    {
        auto owned = std::make_unique<CeilingBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("ceiling-test"));
    }

    // What RadioModel publishes on a connection edge and on a backend's
    // capability revision.
    void publish()
    {
        emit radio.capabilitiesChanged(backend->connected, backend->capabilities());
    }

    void connect()
    {
        backend->connected = true;
        publish();
    }

    void revise(int ceiling)
    {
        backend->ceiling = ceiling;
        publish();
    }

    void addReceiver(int sliceId)
    {
        SliceDelta delta;
        delta.frequency = 14.074 + 0.01 * sliceId;
        radio.emitBackendSliceChangedForTest(sliceId, delta);
    }
};

void testCeilingRaisedAfterConnectReachesTheLetters()
{
    Fixture f;
    ReceiverSlotCount letters(&f.radio);
    QSignalSpy spy(&letters, &ReceiverSlotCount::countChanged);

    check("no letters are claimed for a radio that is not connected", letters.count() == 0);

    // The HL2 shape: connected, but the ceiling it can state at that edge is 1.
    f.connect();
    check("at the connect edge the letters follow the declared ceiling of 1",
          letters.count() == 1);

    // The link is up and the backend now knows its board: four receivers.
    f.revise(4);
    check("a ceiling revised to 4 after connect offers four letters (A-D)",
          letters.count() == 4);
    check("and the revision is announced, so the tabs and CAT applet can resize",
          !spy.isEmpty() && spy.constLast().at(0).toInt() == 4);

    // The number both surfaces take must not come from a model-name table. The
    // test backend's model string matches no row, which is exactly the HL2's
    // position in RadioModel::maxSlicesForModel().
    check("the count is the backend's declaration, not the model table's default",
          letters.count() == f.radio.maxSlices()
              && letters.count() != RadioModel::maxSlicesForModel(f.radio.model()));
}

void testAFallingCeilingDoesNotStrandARunningReceiver()
{
    Fixture f;
    ReceiverSlotCount letters(&f.radio);
    f.connect();
    f.revise(4);
    for (int id = 0; id < 4; ++id) {
        f.addReceiver(id);
    }
    check("four receivers are running", f.radio.slices().size() == 4);

    // The operator zooms out and the link budget carries only three receivers.
    // The fourth is still running until something removes it.
    f.revise(3);
    check("a ceiling that falls below the running receivers keeps a letter for each",
          letters.count() == 4);
}

void testTheFloorIsBySlotNotByCount()
{
    // The tab row is indexed by global slice id: a lone receiver in slot C
    // needs letters A-C, not one letter.
    SliceModel c(2);
    check("one receiver in slot C needs three letters under a ceiling of 1",
          ReceiverSlotCount::forCeiling(1, QList<SliceModel*>{&c}) == 3);
    check("and a ceiling above the occupied slots is the answer as declared",
          ReceiverSlotCount::forCeiling(4, QList<SliceModel*>{&c}) == 4);
}

void testDisconnectWithdrawsTheCount()
{
    Fixture f;
    ReceiverSlotCount letters(&f.radio);
    f.connect();
    f.revise(4);
    QSignalSpy spy(&letters, &ReceiverSlotCount::countChanged);

    f.backend->connected = false;
    f.publish();
    check("disconnecting withdraws the count", letters.count() == 0);
    check("and says so", !spy.isEmpty() && spy.constLast().at(0).toInt() == 0);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("receiver-slot-count"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testCeilingRaisedAfterConnectReachesTheLetters();
    testAFallingCeilingDoesNotStrandARunningReceiver();
    testTheFloorIsBySlotNotByCount();
    testDisconnectWithdrawsTheCount();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
