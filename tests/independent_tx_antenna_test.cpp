// hamlib set_ant writes the receive antenna, and the TX antenna too unless the
// radio declares its TX output an independent choice (IndependentTxAntenna,
// #4954); a band-stack recall restores the TX antenna under the same rule. On an
// HL2 that choice is ANT or RF1, and neither a logger nor a recall may move it.
// Production CAT parser, recall and RadioModel with an injected Flex backend: no
// socket, no peer, nothing keyed.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/BandStackSettings.h"
#include "core/RigctlProtocol.h"

#include <QCoreApplication>
#include <QEvent>
#include <cstdio>
#include <memory>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void wire(RadioModel& radio, SliceModel* slice)
    { radio.wireSliceReceiveIntentsToBackend(slice); }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class Backend final : public FlexBackend {
public:
    bool independentTx{false};
    QStringList commands;
    Backend()
    {
        setModelProvider([] { return QStringLiteral("FLEX-6700"); });
        setSliceCommandSink([this](const QString& c) { commands << c; });
        setSetupCommandSink([](const QString&, const RadioSetupCompletion&) { return true; });
    }
    bool isConnected() const override { return true; }
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps = FlexBackend::capabilities();
        if (independentTx) {
            caps.independentTxAntenna = IndependentTxAntenna{};
        }
        return caps;
    }
};

void setAntAndRecall(bool independent)
{
    RadioModel radio;
    auto owned = std::make_unique<Backend>();
    owned->independentTx = independent;
    Backend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    PanadapterModel* pan = RadioSetupControlTestAccess::addPan(radio, QStringLiteral("0x40000000"));
    SliceModel* slice = RadioSetupControlTestAccess::addSlice(radio, 3);
    SliceDelta d;
    d.panId = pan->panId(); d.mode = QStringLiteral("USB"); d.frequency = 14.1;
    d.rxAntenna = QStringLiteral("ANT1"); d.txAntenna = QStringLiteral("ANT1");
    d.rxAntennaList = QStringList{"ANT1", "ANT2"};
    d.txAntennaList = QStringList{"ANT1", "ANT2"};
    slice->applyChanges(d);
    RadioModelSliceLifecycleTestAccess::wire(radio, slice);

    RigctlProtocol cat(&radio);
    cat.setSliceIndex(3);
    check(cat.handleLine("\\set_ant 2") == QLatin1String("RPRT 0\n"), "set_ant 2 is acknowledged");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(slice->rxAntenna() == QLatin1String("ANT2"), "set_ant moves the receive antenna");
    if (independent) {
        check(slice->txAntenna() == QLatin1String("ANT1"),
              "RT8: an independent TX output is left where the operator put it");
    } else {
        check(slice->txAntenna() == QLatin1String("ANT2"),
              "RT8: without the record the TX antenna follows, as before");
    }

    // RT9: a band-stack entry holding another TX antenna.
    SliceDelta back;
    back.rxAntenna = QStringLiteral("ANT1"); back.txAntenna = QStringLiteral("ANT1");
    slice->applyChanges(back);
    backend->commands.clear();
    BandStackEntry e;
    e.frequencyMhz = 14.1; e.mode = QStringLiteral("USB"); e.txAntenna = QStringLiteral("ANT2");
    radio.recallBandStack(slice, e, [](SliceModel*, double) {});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    const bool sentTxAnt = backend->commands.filter(QStringLiteral("txant=")).size() > 0;
    if (independent) {
        check(!sentTxAnt, "RT9: a recall does not move an independent TX output");
    } else {
        check(sentTxAnt, "RT9: without the record a recall restores the TX antenna, as before");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cat-set-ant-tx-route-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    setAntAndRecall(false);
    setAntAndRecall(true);
    std::printf("%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
