// "Show TX in Waterfall" as a client flag where the waterfall rows are made on
// this host: request, store per radio, restore on connect, the Flex path, the
// bridge verb, the store's refusals. Socket-free, on the real RadioModel and
// TransmitModel. Nothing here keys: what the waterfall draws is not tested.
// The Radio Setup button is in radio_setup_show_tx_waterfall_test.

#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/ClientDisplaySettings.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>
#include <memory>

using namespace AetherSDR;
using CDS = AetherSDR::ClientDisplaySettings;

namespace AetherSDR {
// Model friendship: gives the session an identity and runs the two connection
// edges synchronously. The injected backend owns no transport.
struct RadioModelWakeTestAccess {
    static void connectAs(RadioModel& model, const QString& serial)
    {
        model.m_lastInfo.serial = serial;
        model.onConnected();
    }
    static void disconnect(RadioModel& model)
    {
        model.m_intentionalDisconnect = true;
        model.onDisconnected();
    }
};
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& command)
    {
        return server.handleLine(command, nullptr);
    }
};
} // namespace AetherSDR

namespace {

int g_failed = 0;
void check(bool ok, const char* name)
{
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

// A backend on the typed seam with no command plane and no display engine:
// what an HL2 is to RadioModel, without its sockets and threads.
class SeamBackend final : public IRadioBackend {
public:
    bool connected = true;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.family = QStringLiteral("hl2");
        return caps;
    }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64,
                         const QVariant&) override {}
};

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("show-tx-in-waterfall-client-flag"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const QString feature = QStringLiteral("ClientDisplay");
    const QString serialA = QStringLiteral("00:1C:C0:AA:AA:AA");
    const QString serialB = QStringLiteral("00:1C:C0:BB:BB:BB");
    const RadioSettingsScope scopeA(QStringLiteral("hl2"), serialA);
    const RadioSettingsScope scopeB(QStringLiteral("hl2"), serialB);

    // 1 and 2. A backend that shapes its own display.
    {
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<SeamBackend>(), QStringLiteral("hl2"));
        check(radio.shapesDisplayRatesLocally() && !radio.hasCommandPlane(),
              "fixture: shaped locally, no command plane");
        RadioModelWakeTestAccess::connectAs(radio, serialA);
        check(radio.settingsScope().radioId() == serialA, "fixture: the session has an identity");
        check(!radio.transmitModel().showTxInWaterfall(), "the flag starts false");

        int announced = 0;
        QObject::connect(&radio.transmitModel(), &TransmitModel::stateChanged,
                         [&announced] { ++announced; });

        check(radio.requestLocalShowTxInWaterfall(true), "the request is taken locally");
        check(radio.transmitModel().showTxInWaterfall(),
              "and the model reads true at once: no echo is coming");
        check(announced == 1, "stateChanged fired once, which is what reaches the pans");
        check(CDS::showTxInWaterfall(scopeA, true) == true, "stored for this radio");
        check(!CDS::showTxInWaterfall(scopeB, true), "and for no other");

        RadioModelWakeTestAccess::disconnect(radio);
        check(!radio.transmitModel().showTxInWaterfall(),
              "a disconnect clears the model, as before");
        RadioModelWakeTestAccess::connectAs(radio, serialA);
        check(radio.transmitModel().showTxInWaterfall(),
              "the next connect puts the stored value back");

        // Another radio of the same family: its own value, which is none.
        RadioModelWakeTestAccess::disconnect(radio);
        RadioModelWakeTestAccess::connectAs(radio, serialB);
        check(!radio.transmitModel().showTxInWaterfall(),
              "a second radio does not inherit the first one's flag");
        RadioModelWakeTestAccess::disconnect(radio);

        // Off is stored too, and survives the same round trip.
        RadioModelWakeTestAccess::connectAs(radio, serialA);
        check(radio.requestLocalShowTxInWaterfall(false), "switching off is taken locally");
        check(!radio.transmitModel().showTxInWaterfall(), "the model reads false");
        check(CDS::showTxInWaterfall(scopeA, true) == false, "a stored off is a value");
        RadioModelWakeTestAccess::disconnect(radio);
        RadioModelWakeTestAccess::connectAs(radio, serialA);
        check(!radio.transmitModel().showTxInWaterfall(), "and comes back as off");
        RadioModelWakeTestAccess::disconnect(radio);
    }

    // The real HL2 backend, never connected: the same route, and no identity
    // means the flag is applied but not written under the family-wide row.
    {
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
        check(radio.requestLocalShowTxInWaterfall(true), "Hl2Backend: taken locally");
        check(radio.transmitModel().showTxInWaterfall(), "Hl2Backend: the model reads true");
        check(RadioSettingsScope(QStringLiteral("hl2"), {}).featureExact(feature).isEmpty(),
              "an unknown identity never writes the family default");
    }

    // 3. A Flex: declined, nothing stored, the echo still works.
    {
        RadioModel radio;
        check(radio.rebuildBackendForTest(QStringLiteral("flex")), "fixture: Flex backend built");
        check(!radio.shapesDisplayRatesLocally(), "fixture: a Flex shapes nothing locally");
        check(!radio.requestLocalShowTxInWaterfall(true),
              "Flex: the request declines, so the caller sends the wire command");
        check(!radio.transmitModel().showTxInWaterfall(),
              "Flex: the model is NOT set optimistically - the radio decides");
        check(RadioSettingsScope(QStringLiteral("flex"), {}).featureExact(feature).isEmpty(),
              "Flex: nothing is stored");
        radio.handleStatusForTest(QStringLiteral("transmit"),
                                  {{QStringLiteral("show_tx_in_waterfall"), QStringLiteral("1")}});
        check(radio.transmitModel().showTxInWaterfall(),
              "Flex: the radio's status echo sets the model, unchanged");
        radio.handleStatusForTest(QStringLiteral("transmit"),
                                  {{QStringLiteral("show_tx_in_waterfall"), QStringLiteral("0")}});
        check(!radio.transmitModel().showTxInWaterfall(), "Flex: and clears it");
    }

    // 4. The bridge verb takes the same route.
    {
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<SeamBackend>(), QStringLiteral("hl2"));
        AutomationServer bridge;
        bridge.setRadioModel(&radio);
        const QJsonObject on = AutomationServerTestAccess::request(bridge, "txwaterfall on");
        check(on.value(QStringLiteral("ok")).toBool()
                  && on.value(QStringLiteral("txwaterfall")).toBool(),
              "txwaterfall on: accepted");
        check(radio.transmitModel().showTxInWaterfall(),
              "txwaterfall on: the model reads true");
        check(on.value(QStringLiteral("note")).toString().contains(QStringLiteral("client-side")),
              "txwaterfall on: the note says the flag is client-side here");
        const QJsonObject off = AutomationServerTestAccess::request(bridge, "txwaterfall off");
        check(off.value(QStringLiteral("ok")).toBool()
                  && !radio.transmitModel().showTxInWaterfall(),
              "txwaterfall off: the model reads false");
    }
    {
        RadioModel radio;
        check(radio.rebuildBackendForTest(QStringLiteral("flex")), "fixture: Flex backend built");
        AutomationServer bridge;
        bridge.setRadioModel(&radio);
        const QJsonObject on = AutomationServerTestAccess::request(bridge, "txwaterfall on");
        check(on.value(QStringLiteral("ok")).toBool(), "Flex txwaterfall on: accepted");
        check(on.value(QStringLiteral("note")).toString().contains(QStringLiteral("radio echoes")),
              "Flex txwaterfall on: the note still says the radio echoes");
        check(!radio.transmitModel().showTxInWaterfall(),
              "Flex txwaterfall on: the model waits for the radio, unchanged");
    }

    // 5. The store's own boundary.
    {
        const RadioSettingsScope c(QStringLiteral("hl2"), QStringLiteral("C"));
        const RadioSettingsScope flex(QStringLiteral("flex"), QStringLiteral("C"));
        check(!CDS::showTxInWaterfall(c, true), "nothing stored reads as unconfigured");
        CDS::saveShowTxInWaterfall(flex, false, true);
        check(flex.featureExact(feature).isEmpty(), "a radio-owned flag is never saved");
        CDS::saveShowTxInWaterfall(c, true, true);
        check(!CDS::showTxInWaterfall(c, false),
              "and never restored for a radio that owns it, even from a document that holds it");
        // The waterfall rate shares the document; neither writer disturbs the other.
        CDS::saveWaterfallRate(c, 0, true, 60);
        check(CDS::showTxInWaterfall(c, true) == true && CDS::waterfallRate(c, 0, true) == 60,
              "the flag and the waterfall rate share one document");
        CDS::saveShowTxInWaterfall(c, true, false);
        check(CDS::waterfallRate(c, 0, true) == 60, "writing the flag keeps the rate");
        check(c.setFeature(feature, 1, QJsonObject{{QStringLiteral("showTxInWaterfall"), 1}}),
              "non-bool fixture stored");
        check(!CDS::showTxInWaterfall(c, true), "a number is not the flag");
        const QJsonObject future{{QStringLiteral("showTxInWaterfall"), true}};
        check(c.setFeature(feature, 2, future), "future fixture stored");
        CDS::saveShowTxInWaterfall(c, true, false);
        check(!CDS::showTxInWaterfall(c, true), "future schema not interpreted");
        check(c.featureExact(feature) == future, "future schema not overwritten");
    }

    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
    }
    return g_failed ? 1 : 0;
}
