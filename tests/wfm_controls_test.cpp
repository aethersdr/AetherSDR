// Socket-free model and widget validation. The injected backend captures
// typed intents; no receiver, firmware peer, sound device or RF is opened.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "gui/FilterPassbandWidget.h"
#include "gui/RxApplet.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
#include <thread>

using namespace AetherSDR;

namespace {
class WfmBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected = true;
    QVector<QPair<int, int>> requests;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setSliceWfmDeemphasis(int id, int microseconds) override
    { requests.append({id, microseconds}); }
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

SliceDelta initialWfm()
{
    SliceDelta delta;
    delta.mode = QStringLiteral("WFM");
    delta.frequency = 100.3;
    delta.filterLow = -90000;
    delta.filterHigh = 90000;
    delta.wfmDeemphasisUs = 75;
    delta.wfmStereoStatus = WfmStereoStatus::Acquiring;
    return delta;
}

WfmBackend* attachBackend(RadioModel& model)
{
    auto backend = std::make_unique<WfmBackend>();
    backend->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}};
    backend->caps.receiveFilterControl = ReceiveFilterControl{
        SliceFrequencyControl::Authority::Engine,
        {{QStringLiteral("WFM"), -100000, -15000, 15000, 100000, 30000, 200000}}};
    WfmBackend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("test"));
    emit source->sliceChanged(3, initialWfm());
    return source;
}

