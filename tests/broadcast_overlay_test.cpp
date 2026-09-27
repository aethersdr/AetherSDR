// Socket-free current-reception presentation. No receiver, cluster, audio or RF.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/WfmBroadcastOverlay.h"
#include "gui/WfmPresentationSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;
namespace {
class Backend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    int commands{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override { ++commands; }
    void setSliceMode(int, const QString&) override { ++commands; }
    void setSliceFilter(int, int, int) override { ++commands; }
    void setSliceAgc(int, const QString&, int) override { ++commands; }
    void setPanCenter(const QString&, double, PanCenterIntent) override { ++commands; }
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { ++commands; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override { ++commands; }
};
HdFmReception reception(int program = 0, qint64 frequency = 100300000)
{
    HdFmReception value;
    value.valid = true;
    value.sessionId = 11;
    value.receiverEpoch = 22;
    value.revision = 33;
    value.frequencyHz = frequency;
    value.selectedProgram = program;
    value.observationSequence = 1;
    value.synced = value.audioValid = true;
    value.services = {{0, QStringLiteral("Main"), true}, {1, QStringLiteral("Second"), true}};
    value.stationName = QStringLiteral("Station");
    value.title = QStringLiteral("First song");
    value.artist = QStringLiteral("Artist");
    return value;
}
SliceDelta hdSlice(int program = 0, qint64 frequency = 100300000)
{
    SliceDelta value;
    value.mode = QStringLiteral("WFM");
    value.panId = QStringLiteral("test-pan");
    value.frequency = frequency / 1.0e6;
    value.inCapture = true;
    value.wfmAudioMode = WfmAudioMode::HdStereo;
    value.hdProgram = program;
    value.hdFmReception = reception(program, frequency);
    return value;
}
Backend* attach(RadioModel& model)
{
    auto backend = std::make_unique<Backend>();
    backend->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}, true, true, true};
    Backend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("test"));
    emit source->sliceChanged(3, hdSlice());
    return source;
}
}
class BroadcastOverlayTest : public QObject {
    Q_OBJECT
private slots:
    void init() { AppSettings::instance().remove(QStringLiteral("WfmApplet")); }
    void freshDefaultAndExplicitFalseHaveOnePersistentOwner()
    {
        auto& settings = WfmPresentationSettings::instance();
        QVERIFY(settings.broadcastOverlayEnabled());
        AppSettings::instance().setValue(QStringLiteral("WfmApplet"),
            QStringLiteral(R"({"other":17,"ui":{"retained":"yes","showLockScope":false}})"));
        QSignalSpy changed(&settings, &WfmPresentationSettings::overlayEnabledChanged);
        settings.setBroadcastOverlayEnabled(false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!settings.broadcastOverlayEnabled());
        QVERIFY(!settings.showLockScope());
        settings.setAppletOptions(true, true);
        AppSettings::instance().load();
        QVERIFY(!settings.broadcastOverlayEnabled());
        QVERIFY(settings.showLockScope());
        QVERIFY(settings.showDiagnostics());
        const QJsonObject document = QJsonDocument::fromJson(AppSettings::instance()
            .value(QStringLiteral("WfmApplet")).toString().toUtf8()).object();
        QCOMPARE(document.value(QStringLiteral("other")).toInt(), 17);
        QCOMPARE(document.value(QStringLiteral("ui")).toObject().value(QStringLiteral("retained")).toString(), QStringLiteral("yes"));
        settings.setBroadcastOverlayEnabled(false);
        QCOMPARE(changed.count(), 1);
    }
    void sameLocalEntryReplacesSongWithoutSpotOrCommandSideEffects()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        QCOMPARE(overlay.records().size(), 1);
        QCOMPARE(overlay.records().first().sliceId, 3);
        QVERIFY(overlay.records().first().displayText().startsWith(QStringLiteral("HD Radio · HD1")));
        QSignalSpy changed(&overlay, &WfmBroadcastOverlay::overlaysChanged);
        for (int i = 0; i < 30; ++i) {
            HdFmReception value = reception();
            value.observationSequence = i + 2;
            value.title = QStringLiteral("Song %1 <not markup>").arg(i);
            SliceDelta delta; delta.hdFmReception = value;
            emit source->sliceChanged(3, delta);
            QCOMPARE(overlay.records().size(), 1);
            QCOMPARE(overlay.records().first().title, value.title);
        }
        QCOMPARE(changed.count(), 30);
        QCOMPARE(source->commands, 0);
        QVERIFY(model.spotModel().spots().isEmpty());
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(false);
        QVERIFY(overlay.records().isEmpty());
        QVERIFY(model.slice(3)->hdFmReception().valid);
        QCOMPARE(model.slice(3)->hdFmReception().title, QStringLiteral("Song 29 <not markup>"));
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(true);
        QCOMPARE(overlay.records().size(), 1);
        QCOMPARE(overlay.records().first().title, QStringLiteral("Song 29 <not markup>"));
        QCOMPARE(source->commands, 0);
    }
    void acceptedIdentityAndPanOwnershipClearOldPresentation()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        const QString pan = model.slice(3)->panId();
        overlay.bind(&model, QStringLiteral("foreign-pan"));
        QVERIFY(overlay.records().isEmpty());
        overlay.bind(&model, pan);
        QCOMPARE(overlay.records().size(), 1);
        model.slice(3)->setHdProgram(1); // intent must not clear the accepted entry
        QCOMPARE(overlay.records().first().program, 0);
        SliceDelta change;
        change.hdProgram = 1;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change = {};
        change.hdFmReception = reception(1);
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().first().program, 1);
        change = {}; change.frequency = 101.1;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change = {}; change.hdFmReception = reception(1); // old frequency is rejected
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change.hdFmReception = reception(1, 101100000);
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().size(), 1);
        change = {}; change.inCapture = false;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change.inCapture = true;
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().size(), 1);
        change = {}; change.mode = QStringLiteral("AM");
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
    }
    void disconnectCapabilityRetirementAndRebindCannotRetainLabels()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        source->connected = false;
        emit model.connectionStateChanged(false);
        QVERIFY(overlay.records().isEmpty());
        source->connected = true;
        emit model.connectionStateChanged(true);
        QCOMPARE(overlay.records().size(), 1);
        source->caps.broadcastFmReceive->hdStereo = false;
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(overlay.records().isEmpty());
        source->caps.broadcastFmReceive->hdStereo = true;
        emit model.capabilitiesChanged(true, source->caps);
        QCOMPARE(overlay.records().size(), 1);
        emit source->sliceRemoved(3);
        QVERIFY(overlay.records().isEmpty());
        emit source->sliceChanged(3, hdSlice());
        QCOMPARE(overlay.records().size(), 1);
        overlay.bind(nullptr, {});
        QVERIFY(overlay.records().isEmpty());
        emit source->sliceChanged(3, hdSlice());
        QVERIFY(overlay.records().isEmpty());
        auto other = std::make_unique<RadioModel>();
        attach(*other);
        overlay.bind(other.get(), other->slice(3)->panId());
        QCOMPARE(overlay.records().size(), 1);
        other.reset();
        QVERIFY(overlay.records().isEmpty());
    }
    void lossOfSyncClearsTextAndBoundedSlicesNeverAccumulateHistory()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        HdFmReception lost = reception();
        lost.synced = false;
        ++lost.observationSequence;
        SliceDelta delta; delta.hdFmReception = lost;
        emit source->sliceChanged(3, delta);
        QVERIFY(overlay.records().isEmpty());
        for (int i = 0; i < 12; ++i) { emit source->sliceChanged(i, hdSlice()); }
        QCOMPARE(overlay.records().size(), 8);
        QVERIFY(model.spotModel().spots().isEmpty());
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("broadcast-overlay"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    BroadcastOverlayTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "broadcast_overlay_test.moc"
