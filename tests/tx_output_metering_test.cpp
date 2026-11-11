// The TX applet's power and SWR gauges say "not measured" on a TX output the
// backend cannot meter (IndependentTxAntenna::unmeteredOutputs, the HL2's RF1),
// instead of presenting about 0 W and 1.0 as a reading (#4954).
//
// Pinned, socket-free, nothing keyed:
//   * the decision (txOutputIsMetered): ANT measured, RF1 not, an output on no
//     list measured, no record (every other backend) measured even for a token
//     spelled RF1, not connected measured;
//   * the real TxApplet on a RadioModel with a seam backend: on ANT both gauges
//     keep their own label, accessible name and description and stay enabled;
//     a TX antenna change to RF1 dims both with the reason in the bar, the
//     tooltip and accessibleDescription; back to ANT restores every text; the
//     same slice on a backend without the record is unchanged; a slice that is
//     not the TX slice does not label the gauges; the meter keeps drawing what
//     it is sent on RF1; retiring the RF1 TX slice restores the gauges.
//   * the HL2 backend declares RF1, and only RF1, as unmetered.
// NOT covered: pixels (the dimming is HGauge's existing !isEnabled() paint).

#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "gui/HGauge.h"
#include "gui/TxApplet.h"
#include "gui/TxOutputMetering.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QApplication>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

static RadioCapabilities withRecord(QStringList unmetered)
{
    RadioCapabilities caps;
    caps.independentTxAntenna = IndependentTxAntenna{std::move(unmetered)};
    return caps;
}

static void decisionCases()
{
    const RadioCapabilities hl2 = withRecord({QStringLiteral("RF1")});
    const RadioCapabilities plain;  // Flex, Sim, Icom, RTL: no record
    check(txOutputIsMetered(true, hl2, QStringLiteral("ANT")), "decision: HL2 ANT is measured");
    check(!txOutputIsMetered(true, hl2, QStringLiteral("RF1")), "decision: HL2 RF1 is not measured");
    check(txOutputIsMetered(true, hl2, QStringLiteral("rf1")),
          "decision: the token is matched exactly, as the backend publishes it");
    check(txOutputIsMetered(true, hl2, QString()), "decision: no TX output known is measured");
    check(txOutputIsMetered(false, hl2, QStringLiteral("RF1")), "decision: not connected is measured");
    check(txOutputIsMetered(true, plain, QStringLiteral("RF1")),
          "decision: without the record a token spelled RF1 is measured");
    check(txOutputIsMetered(true, plain, QStringLiteral("XVTA")),
          "decision: without the record every output is measured");
    check(txOutputIsMetered(true, withRecord({}), QStringLiteral("RF1")),
          "decision: a record with an empty list meters every output");
    check(txOutputNotMeteredReason(QStringLiteral("RF1")).startsWith(QLatin1String("Not measured on RF1:")),
          "decision: the reason names the output");
}

