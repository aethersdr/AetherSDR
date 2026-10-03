// #5774: rigctl `L RF` / `l RF` on a radio without a command plane (the HL2)
// drive and read the pan's RF gain, Hamlib's 0.0-1.0 spread across the range
// the backend published, and refuse (RPRT -11) with nothing to address or no
// published range. A radio with a command plane (Flex) keeps main's slice
// setter and wire text. Socket-free: injected stub backend or the real Flex
// backend built without dialling; nothing is opened and nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/sim/SimBackend.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QStringList>
#include <QString>

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

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

const QString kBackendPanId = QStringLiteral("stub-pan-0");

// A backend whose RF gain lives behind the seam, as the HL2's AD9866 LNA does.
// It records what it was handed; it does not answer, so the test controls the
// pan's reported value itself.
class StubBackend final : public IRadioBackend {
public:
    bool connected{false};
    std::optional<int> lastRfGain;
    QString lastRfGainPanId;
    int rfGainWrites{0};

    RadioCapabilities capabilities() const override { return {}; }
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
    void setPanRfGain(const QString& panId, int gainDb) override
    {
        lastRfGain = gainDb;
        lastRfGainPanId = panId;
        ++rfGainWrites;
    }
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};
    PanadapterModel* pan{nullptr};

    // withPan=false leaves the slice attached to nothing, which on a backend
    // with no command plane means there is no RF gain control to address.
    // publishRange=false leaves the pan as it is before the backend's
    // panRfGainInfoChanged lands: PanadapterModel's own defaults, which are a
    // Flex's shape (-8..32 step 8) and not any backend's statement.
    explicit Fixture(bool withPan = true, bool publishRange = true)
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("rfgain-test"));

        SliceDelta slice;
        slice.frequency = 14.074;
        if (withPan) {
            // The geometry signal is how a wire-less backend's pan comes into
            // being; the slice delta then names it in the backend's namespace
            // and RadioModel re-addresses it to the model key.
            emit backend->panCenterBandwidthChanged(kBackendPanId, 14.1, 0.192);
            slice.panId = kBackendPanId;
        }
        radio.emitBackendSliceChangedForTest(0, slice);
        if (!radio.slice(0)) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
        if (withPan) {
            pan = radio.panadapter(radio.slice(0)->panId());
            if (!pan) {
                std::fprintf(stderr, "FATAL: the slice is not attached to a panadapter\n");
                std::exit(2);
            }
            // The Hermes-Lite 2 LNA range, as Hl2Backend publishes it through
            // panRfGainInfoChanged (kLnaGainMinDb / kLnaGainMaxDb, 1 dB step).
            if (publishRange) {
                pan->setRfGainInfo(-12, 48, 1);
            }
        }
        backend->connected = true;
    }
};

bool nearly(double a, double b)
{
    return std::fabs(a - b) < 1e-4;
}

// `L RF <v>` is queued onto the model's thread, as every rigctl setter is.
QString setRf(RigctlProtocol& port, const QString& value)
{
    const QString reply = port.handleLine(QStringLiteral("L RF ") + value).trimmed();
    QCoreApplication::processEvents();
    return reply;
}

