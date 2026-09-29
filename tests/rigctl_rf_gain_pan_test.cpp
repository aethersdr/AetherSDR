// #5774 — `L RF` / `l RF` over rigctl must drive and read the operator's RF
// Gain control, which is the PANADAPTER's.
//
// On every family the ANT panel's RF Gain slider, the controller wheel and the
// automation bridge's `pan rfgain` all go through RadioModel::setPanRfGainFor()
// and read PanadapterModel::rfGain(). rigctl alone wrote SliceModel::setRfGain,
// whose whole body is `slice set N rfgain=X` — wire text a backend without a
// command plane (the Hermes-Lite 2 among them) drops — and it answered RPRT 0
// regardless. `l RF` then read the slice's local copy of that write back, so a
// client saw its own number echoed and never the slider.
//
// A second defect sat in the scale: Hamlib RIG_LEVEL_RF is normalized 0.0-1.0,
// and it was treated as 0-100 %. The HL2 publishes its LNA as -12..+48 dB in
// 1 dB steps, so half of travel is +18 dB and the bottom is -12 dB, a value the
// percent mapping cannot express at all.
//
// Socket-free: an injected stub backend that records the RF gain it is handed,
// a panadapter materialised through the same seam signal a wire-less backend
// uses, and a slice attached to it. Nothing is opened and nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QString>

#include <cmath>
#include <cstdio>
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
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
