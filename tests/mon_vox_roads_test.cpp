// MON and VOX on the CAT and TCI roads. A radio that declares no transmit
// monitor (RadioCapabilities::txMonitorControl) or no VOX (voxControl) refuses
// the rigctl `L`/`U` verbs with RPRT -11 and the TCI mon_enable / mon_volume
// sets without touching the model, TCI reporting the state the model really
// has. A radio that declares them, and no radio at all, behave as before.
// Socket-free: an injected stub backend; nothing is opened and nothing is keyed.
// The shortcut and MIDI roads live in MainWindow and are not covered here.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/TciProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QString>

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

class StubBackend final : public IRadioBackend {
public:
    bool connected{false};
    bool declareMon{false};
    bool declareVox{false};

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        if (declareMon) {
            caps.txMonitorControl = RadioCapabilities::TxMonitorControl{};
        }
        if (declareVox) {
            caps.voxControl = RadioCapabilities::VoxControl{true};
        }
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
    void setPanRfGain(const QString&, int) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    StubBackend* backend{nullptr};

    // connected=false leaves the injected backend unconnected: the "no radio"
    // case, where nothing is refused whatever it declares.
    Fixture(bool mon, bool vox, bool connected = true)
    {
        auto owned = std::make_unique<StubBackend>();
        backend = owned.get();
        backend->declareMon = mon;
        backend->declareVox = vox;
        radio.setBackendForTest(std::move(owned), QStringLiteral("mon-vox-test"));
        SliceDelta slice;
        slice.frequency = 14.074;
        radio.emitBackendSliceChangedForTest(0, slice);
        if (!radio.slice(0)) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
        backend->connected = connected;
    }
};

QString cat(RigctlProtocol& port, const QString& line)
{
    const QString reply = port.handleLine(line).trimmed();
    QCoreApplication::processEvents();
    return reply;
}

void testRigctlRefusesOnARadioWithNeither()
{
    Fixture f(false, false);
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    auto& tx = f.radio.transmitModel();
    const int vox0 = tx.voxLevel();
    const int mon0 = tx.monGainSb();

    check("L VOXGAIN refused with RIG_ENAVAIL",
          cat(port, QStringLiteral("L VOXGAIN 0.9")) == QLatin1String("RPRT -11"));
    check("L VOXDELAY refused with RIG_ENAVAIL",
          cat(port, QStringLiteral("L VOXDELAY 1.0")) == QLatin1String("RPRT -11"));
    check("L MONITOR_GAIN refused with RIG_ENAVAIL",
          cat(port, QStringLiteral("L MONITOR_GAIN 0.9")) == QLatin1String("RPRT -11"));
    check("U VOX 1 refused with RIG_ENAVAIL",
          cat(port, QStringLiteral("U VOX 1")) == QLatin1String("RPRT -11"));
    check("and the model is untouched",
          tx.voxLevel() == vox0 && tx.monGainSb() == mon0 && !tx.voxEnable());
    check("U VOX 0 is still accepted: off is already true",
          cat(port, QStringLiteral("U VOX 0")) == QLatin1String("RPRT 0"));
}

void testRigctlUnchangedWhereDeclaredOrNoRadio()
{
    {
        Fixture f(true, true);
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("declared: L VOXGAIN accepted",
              cat(port, QStringLiteral("L VOXGAIN 0.9")) == QLatin1String("RPRT 0"));
        check("declared: L VOXDELAY accepted",
              cat(port, QStringLiteral("L VOXDELAY 1.0")) == QLatin1String("RPRT 0"));
        check("declared: L MONITOR_GAIN accepted",
              cat(port, QStringLiteral("L MONITOR_GAIN 0.9")) == QLatin1String("RPRT 0"));
        check("declared: U VOX 1 accepted",
              cat(port, QStringLiteral("U VOX 1")) == QLatin1String("RPRT 0"));
        check("declared: the model took the values",
              f.radio.transmitModel().voxLevel() == 90
                  && f.radio.transmitModel().monGainSb() == 90
                  && f.radio.transmitModel().voxEnable());
    }
    {
        Fixture f(false, false, /*connected=*/false);
        RigctlProtocol port(&f.radio);
        port.setSliceIndex(0);
        check("no radio: L VOXGAIN accepted as before",
              cat(port, QStringLiteral("L VOXGAIN 0.9")) == QLatin1String("RPRT 0"));
        // With no connected radio rigctl may not resolve the slice at all
        // (RPRT -8 before the function is looked at), which this change does not
        // touch: the property here is only that the new refusal does not fire.
        check("no radio: U VOX 1 is not refused with RIG_ENAVAIL",
              cat(port, QStringLiteral("U VOX 1")) != QLatin1String("RPRT -11"));
    }
}

void testTciRefusesAndReportsTheTrueState()
{
    Fixture f(false, false);
    TciProtocol tci(&f.radio);
    auto& tx = f.radio.transmitModel();
    const int mon0 = tx.monGainSb();

    tci.handleCommand(QStringLiteral("mon_enable:true"));
    QCoreApplication::processEvents();
    check("mon_enable:true leaves the model off", !tx.sbMonitor());
    check("and the peers are told it is still false",
          tci.pendingNotification() == QLatin1String("mon_enable:false;"));

    tci.handleCommand(QStringLiteral("mon_volume:80"));
    QCoreApplication::processEvents();
    check("mon_volume leaves the model's level alone", tx.monGainSb() == mon0);
    check("and the peers are told the real level",
          tci.pendingNotification() == QStringLiteral("mon_volume:%1;").arg(mon0));
}

void testTciUnchangedWhereDeclaredOrNoRadio()
{
    {
        Fixture f(true, false);
        TciProtocol tci(&f.radio);
        tci.handleCommand(QStringLiteral("mon_enable:true"));
        QCoreApplication::processEvents();
        check("declared: mon_enable:true turns the monitor on", f.radio.transmitModel().sbMonitor());
        check("declared: the notification says true",
              tci.pendingNotification() == QLatin1String("mon_enable:true;"));
        tci.handleCommand(QStringLiteral("mon_volume:80"));
        QCoreApplication::processEvents();
        check("declared: mon_volume is taken", f.radio.transmitModel().monGainSb() == 80);
    }
    {
        Fixture f(false, false, /*connected=*/false);
        TciProtocol tci(&f.radio);
        tci.handleCommand(QStringLiteral("mon_enable:true"));
        QCoreApplication::processEvents();
        check("no radio: mon_enable:true is taken as before", f.radio.transmitModel().sbMonitor());
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("mon-vox-roads"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testRigctlRefusesOnARadioWithNeither();
    testRigctlUnchangedWhereDeclaredOrNoRadio();
    testTciRefusesAndReportsTheTrueState();
    testTciUnchangedWhereDeclaredOrNoRadio();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