QPushButton* button(QWidget& widget, const QString& text)
{
    for (QPushButton* candidate : widget.findChildren<QPushButton*>()) {
        if (candidate->text().compare(text, Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }
    return nullptr;
}
} // namespace

class WfmControlsTest : public QObject {
    Q_OBJECT
private slots:
    void observationsNeverFollowIntentOptimistically()
    {
        SliceModel slice(3);
        QCOMPARE(slice.wfmDeemphasisUs(), 0);
        QCOMPARE(slice.wfmStereoStatus(), WfmStereoStatus::Unavailable);
        QSignalSpy requested(&slice, &SliceModel::wfmDeemphasisRequested);
        QSignalSpy adopted(&slice, &SliceModel::wfmDeemphasisChanged);
        QSignalSpy status(&slice, &SliceModel::wfmStereoStatusChanged);
        QSignalSpy wire(&slice, &SliceModel::commandReady);
        slice.setWfmDeemphasis(50);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(slice.wfmDeemphasisUs(), 0);
        QVERIFY(adopted.isEmpty());
        QVERIFY(wire.isEmpty());
        for (int invalid : {-1, 0, 51, 100}) { slice.setWfmDeemphasis(invalid); }
        QCOMPARE(requested.count(), 1);
        slice.applyChanges(initialWfm());
        QCOMPARE(slice.wfmDeemphasisUs(), 75);
        QCOMPARE(slice.wfmStereoStatus(), WfmStereoStatus::Acquiring);
        QCOMPARE(adopted.count(), 1);
        QCOMPARE(status.count(), 1);
        slice.applyChanges(initialWfm());
        QCOMPARE(adopted.count(), 1);
        QCOMPARE(status.count(), 1);
        SliceDelta invalid;
        invalid.wfmDeemphasisUs = 99;
        invalid.wfmStereoStatus = static_cast<WfmStereoStatus>(99);
        slice.applyChanges(invalid);
        QCOMPARE(slice.wfmDeemphasisUs(), 75);
        QCOMPARE(slice.wfmStereoStatus(), WfmStereoStatus::Acquiring);
        for (WfmStereoStatus value : {WfmStereoStatus::Stereo, WfmStereoStatus::Mono,
                                      WfmStereoStatus::Unavailable}) {
            SliceDelta delta;
            delta.wfmStereoStatus = value;
            slice.applyChanges(delta);
            QCOMPARE(slice.wfmStereoStatus(), value);
        }
        QCOMPARE(status.count(), 4);
    }

    void intentsRequireLiveOwnedWfmAndDeclaredValue()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        SliceModel* slice = model.slice(3);
        QVERIFY(slice);
        slice->setWfmDeemphasis(50);
        QCOMPARE(source->requests.size(), 1);
        QCOMPARE(source->requests.last().first, 3);
        QCOMPARE(source->requests.last().second, 50);
        QCOMPARE(slice->wfmDeemphasisUs(), 75);
        source->caps.broadcastFmReceive.reset();
        slice->setWfmDeemphasis(50);
        source->caps.broadcastFmReceive = BroadcastFmReceive{{75}};
        slice->setWfmDeemphasis(50);
        source->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}};
        source->connected = false;
        slice->setWfmDeemphasis(50);
        source->connected = true;
        SliceDelta delta;
        delta.mode = QStringLiteral("FM");
        slice->applyChanges(delta);
        slice->setWfmDeemphasis(50);
        delta.mode = QStringLiteral("WFM");
        slice->applyChanges(delta);
        slice->setExternalReceiveAudioReplacementMute(true);
        slice->setWfmDeemphasis(50);
        slice->setExternalReceiveAudioReplacementMute(false);
        std::thread foreignThread([slice] { slice->setWfmDeemphasis(50); });
        foreignThread.join();
        QCOMPARE(source->requests.size(), 1);
        emit source->sliceRemoved(3);
        QVERIFY(!model.slice(3));
        // The retired object remains alive until deleteLater is delivered.
        slice->setWfmDeemphasis(50);
        QCOMPARE(source->requests.size(), 1);
    }

    void registryKeepsLegacyOfflinePolicyWhileLiveControlsDim()
    {
        RadioModel model;
        QWidget parent;
        QWidget legacy(&parent);
        QWidget live(&parent);
        ControlAvailabilityRegistry registry(model);
        const auto never = [](bool, const RadioCapabilities&) { return false; };
        registry.registerWidget(&legacy, QStringLiteral("Unsupported"), never);
        registry.registerWidget(&live, QStringLiteral("Connect a receiver"), never, {}, false);
        QVERIFY(legacy.isEnabled());
        QVERIFY(!live.isEnabled());
        QVERIFY(!live.isHidden());
        QCOMPARE(live.accessibleDescription(), QStringLiteral("Connect a receiver"));
    }

    void keyboardSelectionWaitsForAdoptedStateAndStatusIsObserved()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        SliceModel* slice = model.slice(3);
        QVERIFY(slice);
        RxApplet rx;
        QComboBox* combo = rx.findChild<QComboBox*>(QStringLiteral("wfmDeemphasis"));
        QLabel* status = rx.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        QVERIFY(combo && status);
        QVERIFY(!combo->isHidden());
        QVERIFY(!combo->isEnabled());
        QVERIFY(!combo->accessibleDescription().isEmpty());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        rx.setSlice(slice);
        rx.setRadioModel(&model);
        QVERIFY(combo->isEnabled());
        QCOMPARE(combo->focusPolicy(), Qt::StrongFocus);
        QCOMPARE(combo->currentData().toInt(), 75);
        QCOMPARE(status->text(), QStringLiteral("Acquiring"));
        QVERIFY(source->requests.isEmpty());
        QTest::keyClick(combo, Qt::Key_Up);
        QCOMPARE(source->requests.size(), 1);
        QCOMPARE(source->requests.last().second, 50);
        QCOMPARE(combo->currentData().toInt(), 75);
        SliceDelta delta;
        delta.wfmDeemphasisUs = 50;
        delta.wfmStereoStatus = WfmStereoStatus::Stereo;
        emit source->sliceChanged(3, delta);
        QCOMPARE(combo->currentData().toInt(), 50);
        QCOMPARE(status->text(), QStringLiteral("Stereo"));
        QVERIFY(status->accessibleName().contains(QStringLiteral("Stereo")));
        delta.wfmStereoStatus = WfmStereoStatus::Mono;
        emit source->sliceChanged(3, delta);
        QCOMPARE(status->text(), QStringLiteral("Mono"));
        delta.mode = QStringLiteral("FM");
        emit source->sliceChanged(3, delta);
        QVERIFY(!combo->isEnabled());
        QVERIFY(!combo->isHidden());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        delta.mode = QStringLiteral("WFM");
        emit source->sliceChanged(3, delta);
        QVERIFY(combo->isEnabled());
        slice->setExternalReceiveAudioReplacementMute(true);
        QVERIFY(!combo->isEnabled());
        slice->setExternalReceiveAudioReplacementMute(false);
        QVERIFY(combo->isEnabled());
        source->caps.broadcastFmReceive.reset();
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(!combo->isEnabled());
        source->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}};
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(combo->isEnabled());
        source->connected = false;
        emit model.connectionStateChanged(false);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        QVERIFY(!combo->accessibleDescription().isEmpty());
        source->connected = true;
        emit model.connectionStateChanged(true);
        QVERIFY(combo->isEnabled());
        rx.setSlice(nullptr);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
    }

    void broadcastFilterPresetsPreserveSavedEdgesInBothWidgets()
    {
        RadioModel model;
        attachBackend(model);
        SliceModel* slice = model.slice(3);
        QVERIFY(slice);
        RxApplet rx;
        VfoWidget vfo;
        rx.setRadioModel(&model);
        vfo.setRadioModel(&model);
        rx.setSlice(slice);
        vfo.setSlice(slice);
        FilterPassbandWidget* passband = rx.findChild<FilterPassbandWidget*>();
        QVERIFY(passband && passband->isEnabled());
        QSignalSpy intents(slice, &SliceModel::filterCommandIssued);
        for (QWidget* surface : {static_cast<QWidget*>(&rx), static_cast<QWidget*>(&vfo)}) {
            QPushButton* preset = button(*surface, QStringLiteral("180K"));
            QVERIFY(preset && preset->isEnabled());
            const int before = intents.count();
            preset->click();
            QCOMPARE(intents.count(), before + 1);
            QCOMPARE(intents.last().at(0).toInt(), -90000);
            QCOMPARE(intents.last().at(1).toInt(), 90000);
        }
        AppSettings::instance().setValue(QStringLiteral("FilterPresets_WFM"),
                                         QStringLiteral("-70000:80000"));
        rx.setSlice(slice);
        vfo.setSlice(slice);
        for (QWidget* surface : {static_cast<QWidget*>(&rx), static_cast<QWidget*>(&vfo)}) {
            QPushButton* saved = button(*surface, QStringLiteral("150K"));
            QVERIFY(saved && saved->isEnabled());
            const int before = intents.count();
            saved->click();
            QCOMPARE(intents.count(), before + 1);
            QCOMPARE(intents.last().at(0).toInt(), -70000);
            QCOMPARE(intents.last().at(1).toInt(), 80000);
        }
        QCOMPARE(AppSettings::instance().value(QStringLiteral("FilterPresets_WFM")).toString(),
                 QStringLiteral("-70000:80000"));
        AppSettings::instance().remove(QStringLiteral("FilterPresets_WFM"));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("wfm-controls"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    qRegisterMetaType<WfmStereoStatus>();
    WfmControlsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "wfm_controls_test.moc"