// The production backend seam, no sockets.
class SeamBackend final : public IRadioBackend {
public:
    bool withRecord{true};
    // The slice fixture is refused while connected, so connect after it.
    bool connected{false};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.canTransmit = true;
        if (withRecord) {
            caps.independentTxAntenna = IndependentTxAntenna{{QStringLiteral("RF1")}};
        }
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void setAtu(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void setTune(bool, int, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    TxApplet applet;
    HGauge* fwd{nullptr};
    HGauge* swr{nullptr};
    explicit Fixture(bool withRecord)
    {
        auto b = std::make_unique<SeamBackend>();
        b->withRecord = withRecord;
        SeamBackend* backend = b.get();
        radio.setBackendForTest(std::move(b), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            check(false, "slice fixture");
            return;
        }
        SliceDelta d;
        d.txSlice = true;
        d.mode = QStringLiteral("USB");
        d.panId = QStringLiteral("0x40000000");
        d.txAntenna = QStringLiteral("ANT");
        d.txAntennaList = QStringList{"ANT", "RF1"};
        radio.slice(0)->applyChanges(d);
        backend->connected = true;
        applet.setRadioModel(&radio);
        applet.setTransmitModel(&radio.transmitModel());
        // HGauge has no Q_OBJECT, so findChildren<HGauge*> cannot tell it from a
        // QWidget. The power gauge is built first, the SWR gauge second.
        for (QWidget* w : applet.findChildren<QWidget*>()) {
            if (auto* g = dynamic_cast<HGauge*>(w)) {
                if (!fwd) { fwd = g; } else if (!swr) { swr = g; }
            }
        }
    }
    void setTxAntenna(const QString& ant)
    {
        if (!radio.slice(0)) { return; }
        SliceDelta d;
        d.txAntenna = ant;
        radio.slice(0)->applyChanges(d);
    }
    bool gaugesAsBuilt() const
    {
        return fwd && swr && fwd->isEnabled() && swr->isEnabled()
            && fwd->label() == QLatin1String("RF Pwr") && swr->label() == QLatin1String("SWR")
            && fwd->accessibleName() == QLatin1String("Forward power gauge")
            && fwd->accessibleDescription() == QLatin1String("RF forward power in watts")
            && swr->accessibleName() == QLatin1String("SWR gauge")
            && swr->accessibleDescription() == QLatin1String("Standing wave ratio")
            && fwd->toolTip().isEmpty() && swr->toolTip().isEmpty();
    }
    bool gaugesNotMeasuredOn(const QString& ant) const
    {
        const QString reason = txOutputNotMeteredReason(ant);
        return fwd && swr && !fwd->isEnabled() && !swr->isEnabled()
            && fwd->label() == QStringLiteral("RF Pwr: not measured on %1").arg(ant)
            && swr->label() == QStringLiteral("SWR: not measured on %1").arg(ant)
            && fwd->accessibleName() == QStringLiteral("Forward power gauge, not measured on %1").arg(ant)
            && swr->accessibleName() == QStringLiteral("SWR gauge, not measured on %1").arg(ant)
            && fwd->accessibleDescription() == reason && swr->accessibleDescription() == reason
            && fwd->toolTip() == reason && swr->toolTip() == reason;
    }
};

static void appletCases()
{
    {
        Fixture f(/*withRecord*/ true);
        check(f.fwd && f.swr, "applet: both gauges found");
        if (!f.fwd || !f.swr) { return; }
        check(f.applet.txOutputMetered() && f.gaugesAsBuilt(),
              "applet HL2 ANT: both gauges unchanged (enabled, own label, name, description)");

        f.setTxAntenna(QStringLiteral("RF1"));
        check(!f.applet.txOutputMetered() && f.gaugesNotMeasuredOn(QStringLiteral("RF1")),
              "applet HL2 RF1: both gauges dimmed, labeled not measured, reason announced");

        // Still draws what the meter sends: a reading on RF1 is not hidden.
        f.applet.setTransmitting(true);
        f.applet.updateMeters(0.3f, 1.4f, true);
        check(f.fwd->value() == 0.3f && f.swr->value() == 1.4f,
              "applet HL2 RF1: the gauges keep drawing the meter's values");
        f.applet.setTransmitting(false);

        f.setTxAntenna(QStringLiteral("ANT"));
        check(f.applet.txOutputMetered() && f.gaugesAsBuilt(),
              "applet HL2 back to ANT: every text restored, gauges enabled");

        f.setTxAntenna(QStringLiteral("RF1"));
        SliceDelta notTx;
        notTx.txSlice = false;
        f.radio.slice(0)->applyChanges(notTx);
        check(f.applet.txOutputMetered() && f.gaugesAsBuilt(),
              "applet: an RF1 slice that is not the TX slice does not label the gauges");
    }
    {
        Fixture f(/*withRecord*/ false);
        f.setTxAntenna(QStringLiteral("RF1"));
        check(f.applet.txOutputMetered() && f.gaugesAsBuilt(),
              "applet non-HL2 (no record), TX antenna RF1: gauges unchanged");
    }
    {
        // Built with RF1 already in force: correct from the first paint.
        RadioModel radio;
        auto b = std::make_unique<SeamBackend>();
        SeamBackend* backend = b.get();
        radio.setBackendForTest(std::move(b), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            check(false, "slice fixture (pre-set)");
            return;
        }
        SliceDelta d;
        d.txSlice = true;
        d.panId = QStringLiteral("0x40000000");
        d.txAntenna = QStringLiteral("RF1");
        radio.slice(0)->applyChanges(d);
        backend->connected = true;
        TxApplet applet;
        applet.setRadioModel(&radio);
        check(!applet.txOutputMetered(), "applet attached while on RF1: not metered at once");

        // The TX slice on RF1 is retired by the backend: no TX slice is left,
        // so nothing is unmetered and the gauges come back.
        emit backend->sliceRemoved(0);
        QCoreApplication::processEvents();
        check(!radio.txSlice() && applet.txOutputMetered(),
              "applet: retiring the RF1 TX slice restores the gauges");
    }
}

static void hl2DeclaresRf1()
{
    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();
    check(caps.independentTxAntenna
              && caps.independentTxAntenna->unmeteredOutputs == QStringList{QStringLiteral("RF1")},
          "HL2 declares RF1, and only RF1, as unmetered");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tx-output-metering-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    decisionCases();
    appletCases();
    hl2DeclaresRf1();
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