std::optional<double> getRf(RigctlProtocol& port)
{
    const QString reply = port.handleLine(QStringLiteral("l RF")).trimmed();
    if (reply.startsWith(QLatin1String("RPRT"))) {
        return std::nullopt;
    }
    bool ok = false;
    const double value = reply.toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

void testHalfTravelLandsMidRangeOnThePan()
{
    Fixture f;
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    const QString reply = setRf(port, QStringLiteral("0.5"));
    check("L RF 0.5 is acknowledged", reply == QLatin1String("RPRT 0"));
    check("L RF reaches the backend's pan RF gain verb", f.backend->rfGainWrites == 1);
    check("L RF 0.5 on a -12..+48 dB pan asks for +18 dB",
          f.backend->lastRfGain.has_value() && *f.backend->lastRfGain == 18);
    check("and it is addressed at the slice's own pan, in the backend's namespace",
          f.backend->lastRfGainPanId == kBackendPanId);
}

void testBottomOfTravelIsTheBottomOfTheRange()
{
    Fixture f;
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    setRf(port, QStringLiteral("0.0"));
    check("L RF 0.0 asks for -12 dB, which a percent scale cannot express",
          f.backend->lastRfGain.has_value() && *f.backend->lastRfGain == -12);
    setRf(port, QStringLiteral("1.0"));
    check("L RF 1.0 asks for +48 dB",
          f.backend->lastRfGain.has_value() && *f.backend->lastRfGain == 48);
    setRf(port, QStringLiteral("7"));
    check("an out-of-range L RF clamps to the top rather than overshooting",
          f.backend->lastRfGain.has_value() && *f.backend->lastRfGain == 48);
}

void testReadBackIsThePansValue()
{
    Fixture f;
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    // What the backend echoes back after taking the value — exactly the edge
    // RadioModel drives from panRfGainChanged.
    setRf(port, QStringLiteral("0.5"));
    f.pan->setRfGain(f.backend->lastRfGain.value_or(0));
    const auto afterSet = getRf(port);
    check("l RF round-trips an L RF 0.5 as 0.5",
          afterSet.has_value() && nearly(*afterSet, 0.5));

    // THE CONTROL: the operator moves the slider, not the CAT client. A reader
    // that echoes the client's own last write (the slice's local copy) cannot
    // see this; one that reads the pan must.
    f.pan->setRfGain(30);
    const auto slider = getRf(port);
    check("l RF reports a gain the operator set on the slider (+30 dB -> 0.7)",
          slider.has_value() && nearly(*slider, 0.7));

    f.pan->setRfGain(-12);
    const auto bottom = getRf(port);
    check("l RF reports -12 dB as 0.0",
          bottom.has_value() && nearly(*bottom, 0.0));
}

void testNothingToAddressIsNotAcknowledged()
{
    Fixture f(/*withPan=*/false);
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    // No pan and no command plane: the slice setter's wire text has nowhere to
    // go. Answering RPRT 0 is what made the no-op look like success.
    const QString reply = setRf(port, QStringLiteral("0.5"));
    check("L RF with no RF gain control to address answers RIG_ENAVAIL",
          reply == QLatin1String("RPRT -11"));
    check("l RF with no RF gain control to address does not invent a reading",
          !getRf(port).has_value());
    check("and nothing was sent to the backend", f.backend->rfGainWrites == 0);
}

void testNoPublishedRangeIsNotAGuess()
{
    Fixture f(/*withPan=*/true, /*publishRange=*/false);
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);

    // The pan exists from its geometry signal on; its range arrives later, on
    // panRfGainInfoChanged. In between, the model holds Flex-shaped defaults,
    // and `L RF 0.5` scaled against them would send +16 dB to an HL2 whose
    // half-travel is +18. Refuse until the backend has said what its range is.
    check("L RF before the backend publishes its range answers RIG_ENAVAIL",
          setRf(port, QStringLiteral("0.5")) == QLatin1String("RPRT -11"));
    check("l RF before the backend publishes its range does not invent a reading",
          !getRf(port).has_value());
    check("and nothing was sent to the backend", f.backend->rfGainWrites == 0);

    // The range lands, as RadioModel relays panRfGainInfoChanged.
    f.pan->setRfGainInfo(-12, 48, 1);
    check("once the range is published L RF is taken",
          setRf(port, QStringLiteral("0.5")) == QLatin1String("RPRT 0"));
    check("and lands at half travel of the published range (+18 dB)",
          f.backend->lastRfGain == 18);
    f.pan->setRfGain(18);
    const auto after = getRf(port);
    check("and l RF reads it back", after.has_value() && nearly(*after, 0.5));
}


// THE COMMAND-PLANE PATH (maintainer ruling on #6013): a radio WITH a command
// plane (Flex) keeps main's `L RF` / `l RF` byte for byte, because third-party
// CAT clients bind to it. The in-process Demo has a Flex command plane and plays
// the radio's part with no socket dialled. Its pan is given a published Flex
// range, so a mutation that routes this radio to the pan sends `display pan ...
// rfgain=` instead of the slice text and fails here rather than merely refusing.
bool waitFor(const std::function<bool()>& done, int ms = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

void testCommandPlaneKeepsTheSliceSetter()
{
    RadioModel radio;
    RadioInfo demo;
    demo.name = QStringLiteral("FLEX-6700");
    demo.model = SimBackend::demoModelName();
    demo.serial = SimBackend::demoSerial();
    demo.family = SimBackend::familyName();
    demo.address = QHostAddress(QHostAddress::LocalHost);  // never dialled
    demo.port = 4992;
    radio.connectToRadio(demo);
    const bool up = waitFor([&radio] {
        return radio.isConnected() && radio.slice(0) && !radio.slice(0)->panId().isEmpty()
            && radio.panadapter(radio.slice(0)->panId());
    });
    check("the Demo connects with a command plane and slice 0 on a pan",
          up && radio.hasCommandPlane());
    if (!up) {
        return;
    }
    SliceModel* slice = radio.slice(0);
    PanadapterModel* pan = radio.panadapter(slice->panId());
    pan->setRfGainInfo(-8, 32, 8);
    pan->setRfGain(-8);

    QStringList wire;
    QObject::connect(slice, &SliceModel::commandReady, [&wire](const QString& cmd) {
        wire << cmd;
    });
    RigctlProtocol port(&radio);
    port.setSliceIndex(0);

    check("L RF 0.5 on a Flex is acknowledged as on main",
          setRf(port, QStringLiteral("0.5")) == QLatin1String("RPRT 0"));
    check("and sends main's wire text, `slice set 0 rfgain=50`",
          wire == QStringList{QStringLiteral("slice set 0 rfgain=50")});
    check("l RF on a Flex reads the slice's rfgain as on main (0.5), not the pan's",
          port.handleLine(QStringLiteral("l RF")).trimmed() == QLatin1String("0.5"));
    wire.clear();
    setRf(port, QStringLiteral("7"));
    check("an out-of-range L RF on a Flex clamps to `slice set 0 rfgain=100`",
          wire == QStringList{QStringLiteral("slice set 0 rfgain=100")});
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rigctl-rf-gain-pan"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testHalfTravelLandsMidRangeOnThePan();
    testBottomOfTravelIsTheBottomOfTheRange();
    testReadBackIsThePansValue();
    testNothingToAddressIsNotAcknowledged();
    testNoPublishedRangeIsNotAGuess();
    testCommandPlaneKeepsTheSliceSetter();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
