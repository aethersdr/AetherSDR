// "Show TX in Waterfall" as a client flag where the backend declares the client
// owns it (RadioCapabilities::txWaterfallClientFlag): request, store per radio,
// restore on connect; the backends that declare nothing; the Flex path; the
// bridge verb; the store's refusals. Socket-free, on the real RadioModel,
// TransmitModel and backends. Nothing here keys: what the waterfall draws is
// not tested. The Radio Setup button is in radio_setup_show_tx_waterfall_test.

#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/ClientDisplaySettings.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

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
        setSerial(model, serial);
        model.onConnected();
    }
    // The reported serial too: it is what identifies an RTL-SDR.
    static void setSerial(RadioModel& model, const QString& serial)
    {
        model.m_lastInfo.serial = serial;
        model.m_lastInfo.serialIdentity.reportedSerial = serial;
    }
    // The connect edge's restore alone, for a backend whose onConnected()
    // continues into the command-plane handshake.
    static void restoreOnly(RadioModel& model, const QString& serial)
    {
        setSerial(model, serial);
        model.restoreClientShowTxInWaterfall();
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

// A backend on the typed seam that engages the record with the owner field
// false. No production backend declares this; it is the record's own second
// state, and it must behave as an absent record does.
class DeclaresFalseBackend final : public IRadioBackend {
public:
    bool connected = true;
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.family = QStringLiteral("seam");
        caps.txWaterfallClientFlag = RadioCapabilities::TxWaterfallClientFlag{false};
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

    // 1 and 2. The backend that declares the client owns the flag: the real
    // Hl2Backend, never connected.
    {
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
        check(radio.backendCapabilities().clientPersistsShowTxInWaterfall()
                  && !radio.hasCommandPlane(),
              "fixture: the HL2 declares the client owns the flag");
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

    // Production HL2 wiring, and the connect edge through the backend's own
    // connected(), then a pan and a slice in the order Hl2Backend publishes
    // them: the model already holds the stored flag at each announcement.
    // What MainWindow does with it is not tested here.
    {
        CDS::saveShowTxInWaterfall(scopeA, true, true);
        RadioModel radio;
        check(radio.rebuildBackendForTest(QStringLiteral("hl2")) && radio.backend(),
              "fixture: the production HL2 backend is wired");
        IRadioBackend* backend = radio.backend();
        RadioModelWakeTestAccess::setSerial(radio, serialA);
        check(!radio.transmitModel().showTxInWaterfall(), "fixture: false before the connect");

        int atConnected = -1, atPan = -1, atSlice = -1;
        const auto flag = [&radio] { return radio.transmitModel().showTxInWaterfall() ? 1 : 0; };
        QObject::connect(&radio, &RadioModel::connectionStateChanged, &radio,
                         [&](bool up) { if (up) { atConnected = flag(); } });
        QObject::connect(&radio, &RadioModel::panadapterAdded, &radio,
                         [&](PanadapterModel*) { atPan = flag(); });
        QObject::connect(&radio, &RadioModel::sliceAdded, &radio,
                         [&](SliceModel*) { atSlice = flag(); });

        const QString pan = QStringLiteral("seam-pan");
        emit backend->connected();
        emit backend->panCenterBandwidthChanged(pan, 7.1, 0.192);
        SliceDelta slice;
        slice.panId = pan;
        slice.frequency = 7.1;
        slice.mode = QStringLiteral("USB");
        slice.inUse = true;
        emit backend->sliceChanged(0, slice);

        check(atConnected == 1, "restored before connectionStateChanged(true) is emitted");
        check(atPan == 1, "and held when the first pan is announced");
        check(atSlice == 1, "and when the first slice is announced");
        RadioModelWakeTestAccess::disconnect(radio);
        CDS::saveShowTxInWaterfall(scopeA, true, false);
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

    // Declared opt-out, on each real non-Flex backend that declares no owner.
    // Its rows are made on this host, so the old family test would have taken
    // it. A document that already holds the flag is neither read nor rewritten.
    for (const QString& family : {QStringLiteral("icom"), QStringLiteral("anan"),
                                  QStringLiteral("rtl"), QStringLiteral("sim")}) {
        const QByteArray tag = family.toLatin1();
        const auto named = [&tag](const char* what) -> QByteArray {
            return tag + ": " + what;
        };
        RadioModel radio;
        if (!radio.rebuildBackendForTest(family)) {
            check(family == QLatin1String("rtl"), named("fixture: backend built").constData());
            continue;   // RTL-SDR support is optional in a build
        }
        RadioModelWakeTestAccess::setSerial(radio, serialA);
        const RadioSettingsScope scope = radio.settingsScope();
        check(scope.hasRadioIdentity() && scope.family() == family,
              named("fixture: the session has an identity").constData());
        const QJsonObject seeded{{QStringLiteral("showTxInWaterfall"), true},
                                 {QStringLiteral("waterfallRates"),
                                  QJsonObject{{QStringLiteral("0"), 60}}}};
        check(scope.setFeature(feature, 1, seeded), named("fixture: document seeded").constData());
        const RadioCapabilities caps = radio.backendCapabilities();
        check(radio.shapesDisplayRatesLocally(),
              named("fixture: the rows are made on this host").constData());
        check(!caps.clientPersistsShowTxInWaterfall(),
              named("declares no client owner").constData());

        if (radio.hasCommandPlane()) {
            RadioModelWakeTestAccess::restoreOnly(radio, serialA);
        } else {
            RadioModelWakeTestAccess::connectAs(radio, serialA);
        }
        check(CDS::showTxInWaterfall(radio.settingsScope(), true) == true,
              named("fixture: a declared owner would have read the seeded flag").constData());
        check(!radio.transmitModel().showTxInWaterfall(),
              named("the connect edge restores nothing").constData());
        check(!radio.requestLocalShowTxInWaterfall(true),
              named("the request declines").constData());
        check(!radio.transmitModel().showTxInWaterfall(),
              named("the model is not set").constData());
        check(!radio.requestLocalShowTxInWaterfall(false),
              named("and declines off as well").constData());
        check(scope.featureExact(feature) == seeded,
              named("the ClientDisplay document is untouched").constData());
        if (!radio.hasCommandPlane()) {
            RadioModelWakeTestAccess::disconnect(radio);
        }
        check(scope.setFeature(feature, 1, QJsonObject{}), named("fixture: cleared").constData());
    }

    // The record's second state: engaged with the owner field false.
    {
        const RadioSettingsScope scope(QStringLiteral("seam"), serialA);
        const QJsonObject seeded{{QStringLiteral("showTxInWaterfall"), true}};
        check(scope.setFeature(feature, 1, seeded), "declared false fixture: document seeded");
        RadioModel radio;
        radio.setBackendForTest(std::make_unique<DeclaresFalseBackend>(), QStringLiteral("seam"));
        check(radio.backendCapabilities().txWaterfallClientFlag.has_value()
                  && !radio.backendCapabilities().clientPersistsShowTxInWaterfall(),
              "declared false fixture: the record is engaged and names no owner");
        RadioModelWakeTestAccess::connectAs(radio, serialA);
        check(CDS::showTxInWaterfall(radio.settingsScope(), true) == true,
              "declared false fixture: a declared owner would have read the seeded flag");
        check(!radio.transmitModel().showTxInWaterfall(), "declared false: nothing is restored");
        check(!radio.requestLocalShowTxInWaterfall(true), "declared false: the request declines");
        check(!radio.transmitModel().showTxInWaterfall(), "declared false: the model is not set");
        check(!radio.requestLocalShowTxInWaterfall(false), "declared false: off declines too");
        check(scope.featureExact(feature) == seeded, "declared false: nothing is written");
        RadioModelWakeTestAccess::disconnect(radio);
    }

    // 3. A Flex: declined, nothing stored or restored, the echo still works.
    {
        const RadioSettingsScope scope(QStringLiteral("flex"), serialA);
        const QJsonObject seeded{{QStringLiteral("showTxInWaterfall"), true}};
        check(scope.setFeature(feature, 1, seeded), "Flex fixture: document seeded");
        RadioModel radio;
        check(radio.rebuildBackendForTest(QStringLiteral("flex")), "fixture: Flex backend built");
        check(!radio.shapesDisplayRatesLocally(), "fixture: a Flex shapes nothing locally");
        check(!radio.backendCapabilities().clientPersistsShowTxInWaterfall(),
              "Flex: declares no client owner, the radio holds the flag");
        RadioModelWakeTestAccess::restoreOnly(radio, serialA);
        check(!radio.transmitModel().showTxInWaterfall(), "Flex: nothing is restored");
        check(!radio.requestLocalShowTxInWaterfall(true),
              "Flex: the request declines, so the caller sends the wire command");
        check(!radio.transmitModel().showTxInWaterfall(),
              "Flex: the model is NOT set optimistically - the radio decides");
        check(!radio.requestLocalShowTxInWaterfall(false), "Flex: off declines too");
        check(scope.featureExact(feature) == seeded, "Flex: the document is untouched");
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
        radio.setBackendForTest(std::make_unique<hl2::Hl2Backend>(), QStringLiteral("hl2"));
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
    // Flex, and the demo simulator, which declares no owner: the verb sends the
    // wire text and answers as it does on a Flex. Nothing echoes on the demo.
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("sim")}) {
        const QByteArray tag = family.toLatin1();
        const auto named = [&tag](const char* what) -> QByteArray {
            return tag + " txwaterfall on: " + what;
        };
        RadioModel radio;
        check(radio.rebuildBackendForTest(family), named("fixture: backend built").constData());
        AutomationServer bridge;
        bridge.setRadioModel(&radio);
        const QJsonObject on = AutomationServerTestAccess::request(bridge, "txwaterfall on");
        check(on.value(QStringLiteral("ok")).toBool(), named("accepted").constData());
        check(on.value(QStringLiteral("note")).toString()
                  == QStringLiteral("radio echoes status; re-read with get transmit "
                                    "showTxInWaterfall"),
              named("the note is the radio-echo one").constData());
        check(!radio.transmitModel().showTxInWaterfall(),
              named("the model waits for the radio").constData());
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
