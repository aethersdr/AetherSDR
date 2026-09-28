// #5637 §1: a TransmitModel control whose value already crossed the
// IRadioBackend seam must not ALSO be reported as dropped.
//
// TransmitModel emits two things for RF power, mic level and the TX passband:
// a typed intent (rfPowerCommandIssued / micLevelCommandIssued /
// txFilterCommandIssued) that RadioModel hands to the backend, and the legacy
// Flex wire text through commandReady. On a backend with no command plane the
// wire text reached RadioModel::sendCmd, which logged "no command plane for
// this backend, dropping transmit set rfpower=N" and emitted commandDropped —
// while the backend had just applied the value. The report on #5637 was aimed
// at that line, and the one-shot "nothing was sent to the radio" notice it
// raises was consumed by a control that works.
//
// The drop notice is deliberate (#5263: it is how dead controls on non-Flex
// radios are found), so this test pins BOTH halves:
//   1. a routed verb reaches the backend and raises no commandDropped;
//   2. an unrouted verb (VOX here, which this backend does not implement) and a
//      routed verb on a backend that does not declare the capability behind it
//      still raise commandDropped — the alarm is narrowed, not silenced.
//
// Socket-free: an injected backend records the seam calls. No radio, no peer.

#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QList<int> txPowers;
    QList<int> micGains;
    QList<QPair<int, int>> txFilters;
    QList<int> cwPitches;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setTxPower(int percent) override { txPowers << percent; }
    void setMicGain(int level) override { micGains << level; }
    void setTxFilter(int lowHz, int highHz) override { txFilters << qMakePair(lowHz, highHz); }
    void setCwPitch(int hz) override { cwPitches << hz; }
    // setVox() is deliberately NOT overridden: this backend has no VOX, so the
    // Flex text for it really does reach nothing.
};

// The capability set of a host-modulating transmitter with no command plane —
// the Hermes-Lite 2's answers to the questions the gate asks.
RadioCapabilities hostModulatingTransmitter()
{
    RadioCapabilities c;
    c.family = QStringLiteral("hl2");
    c.canTransmit = true;
    c.hostModulates = true;
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine};
    c.hasTxFilterControls = true;
    return c;
}

struct Fixture {
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    QStringList dropped;

    explicit Fixture(const RadioCapabilities& caps)
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        backend->caps = caps;
        radio.setBackendForTest(std::move(owned), caps.family);
        QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                         [this](const QString& cmd) { dropped << cmd; });
    }

    bool droppedStartingWith(const QString& prefix) const
    {
        for (const QString& cmd : dropped) {
            if (cmd.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
};
} // namespace

static void premiseHasNoCommandPlane()
{
    Fixture f(hostModulatingTransmitter());
    check(!f.radio.hasCommandPlane(),
          "premise: the injected non-Flex backend has no command plane");
}

static void rfPowerReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setRfPower(90);
    check(f.backend->txPowers == QList<int>{90},
          "rfpower: setTxPower(90) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set rfpower=")),
          "rfpower: no commandDropped for a value the backend applied");
}

static void micLevelReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setMicLevel(42);
    check(f.backend->micGains == QList<int>{42},
          "miclevel: setMicGain(42) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set miclevel=")),
          "miclevel: no commandDropped for a value the backend applied");
}

static void txFilterReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setTxFilter(200, 2800);
    check(f.backend->txFilters.size() == 1
              && f.backend->txFilters.first() == qMakePair(200, 2800),
          "filter: setTxFilter(200, 2800) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set filter_low=")),
          "filter: no commandDropped for a passband the backend applied");
}

static void cwPitchReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwPitch(700);
    check(f.backend->cwPitches == QList<int>{700},
          "cw pitch: setCwPitch(700) reached the host-modulating backend once");
    check(!f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "cw pitch: no commandDropped for a pitch the backend applied");
}

// The negative control that keeps the first four honest: the same backend,
// the same model, a verb nothing behind the seam implements. If the fix had
// gated the whole commandReady forward, this is what would go quiet.
static void unroutedVerbStillRaisesDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setVoxEnable(true);
    check(f.droppedStartingWith(QStringLiteral("transmit set vox_enable=")),
          "vox: an unrouted verb still raises commandDropped");
}

// Routed on the seam is not enough: the backend must also declare the
// capability that says the setter does something. An RX-only host-DSP backend
// (the ANAN today: drive ownership declared, canTransmit false, setTxPower not
// implemented) must keep its alarm.
static void undeclaredCapabilityKeepsDropNotice()
{
    RadioCapabilities caps = hostModulatingTransmitter();
    caps.canTransmit = false;
    caps.hasTxFilterControls = false;
    caps.hostModulates = false;
    Fixture f(caps);
    f.radio.transmitModel().setRfPower(80);
    check(f.droppedStartingWith(QStringLiteral("transmit set rfpower=")),
          "receive-only backend: rfpower still raises commandDropped");
    f.radio.transmitModel().setMicLevel(30);
    check(f.droppedStartingWith(QStringLiteral("transmit set miclevel=")),
          "receive-only backend: miclevel still raises commandDropped");
    f.radio.transmitModel().setTxFilter(300, 2700);
    check(f.droppedStartingWith(QStringLiteral("transmit set filter_low=")),
          "no TX filter controls declared: the passband still raises commandDropped");
    f.radio.transmitModel().setCwPitch(650);
    check(f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "no host CW demod or radio keyer: cw pitch still raises commandDropped");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("transmit-seam-drop-notice"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    premiseHasNoCommandPlane();
    rfPowerReachesSeamWithoutDropNotice();
    micLevelReachesSeamWithoutDropNotice();
    txFilterReachesSeamWithoutDropNotice();
    cwPitchReachesSeamWithoutDropNotice();
    unroutedVerbStillRaisesDropNotice();
    undeclaredCapabilityKeepsDropNotice();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
