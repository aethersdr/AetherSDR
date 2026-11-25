// Socket-free behavior at the shared GUI/model seam. Real capability records
// select Flex pixels, native analyzer points, or a fixed-geometry local no-op.
#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"
#include "gui/PanSpanControlGate.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QFile>
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
class NativeBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QStringList ids;
    QList<int> points;
    bool refuseClose{false};
    bool deferCreate{false};
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
    void setPanPixelWidth(const QString& id, int width) override { ids.append(id); points.append(width); }
    bool createPanadapter() override
    {
        if (!deferCreate) { emit panCenterBandwidthChanged(QStringLiteral("native-new"), 7.1, 0.384); }
        return true;
    }
    int removeCalls{0};
    bool removePanadapter(const QString&) override { ++removeCalls; return !refuseClose; }
};
// #7553: a keyed close of the pan hosting the TX slice, through each family's
// real capability record. A Refused declaration (HL2) refuses it in the model
// before panAboutToRemove, with the transmit-change Notice, and reaches no
// backend; unkeyed it proceeds. An Allowed one (Flex) is unaffected keyed.
void keyedTxPanClose(const QString& family, bool keyed, bool expectRefused, const char* what)
{
    RadioModel specimen;
    if (!specimen.rebuildBackendForTest(family)) { check(false, what); return; }
    RadioModel radio;
    auto owned = std::make_unique<NativeBackend>();
    NativeBackend* backend = owned.get();
    backend->caps = specimen.backendCapabilities();
    backend->caps.maxPanadapters = 2;
    radio.setBackendForTest(std::move(owned), family);
    emit backend->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
    emit backend->panCenterBandwidthChanged(QStringLiteral("native-tx"), 7.1, 0.384);
    SliceDelta delta;
    delta.inUse = true; delta.frequency = 7.1; delta.mode = QStringLiteral("USB");
    delta.filterLow = 300; delta.filterHigh = 2700;
    delta.panId = QStringLiteral("native-tx"); delta.txSlice = true;
    emit backend->sliceChanged(0, delta);
    const QString txPan = RadioModel::neutralPanIdStringForTest(1);
    const SliceModel* tx = radio.slice(0);
    TransmitDelta transmit;
    transmit.mox = keyed;
    radio.applyBackendTransmitDeltaForTest(transmit);
    int aboutToRemove = 0;
    QStringList notices;
    QObject::connect(&radio, &RadioModel::panAboutToRemove, &radio, [&](const QString&) { ++aboutToRemove; });
    QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &radio,
                     [&](const QString& message, const QString&, const QString& panId) {
                         notices << message + QLatin1Char('@') + panId;
                     });
    const bool gated = radio.keyedTxPanCloseRefused(txPan);
    const bool closed = radio.removePanadapter(txPan);
    const bool ok = tx && tx->isTxSlice() && tx->panId() == txPan && radio.isRadioTransmitting() == keyed
        && (expectRefused
                ? gated && !closed && aboutToRemove == 0 && backend->removeCalls == 0
                      && notices == QStringList{QStringLiteral("TX panadapter can't close while transmitting.@") + txPan}
                      && radio.panadapter(txPan) != nullptr
                : !gated && closed && aboutToRemove == 1 && backend->removeCalls == 1 && notices.isEmpty());
    if (!ok) {
        std::printf("  tx=%d txPan=%s slicePan=%s keyed=%d gated=%d closed=%d about=%d removes=%d notices=%s\n",
                    tx != nullptr, qPrintable(txPan), tx ? qPrintable(tx->panId()) : "-",
                    radio.isRadioTransmitting(), gated, closed, aboutToRemove, backend->removeCalls,
                    qPrintable(notices.join(QLatin1Char('|'))));
    }
    check(ok, what);
}
void native(const QString& family)
{
    RadioModel specimen;
    check(specimen.rebuildBackendForTest(family), "production family backend constructed without dialing");
    const RadioCapabilities caps = specimen.backendCapabilities();
    const bool spanDeclared = caps.panDisplayControl && caps.panDisplayControl->declaresSpan();
    check(spanFixedOnPan(true, caps.panDisplayControl.has_value(), spanDeclared, false)
              == (family == QLatin1String("sim")),
          "only the Demo's fixed span dims the -/+ pair");
    RadioModel radio;
    auto owned = std::make_unique<NativeBackend>();
    NativeBackend* backend = owned.get();
    backend->caps = caps;
    radio.setBackendForTest(std::move(owned), family);
    emit backend->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
    PanadapterModel* pan = radio.panadapter(RadioModel::neutralPanIdStringForTest(0));
    check(pan != nullptr, "native pan published through production mapping");
    const ReceiveDispatch result = radio.requestPanDimensions(pan, 1111, 700, 2222);
    const bool analyzer = caps.panDisplayControl && caps.panDisplayControl->supports(PanDisplayField::SpectrumPoints);
    check(result == (analyzer ? ReceiveDispatch::Dispatched : ReceiveDispatch::LocalOnly),
          "native dimensions distinguish actual analyzer dispatch from fixed geometry");
    check(analyzer ? backend->points == QList<int>{2222} && backend->ids == QStringList{QStringLiteral("native-first")}
                   : backend->points.isEmpty(), "only declared analyzer receives actual local points and backend ID");
    check(radio.requestPanDimensions(pan, 0, 700, 2222) == ReceiveDispatch::Unsupported
        && radio.requestPanDimensions(nullptr, 1111, 700, 2222) == ReceiveDispatch::Unsupported,
          "invalid dimensions and missing pan are refused");
    if (caps.panLifecycleControl && caps.panLifecycleControl->creation == PanLifecycleControl::Creation::Native) {
        backend->caps.maxPanadapters = 2;
        int calls = 0;
        QPointer<PanadapterModel> created;
        const ReceiveDispatch dispatched = radio.requestPanCreation(&radio, [&](const RadioControlReply& reply, PanadapterModel* value) {
            ++calls; if (reply.kind == RadioControlReply::Kind::Accepted) { created = value; }
        });
        check(dispatched == ReceiveDispatch::Dispatched && calls == 1 && created
            && created->panId() == RadioModel::neutralPanIdStringForTest(1), "native immediate creation timing and exact identity preserved");
        if (!created) { return; }
        backend->refuseClose = true;
        check(!radio.removePanadapter(created->panId(), PanCloseIntent::ExplicitTeardown)
            && radio.panadapter(created->panId()) == created, "explicit teardown never overrides native refusal");
        // A native radio can wait for receive-enable readback before it
        // publishes the pane. Admission alone must not report capacity full.
        RadioModel deferredRadio;
        auto deferredOwned = std::make_unique<NativeBackend>();
        NativeBackend* deferred = deferredOwned.get();
        deferred->caps = backend->caps;
        deferred->deferCreate = true;
        deferredRadio.setBackendForTest(std::move(deferredOwned), family);
        emit deferred->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
        calls = 0;
        created.clear();
        check(deferredRadio.requestPanCreation(&deferredRadio, [&](const RadioControlReply& reply, PanadapterModel* value) {
            ++calls; if (reply.kind == RadioControlReply::Kind::Accepted) { created = value; }
        }) == ReceiveDispatch::Dispatched && calls == 0,
              "delayed native creation waits without a false refusal");
        emit deferred->panCenterBandwidthChanged(QStringLiteral("native-new"), 7.1, 0.384);
        check(calls == 1 && created && created->panId() == RadioModel::neutralPanIdStringForTest(1),
              "native pane completes after its confirmed publication");
    }
}
} // namespace
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("pan-dimension-routing"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    for (const char* family : {"hl2", "anan", "icom", "sim"}) { native(QString::fromLatin1(family)); }
#ifdef AETHER_BACKEND_RTL
    native(QStringLiteral("rtl"));
#else
    {
        RadioModel radio;
        check(!radio.rebuildBackendForTest(QStringLiteral("rtl")), "optional RTL backend remains unavailable when not compiled");
    }
#endif
    {
        PanControlFixture f;
        PanadapterModel* first = f.publish(QStringLiteral("0x40000000"));
        f.publish(QStringLiteral("0x40000001"));
        f.clear();
        check(f.radio.requestPanDimensions(first, 1111, 700, 2222) == ReceiveDispatch::Dispatched
            && f.backend->commands() == QStringList{QStringLiteral("display pan set 0x40000000 xpixels=1111 ypixels=700")},
              "Flex sends actual widget dimensions as one paired request");
        const auto flexDisplay = f.radio.backendCapabilities().panDisplayControl;
        check(!spanFixedOnPan(true, flexDisplay.has_value(),
                  flexDisplay && flexDisplay->declaresSpan(), false),
              "Flex declares its span, so its -/+ pair stays live");
    }
    keyedTxPanClose(QStringLiteral("hl2"), true, true,
                    "HL2 caps, keyed: the TX pan's close is refused in the model before panAboutToRemove, with the Notice");
    keyedTxPanClose(QStringLiteral("hl2"), false, false,
                    "HL2 caps, unkeyed: the TX pan's close proceeds");
    keyedTxPanClose(QStringLiteral("flex"), true, false,
                    "Flex caps (Allowed), keyed: the TX pan's close proceeds unchanged");
    {
        // Source pin (MainWindow cannot be constructed in a test): pan_close
        // and the title-bar X close through the helper that forgets the Center
        // Lock only once removePanadapter() accepts; pan_close_center_lock_test
        // covers the helper's behaviour (#7584).
        QFile actions(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_ControlActions.cpp"));
        QFile wiring(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
        const bool read = actions.open(QIODevice::ReadOnly) && wiring.open(QIODevice::ReadOnly);
        const QByteArray actionsCode = actions.readAll();
        const QByteArray wiringCode = wiring.readAll();
        const QByteArray route = "closePanForgettingCenterLock(panId);";
        const QByteArray early = "clearCenterLockForPan(panId, true);";
        check(read && actionsCode.count(route) == 1 && wiringCode.count(route) == 1
                  && !actionsCode.contains(early) && !wiringCode.contains(early)
                  && wiringCode.contains("PanCloseCenterLock::closeThenForget(")
                  && wiringCode.contains("return m_radioModel.removePanadapter(id);"),
              "pan_close and the title-bar X forget the center lock only through the accepted close");
    }
    QFile source(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
    check(source.open(QIODevice::ReadOnly), "GUI dimension source readable");
    const QByteArray code = source.readAll();
    check(code.contains("requestPanDimensions(pan, xpix, ypix, panLocalSpectrumPointsFor(sw))")
        && !code.contains("display pan set %1 xpixels="), "GUI uses typed dimensions with distinct radio and analyzer counts");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
