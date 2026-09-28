// Socket-free model and widget validation. The injected backend captures
// typed intents; no receiver, firmware peer, sound device or RF is opened.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/ControlAvailabilityRegistry.h"
#include "gui/FilterPassbandWidget.h"
#include "gui/RxApplet.h"
#include "gui/VfoWidget.h"
#include "gui/WfmApplet.h"
#include "gui/WfmLockScope.h"
#include "gui/WfmPresentationSettings.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QAccessible>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QPixmap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVBoxLayout>
#include <limits>
#include <QLabel>
#include <QLayout>
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
    QVector<QPair<int, bool>> monoRequests;
    QVector<QPair<int, WfmAudioMode>> audioModeRequests;
    QVector<QPair<int, int>> programRequests;
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
    void setSliceWfmForceMono(int id, bool mono) override
    { monoRequests.append({id, mono}); }
    void setSliceWfmAudioMode(int id, WfmAudioMode mode) override
    { audioModeRequests.append({id, mode}); }
    void setSliceHdProgram(int id, int program) override
    { programRequests.append({id, program}); }
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

WfmReceptionDiagnostics measuredPilot(bool locked = false)
{
    WfmReceptionDiagnostics value;
    value.valid = true;
    value.pilotMagnitude = locked ? 0.04 : 0.001;
    value.pilotEngageThreshold = 0.01;
    value.pilotReleaseThreshold = 0.005;
    value.engageBlocks = 3;
    value.releaseBlocks = 5;
    value.pilotLocked = locked;
    value.observationDurationMs = 6000;
    value.stableDurationMs = 5000;
    value.lockDurationMs = locked ? 6000 : 0;
    return value;
}

HdFmReception measuredHd(int program = 0)
{
    HdFmReception value;
    value.valid = true;
    value.sessionId = 1;
    value.receiverEpoch = 2;
    value.revision = 3;
    value.frequencyHz = 100300000;
    value.selectedProgram = program;
    value.observationSequence = 1;
    value.synced = value.audioValid = true;
    value.services = {{0, QStringLiteral("Main"), true}, {1, QStringLiteral("Second"), true},
                      {2, QStringLiteral("Data only"), false}};
    value.stationName = QStringLiteral("Fixture HD station");
    value.title = QStringLiteral("Title <plain text>");
    value.artist = QStringLiteral("Artist");
    value.merLowerDb = 16.5;
    value.merUpperDb = 17.0;
    value.cber = 0.0001;
    value.syncDurationMs = 5000;
    return value;
}

SliceDelta initialWfm()
{
    SliceDelta delta;
    delta.mode = QStringLiteral("WFM");
    delta.frequency = 100.3;
    delta.filterLow = -90000;
    delta.filterHigh = 90000;
    delta.wfmDeemphasisUs = 75;
    delta.wfmStereoStatus = WfmStereoStatus::Acquiring;
    delta.wfmForceMono = false;
    delta.wfmReceptionDiagnostics = measuredPilot();
    return delta;
}

WfmBackend* attachBackend(RadioModel& model)
{
    auto backend = std::make_unique<WfmBackend>();
    backend->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}, true, true};
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

// Mirrors the panel's ownership of complete tile visibility. The child
// must remain showable when the parent tile reconnects or floats.
class WfmHarness : public QWidget {
public:
    QWidget* tile;
    WfmApplet* applet;
    WfmHarness()
    {
        auto* outer = new QVBoxLayout(this);
        tile = new QWidget(this);
        auto* inner = new QVBoxLayout(tile);
        applet = new WfmApplet(tile);
        inner->addWidget(applet);
        outer->addWidget(tile);
        tile->hide();
        connect(applet, &WfmApplet::availabilityChanged, tile, &QWidget::setVisible);
    }
};

void checkBroadcastFmVisibility(WfmApplet& applet, bool visible)
{
    QComboBox* combo = applet.findChild<QComboBox*>(QStringLiteral("wfmDeemphasis"));
    QLabel* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
    QVERIFY(combo && status);
    QWidget* tile = applet.parentWidget();
    QVERIFY(tile && tile->parentWidget());
    QCOMPARE(applet.isAvailable(), visible);
    QCOMPARE(tile->isHidden(), !visible);
    QVERIFY(!applet.isHidden());
    QCOMPARE(combo->isVisibleTo(tile->parentWidget()), visible);
    QCOMPARE(status->isVisibleTo(tile->parentWidget()), visible);
    QLayout* layout = tile->parentWidget()->layout();
    QVERIFY(layout);
    const int index = layout->indexOf(tile);
    QVERIFY(index >= 0);
    QCOMPARE(layout->itemAt(index)->isEmpty(), !visible);
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
        WfmHarness host;
        WfmApplet& rx = *host.applet;
        QComboBox* combo = rx.findChild<QComboBox*>(QStringLiteral("wfmDeemphasis"));
        QLabel* status = rx.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        QVERIFY(combo && status);
        checkBroadcastFmVisibility(rx, false);
        QVERIFY(!combo->isEnabled());
        QVERIFY(!combo->accessibleDescription().isEmpty());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        rx.setSlice(slice);
        checkBroadcastFmVisibility(rx, false);
        rx.setRadioModel(&model);
        checkBroadcastFmVisibility(rx, true);
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
        delta.wfmReceptionDiagnostics = measuredPilot(true);
        emit source->sliceChanged(3, delta);
        QCOMPARE(combo->currentData().toInt(), 50);
        QCOMPARE(status->text(), QStringLiteral("Stereo"));
        QVERIFY(status->accessibleName().contains(QStringLiteral("Stereo")));
        delta.wfmStereoStatus = WfmStereoStatus::Mono;
        delta.wfmReceptionDiagnostics = measuredPilot(false);
        emit source->sliceChanged(3, delta);
        QCOMPARE(status->text(), QStringLiteral("Mono fallback"));
        delta.mode = QStringLiteral("FM");
        emit source->sliceChanged(3, delta);
        checkBroadcastFmVisibility(rx, false);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        delta.mode = QStringLiteral("WFM");
        emit source->sliceChanged(3, delta);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(combo->isEnabled());
        // Replacement receive changes the individual controls' availability,
        // not whether this supported WFM cluster belongs in the layout.
        slice->setExternalReceiveAudioReplacementMute(true);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(!combo->isEnabled());
        slice->setExternalReceiveAudioReplacementMute(false);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(combo->isEnabled());
        delta.inCapture = false;
        delta.wfmStereoStatus = WfmStereoStatus::Unavailable;
        emit source->sliceChanged(3, delta);
        checkBroadcastFmVisibility(rx, true);
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        delta.inCapture = true;
        delta.wfmStereoStatus = WfmStereoStatus::Acquiring;
        emit source->sliceChanged(3, delta);
        checkBroadcastFmVisibility(rx, true);

        QLayout* layout = host.layout();
        QVERIFY(layout);
        layout->activate();
        const int visibleHeight = layout->minimumSize().height();
        source->caps.broadcastFmReceive.reset();
        emit model.capabilitiesChanged(true, source->caps);
        checkBroadcastFmVisibility(rx, false);
        QVERIFY(!combo->isEnabled());
        layout->activate();
        QVERIFY(layout->minimumSize().height() < visibleHeight);
        source->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}, true, true};
        emit model.capabilitiesChanged(true, source->caps);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(combo->isEnabled());
        layout->activate();
        QCOMPARE(layout->minimumSize().height(), visibleHeight);
        source->connected = false;
        emit model.connectionStateChanged(false);
        checkBroadcastFmVisibility(rx, false);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        QVERIFY(!combo->accessibleDescription().isEmpty());
        source->connected = true;
        emit model.connectionStateChanged(true);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(combo->isEnabled());
        rx.setSlice(nullptr);
        checkBroadcastFmVisibility(rx, false);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        QCOMPARE(source->requests.size(), 1);
    }

    void clusterVisibilityWaitsForAcceptedMode()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        SliceModel* slice = model.slice(3);
        QVERIFY(slice);
        WfmHarness host;
        WfmApplet& rx = *host.applet;
        rx.setRadioModel(&model);
        rx.setSlice(slice);
        QSignalSpy requested(slice, &SliceModel::modeChangeRequested);
        checkBroadcastFmVisibility(rx, true);

        slice->setMode(QStringLiteral("AM"));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(slice->mode(), QStringLiteral("WFM"));
        checkBroadcastFmVisibility(rx, true);
        SliceDelta accepted;
        accepted.mode = QStringLiteral("AM");
        emit source->sliceChanged(3, accepted);
        checkBroadcastFmVisibility(rx, false);

        slice->setMode(QStringLiteral("WFM"));
        QCOMPARE(requested.count(), 2);
        QCOMPARE(slice->mode(), QStringLiteral("AM"));
        checkBroadcastFmVisibility(rx, false);
        accepted.mode = QStringLiteral("WFM");
        emit source->sliceChanged(3, accepted);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(source->requests.isEmpty());
    }

    void clusterFollowsSelectedOwnedSliceAndModelBinding()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        SliceDelta otherState = initialWfm();
        otherState.mode = QStringLiteral("FM");
        otherState.filterLow = -8000;
        otherState.filterHigh = 8000;
        otherState.wfmStereoStatus = WfmStereoStatus::Unavailable;
        emit source->sliceChanged(4, otherState);
        SliceModel* first = model.slice(3);
        SliceModel* second = model.slice(4);
        QVERIFY(first && second);
        SliceModel foreign(3);
        foreign.applyChanges(initialWfm());
        WfmHarness host;
        WfmApplet& rx = *host.applet;
        rx.setRadioModel(&model);
        checkBroadcastFmVisibility(rx, false);
        rx.setSlice(first);
        checkBroadcastFmVisibility(rx, true);
        rx.setSlice(second);
        checkBroadcastFmVisibility(rx, false);

        SliceDelta oldSliceUpdate;
        oldSliceUpdate.wfmStereoStatus = WfmStereoStatus::Stereo;
        emit source->sliceChanged(3, oldSliceUpdate);
        checkBroadcastFmVisibility(rx, false);
        otherState = initialWfm();
        emit source->sliceChanged(4, otherState);
        checkBroadcastFmVisibility(rx, true);
        oldSliceUpdate.mode = QStringLiteral("AM");
        emit source->sliceChanged(3, oldSliceUpdate);
        checkBroadcastFmVisibility(rx, true);
        rx.setSlice(first);
        checkBroadcastFmVisibility(rx, false);
        emit source->sliceChanged(3, initialWfm());
        checkBroadcastFmVisibility(rx, true);

        // A matching numeric ID does not make a slice owned by this model.
        rx.setSlice(&foreign);
        checkBroadcastFmVisibility(rx, false);
        rx.setSlice(first);
        checkBroadcastFmVisibility(rx, true);
        rx.setRadioModel(nullptr);
        checkBroadcastFmVisibility(rx, false);
        rx.setRadioModel(&model);
        checkBroadcastFmVisibility(rx, true);
        rx.setSlice(nullptr);
        checkBroadcastFmVisibility(rx, false);
        emit source->sliceChanged(3, oldSliceUpdate);
        checkBroadcastFmVisibility(rx, false);
        rx.setSlice(second);
        checkBroadcastFmVisibility(rx, true);
        QVERIFY(source->requests.isEmpty());
    }

    void rxNoLongerContainsBroadcastControls()
    {
        RxApplet rx;
        QVERIFY(!rx.findChild<QComboBox*>(QStringLiteral("wfmDeemphasis")));
        QVERIFY(!rx.findChild<QLabel*>(QStringLiteral("wfmStereoStatus")));
        QVERIFY(!rx.findChild<QWidget*>(QStringLiteral("broadcastFmControls")));
    }

    void audioCycleWaitsForAcceptanceAndPreservesObservedPilot()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        WfmHarness host;
        WfmApplet& applet = *host.applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        QPushButton* cycle = applet.findChild<QPushButton*>(QStringLiteral("wfmAudioMode"));
        QLabel* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        QVERIFY(cycle && status && cycle->isEnabled());
        QCOMPARE(cycle->text(), QStringLiteral("Auto Stereo"));
        SliceDelta accepted;
        accepted.wfmStereoStatus = WfmStereoStatus::Stereo;
        accepted.wfmReceptionDiagnostics = measuredPilot(true);
        emit source->sliceChanged(3, accepted);
        QCOMPARE(status->text(), QStringLiteral("Stereo"));
        const QString stereoStyle = status->styleSheet();
        QTest::keyClick(cycle, Qt::Key_Space);
        QCOMPARE(source->monoRequests.size(), 1);
        QCOMPARE(source->monoRequests.last().first, 3);
        QVERIFY(source->monoRequests.last().second);
        QVERIFY(!model.slice(3)->wfmForceMono());
        QCOMPARE(cycle->text(), QStringLiteral("Auto Stereo"));
        QCOMPARE(status->styleSheet(), stereoStyle);
        accepted.wfmForceMono = true;
        accepted.wfmStereoStatus = WfmStereoStatus::Mono;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(cycle->text(), QStringLiteral("Mono"));
        QCOMPARE(status->text(), QStringLiteral("Pilot detected"));
        QVERIFY(status->styleSheet() != stereoStyle);
        QVERIFY(cycle->accessibleName().contains(QStringLiteral("Mono")));
        cycle->click();
        QCOMPARE(source->monoRequests.size(), 2);
        QVERIFY(!source->monoRequests.last().second);
        QCOMPARE(cycle->text(), QStringLiteral("Mono"));
        accepted.wfmForceMono = false;
        accepted.wfmStereoStatus = WfmStereoStatus::Stereo;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(cycle->text(), QStringLiteral("Auto Stereo"));
        QCOMPARE(status->styleSheet(), stereoStyle);
        source->caps.broadcastFmReceive->forceMonoControl = false;
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(!cycle->isEnabled());
        QVERIFY(!cycle->accessibleDescription().isEmpty());
        checkBroadcastFmVisibility(applet, true);
        cycle->click();
        QCOMPARE(source->monoRequests.size(), 2);
    }

    void bandwidthUsesAcceptedEdgesAndCapabilityBounds()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        WfmHarness host;
        WfmApplet& applet = *host.applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        QComboBox* bandwidth = applet.findChild<QComboBox*>(QStringLiteral("wfmBandwidth"));
        QVERIFY(bandwidth && bandwidth->isEnabled());
        QCOMPARE(bandwidth->currentData().toInt(), 180000);
        QSignalSpy requests(model.slice(3), &SliceModel::filterCommandIssued);
        QTest::keyClick(bandwidth, Qt::Key_Down);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(requests.last().at(0).toInt(), -100000);
        QCOMPARE(requests.last().at(1).toInt(), 100000);
        QCOMPARE(bandwidth->currentData().toInt(), 180000);
        SliceDelta accepted;
        accepted.filterLow = -70000;
        accepted.filterHigh = 80000;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(bandwidth->currentText(), QStringLiteral("150 kHz (custom)"));
        QCOMPARE(requests.count(), 1);
        source->caps.receiveFilterControl.reset();
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(!bandwidth->isEnabled());
        QCOMPARE(model.slice(3)->filterLow(), -70000);
        QCOMPARE(model.slice(3)->filterHigh(), 80000);
        checkBroadcastFmVisibility(applet, true);
        QCOMPARE(requests.count(), 1);
    }

    void diagnosticsExpireAndMeasuredTransitionsWarn()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        WfmApplet applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        QLabel* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        QLabel* details = applet.findChild<QLabel*>(QStringLiteral("wfmDiagnostics"));
        QVERIFY(status && details);
        SliceDelta observation;
        observation.wfmStereoStatus = WfmStereoStatus::Stereo;
        observation.wfmReceptionDiagnostics = measuredPilot(true);
        emit source->sliceChanged(3, observation);
        const QString healthyStyle = status->styleSheet();
        QCOMPARE(status->text(), QStringLiteral("Stereo"));
        observation.wfmReceptionDiagnostics->lockLossCount = 1;
        observation.wfmReceptionDiagnostics->reacquisitionCount = 1;
        observation.wfmReceptionDiagnostics->stableDurationMs = 100;
        emit source->sliceChanged(3, observation);
        QVERIFY(status->text().contains(QStringLiteral("Unstable")));
        QVERIFY(status->styleSheet() != healthyStyle);
        QVERIFY(details->text().contains(QStringLiteral("Lock losses: 1")));
        QVERIFY(details->text().contains(QStringLiteral("Acquire / release: 0.01 / 0.005 relative")));
        QVERIFY(details->text().contains(QStringLiteral("High blocks: 0 / 3")));
        QVERIFY(details->text().contains(QStringLiteral("Pilot state unchanged for 0.1 s (5 s window)")));
        observation.wfmReceptionDiagnostics->stableDurationMs = 5000;
        emit source->sliceChanged(3, observation);
        QCOMPARE(status->text(), QStringLiteral("Stereo"));
        QCOMPARE(status->styleSheet(), healthyStyle);
        // Explicit evidence export only: no files or extra waits in the
        // default test run. The image names and visible banner identify the
        // source as a socket-free fixture, never a live-radio qualification.
        const QString screenshotDirectory = qEnvironmentVariable("AETHER_WFM_APPLET_SCREENSHOT_DIR");
        if (!screenshotDirectory.isEmpty()) {
            QDir output(screenshotDirectory);
            QVERIFY(output.mkpath(QStringLiteral(".")));
            QCheckBox* showScope = applet.findChild<QCheckBox*>(QStringLiteral("wfmShowLockScope"));
            QCheckBox* showDiagnostics = applet.findChild<QCheckBox*>(QStringLiteral("wfmShowDiagnostics"));
            QPushButton* settings = applet.findChild<QPushButton*>(QStringLiteral("wfmSettingsToggle"));
            QVERIFY(showScope && showDiagnostics && settings);
            const bool previousScope = showScope->isChecked();
            const bool previousDiagnostics = showDiagnostics->isChecked();
            QLabel banner(QStringLiteral("TEST FIXTURE · synthetic pilot data"), &applet);
            ThemeManager::instance().applyStyleSheet(&banner,
                "QLabel { color: {{color.accent.warning}}; font-size: 10px; }");
            auto* layout = qobject_cast<QVBoxLayout*>(applet.layout());
            QVERIFY(layout);
            layout->insertWidget(0, &banner);
            showScope->setChecked(true);
            showDiagnostics->setChecked(true);
            settings->setChecked(true);
            applet.resize(300, applet.minimumSizeHint().height());
            applet.show();
            // Populate a short, real-time display history at the same 4 Hz
            // presentation cadence used by the backend. No receiver is open.
            for (int i = 0; i < 16; ++i) {
                SliceDelta fixture = observation;
                fixture.wfmReceptionDiagnostics->pilotMagnitude = 0.025 + 0.003 * (i % 5);
                fixture.wfmReceptionDiagnostics->observationDurationMs += 250 * (i + 1);
                fixture.wfmReceptionDiagnostics->observationSequence = i + 1;
                emit source->sliceChanged(3, fixture);
                QTest::qWait(250);
            }
            QVERIFY(applet.grab().save(output.filePath(QStringLiteral("wfm-test-fixture-expanded.png"))));
            showScope->setChecked(false);
            showDiagnostics->setChecked(false);
            settings->setChecked(false);
            applet.resize(260, applet.minimumSizeHint().height());
            QApplication::processEvents();
            QVERIFY(applet.grab().save(output.filePath(QStringLiteral("wfm-test-fixture-controls.png"))));
            showScope->setChecked(previousScope);
            showDiagnostics->setChecked(previousDiagnostics);
            applet.hide();
        }
        // Invalidation clears a previously green status even if a producer
        // has not delivered a separate stereo-status change yet.
        observation.wfmReceptionDiagnostics = WfmReceptionDiagnostics{};
        emit source->sliceChanged(3, observation);
        QCOMPARE(status->text(), QStringLiteral("Unavailable"));
        QVERIFY(status->styleSheet() != healthyStyle);
        QVERIFY(details->text().startsWith(QStringLiteral("No current receive measurements")));
        observation.wfmStereoStatus = WfmStereoStatus::Mono;
        observation.wfmReceptionDiagnostics = measuredPilot(false);
        emit source->sliceChanged(3, observation);
        QCOMPARE(status->text(), QStringLiteral("Mono fallback"));
        QVERIFY(!status->text().contains(QStringLiteral("Unstable")));
    }

    void settingsPersistIndependentDisplaysAndPreserveSiblingValues()
    {
        AppSettings::instance().setValue(QStringLiteral("WfmApplet"),
            QStringLiteral(R"({"other":17,"ui":{"retained":"yes","showLockScope":true,"showDiagnostics":false}})"));
        RadioModel model;
        attachBackend(model);
        {
            WfmApplet applet;
            applet.setRadioModel(&model);
            applet.setSlice(model.slice(3));
            QCheckBox* scope = applet.findChild<QCheckBox*>(QStringLiteral("wfmShowLockScope"));
            QCheckBox* diagnostics = applet.findChild<QCheckBox*>(QStringLiteral("wfmShowDiagnostics"));
            QLabel* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
            QVERIFY(scope && diagnostics && status);
            scope->setChecked(false);
            QVERIFY(!status->isHidden());
            diagnostics->setChecked(true);
            QVERIFY(applet.findChild<WfmLockScope*>()->isHidden());
            QVERIFY(!applet.findChild<QLabel*>(QStringLiteral("wfmDiagnostics"))->isHidden());
        }
        WfmApplet restored;
        QVERIFY(!restored.findChild<QCheckBox*>(QStringLiteral("wfmShowLockScope"))->isChecked());
        QVERIFY(restored.findChild<QCheckBox*>(QStringLiteral("wfmShowDiagnostics"))->isChecked());
        const QJsonObject document = QJsonDocument::fromJson(AppSettings::instance()
            .value(QStringLiteral("WfmApplet")).toString().toUtf8()).object();
        QCOMPARE(document.value(QStringLiteral("other")).toInt(), 17);
        QCOMPARE(document.value(QStringLiteral("ui")).toObject().value(QStringLiteral("retained")).toString(), QStringLiteral("yes"));
        AppSettings::instance().remove(QStringLiteral("WfmApplet"));
    }

    void scopeIsBoundedAndStopsWhileHidden()
    {
        QWidget parent;
        WfmLockScope scope(&parent);
        scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Stereo, false);
        QCOMPARE(scope.sampleCount(), 0);
        parent.show();
        scope.show();
        for (int i = 0; i < 200; ++i) {
            scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Stereo, false);
        }
        QCOMPARE(scope.sampleCount(), 160);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(&scope);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("0.04 relative")));
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("Stereo")));
        scope.hide();
        QCOMPARE(scope.sampleCount(), 0);
        scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Stereo, false);
        QCOMPARE(scope.sampleCount(), 0);
        scope.show();
        QCOMPARE(scope.sampleCount(), 0);
        scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Stereo, false);
        QCOMPARE(scope.sampleCount(), 1);
        scope.appendSample(std::numeric_limits<double>::quiet_NaN(), 0.01, 0.005,
                           WfmStereoStatus::Stereo, false);
        QCOMPARE(scope.sampleCount(), 0);
        scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Stereo, false);
        scope.clear();
        QCOMPARE(scope.sampleCount(), 0);
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("Awaiting")));
        scope.appendSample(0.04, 0.005, 0.01, WfmStereoStatus::Stereo, false);
        QCOMPARE(scope.sampleCount(), 0);
        scope.appendSample(0.04, 0.01, 0.005, WfmStereoStatus::Unavailable, false);
        QCOMPARE(scope.sampleCount(), 0);
    }

    void scopeHistoryClearsAcrossTuneSelectionAndDisconnect()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        WfmHarness host;
        WfmApplet& applet = *host.applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        host.show();
        WfmLockScope* scope = applet.findChild<WfmLockScope*>();
        QVERIFY(scope && scope->isVisible());
        SliceDelta observation;
        observation.wfmStereoStatus = WfmStereoStatus::Stereo;
        observation.wfmReceptionDiagnostics = measuredPilot(true);
        emit source->sliceChanged(3, observation);
        QCOMPARE(scope->sampleCount(), 1);
        SliceDelta tune;
        tune.frequency = 100.5;
        emit source->sliceChanged(3, tune);
        QCOMPARE(scope->sampleCount(), 0);
        observation.wfmReceptionDiagnostics->observationDurationMs += 250;
        emit source->sliceChanged(3, observation);
        QCOMPARE(scope->sampleCount(), 1);
        applet.setSlice(nullptr);
        QCOMPARE(scope->sampleCount(), 0);
        applet.setSlice(model.slice(3));
        QCOMPARE(scope->sampleCount(), 0);
        observation.wfmReceptionDiagnostics->observationDurationMs += 250;
        emit source->sliceChanged(3, observation);
        QCOMPARE(scope->sampleCount(), 1);
        source->connected = false;
        emit model.connectionStateChanged(false);
        QCOMPARE(scope->sampleCount(), 0);
        source->connected = true;
        emit model.connectionStateChanged(true);
        QCOMPARE(scope->sampleCount(), 0);
        observation.wfmReceptionDiagnostics = WfmReceptionDiagnostics{};
        emit source->sliceChanged(3, observation);
        QCOMPARE(scope->sampleCount(), 0);
    }

    void retiredSliceAndDestroyedModelReleaseBinding()
    {
        WfmHarness host;
        auto model = std::make_unique<RadioModel>();
        WfmBackend* source = attachBackend(*model);
        host.applet->setRadioModel(model.get());
        host.applet->setSlice(model->slice(3));
        checkBroadcastFmVisibility(*host.applet, true);
        emit source->sliceRemoved(3);
        checkBroadcastFmVisibility(*host.applet, false);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        emit source->sliceChanged(3, initialWfm());
        // Reusing an ID must not bind the new receiver implicitly.
        checkBroadcastFmVisibility(*host.applet, false);
        host.applet->setSlice(model->slice(3));
        checkBroadcastFmVisibility(*host.applet, true);
        model.reset();
        checkBroadcastFmVisibility(*host.applet, false);
    }

    void hdCycleProgramAndMetadataFollowAcceptedStateOnly()
    {
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        source->caps.broadcastFmReceive->hdStereo = true;
        emit model.capabilitiesChanged(true, source->caps);
        WfmApplet applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        auto* cycle = applet.findChild<QPushButton*>(QStringLiteral("wfmAudioMode"));
        auto* program = applet.findChild<QComboBox*>(QStringLiteral("wfmHdProgram"));
        auto* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        auto* metadata = applet.findChild<QLabel*>(QStringLiteral("wfmBroadcastMetadata"));
        QVERIFY(cycle && program && status && metadata);
        QVERIFY(!program->isEnabled());
        cycle->click();
        QCOMPARE(source->audioModeRequests.size(), 1);
        QCOMPARE(source->audioModeRequests.last().second, WfmAudioMode::HdStereo);
        QCOMPARE(cycle->text(), QStringLiteral("Auto Stereo"));
        QCOMPARE(model.slice(3)->wfmAudioMode(), WfmAudioMode::Stereo);
        SliceDelta accepted;
        accepted.wfmAudioMode = WfmAudioMode::HdStereo;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(cycle->text(), QStringLiteral("Digital"));
        QCOMPARE(status->text(), QStringLiteral("Digital acquiring"));
        QVERIFY(!program->isEnabled());
        accepted = {};
        accepted.hdFmReception = measuredHd();
        emit source->sliceChanged(3, accepted);
        QCOMPARE(status->text(), QStringLiteral("Digital audio valid"));
        const QString healthyStyle = status->styleSheet();
        QCOMPARE(program->count(), 2); // data-only services never masquerade as audio
        QCOMPARE(program->currentData().toInt(), 0);
        QVERIFY(program->isEnabled());
        QCOMPARE(metadata->textFormat(), Qt::PlainText);
        QVERIFY(metadata->text().contains(QStringLiteral("Digital · P1")));
        QVERIFY(metadata->text().contains(QStringLiteral("<plain text>")));
        const QString text = metadata->text();
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(false);
        QCOMPARE(metadata->text(), text); // overlay preference does not stop applet text
        QVERIFY(model.slice(3)->hdFmReception().audioValid);
        program->setCurrentIndex(1);
        QVERIFY(QMetaObject::invokeMethod(program, "activated", Q_ARG(int, 1)));
        QCOMPARE(source->programRequests.size(), 1);
        QCOMPARE(source->programRequests.last().second, 1);
        QCOMPARE(program->currentData().toInt(), 0); // pending intent is not readback
        accepted = {}; accepted.hdProgram = 1;
        emit source->sliceChanged(3, accepted);
        QVERIFY(!metadata->text().contains(QStringLiteral("Fixture HD station")));
        QVERIFY(status->styleSheet() != healthyStyle);
        accepted = {}; accepted.hdFmReception = measuredHd(1);
        emit source->sliceChanged(3, accepted);
        QCOMPARE(program->currentData().toInt(), 1);
        QCOMPARE(status->styleSheet(), healthyStyle);
        QVERIFY(metadata->text().contains(QStringLiteral("P2")));
        cycle->click();
        QCOMPARE(source->audioModeRequests.last().second, WfmAudioMode::Mono);
        QCOMPARE(cycle->text(), QStringLiteral("Digital"));
        accepted = {}; accepted.wfmAudioMode = WfmAudioMode::Mono; accepted.wfmForceMono = true;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(cycle->text(), QStringLiteral("Mono"));
        QVERIFY(!program->isEnabled());
        cycle->click();
        QCOMPARE(source->audioModeRequests.last().second, WfmAudioMode::Stereo);
        QCOMPARE(cycle->text(), QStringLiteral("Mono"));
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(true);
    }

    void hdPopupTelemetryPreservesPendingChoice_data()
    {
        QTest::addColumn<bool>("mouse");
        QTest::addColumn<int>("serviceChange");
        QTest::newRow("mouse-press-telemetry-release") << true << 0;
        QTest::newRow("keyboard-highlight-telemetry-return") << false << 0;
        QTest::newRow("mouse-service-rename") << true << 1;
        QTest::newRow("keyboard-service-rename") << false << 1;
        QTest::newRow("mouse-service-reorder") << true << 2;
        QTest::newRow("keyboard-service-reorder") << false << 2;
    }

    void hdPopupTelemetryPreservesPendingChoice()
    {
        QFETCH(bool, mouse);
        QFETCH(int, serviceChange);
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        source->caps.broadcastFmReceive->hdStereo = true;
        emit model.capabilitiesChanged(true, source->caps);
        WfmHarness host;
        WfmApplet& applet = *host.applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        SliceDelta observation;
        observation.wfmAudioMode = WfmAudioMode::HdStereo;
        observation.hdFmReception = measuredHd();
        emit source->sliceChanged(3, observation);
        auto* program = applet.findChild<QComboBox*>(QStringLiteral("wfmHdProgram"));
        auto* settings = applet.findChild<QPushButton*>(QStringLiteral("wfmSettingsToggle"));
        QVERIFY(program && settings && program->isEnabled());
        host.resize(400, 600);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        if (!settings->isChecked()) { QTest::mouseClick(settings, Qt::LeftButton); }
        QTRY_VERIFY(program->isVisible());
        QCOMPARE(program->currentData().toInt(), 0);
        QTest::mouseClick(program, Qt::LeftButton);
        QAbstractItemView* view = program->view();
        QTRY_VERIFY(view->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(view->window()));
        // Qt ignores the opening click's release for a short interval. Wait
        // before the distinct selection click; telemetry still interleaves
        // strictly between its press and release below.
        if (mouse) { QTest::qWait(QApplication::doubleClickInterval() + 20); }
        const QModelIndex second = program->model()->index(program->findData(1), 0);
        const QPoint position = view->visualRect(second).center();
        QVERIFY(view->viewport()->rect().contains(position));
        if (mouse) {
            QTest::mouseMove(view->viewport(), position);
            QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        } else {
            QTest::keyClick(view, Qt::Key_Down);
        }
        QCOMPARE(view->currentIndex().data(Qt::UserRole).toInt(), 1);
        QVERIFY(source->programRequests.isEmpty());
        for (int sequence = 2; sequence < 18; ++sequence) {
            observation.hdFmReception->observationSequence = sequence;
            observation.hdFmReception->syncDurationMs += 250;
            observation.hdFmReception->title = QStringLiteral("Telemetry %1").arg(sequence);
            if (serviceChange == 1) {
                observation.hdFmReception->services[1].name = QStringLiteral("Renamed %1").arg(sequence);
            } else if (serviceChange == 2) {
                observation.hdFmReception->services.swapItemsAt(0, 1);
            }
            emit source->sliceChanged(3, observation);
            QApplication::processEvents();
        }
        const bool popupStayedOpen = view->isVisible();
        const int highlightedProgram = view->currentIndex().data(Qt::UserRole).toInt();
        QVERIFY(source->programRequests.isEmpty()); // telemetry cannot activate a choice
        if (mouse) {
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        } else {
            QTest::keyClick(view, Qt::Key_Return);
        }
        QApplication::processEvents();
        qInfo("HD popup input=%s openAfterTelemetry=%d highlightedProgram=%d requests=%lld displayedProgram=%d",
              mouse ? "mouse" : "keyboard", popupStayedOpen, highlightedProgram,
              static_cast<long long>(source->programRequests.size()), program->currentData().toInt());
        QCOMPARE(source->programRequests.size(), 1);
        QCOMPARE(source->programRequests.first(), (QPair<int, int>{3, 1}));
        QVERIFY(popupStayedOpen);
        QCOMPARE(highlightedProgram, 1);
        QCOMPARE(model.slice(3)->hdProgram(), 0);
        QCOMPARE(program->currentData().toInt(), 0); // intent is not confirmed state
        if (serviceChange == 1) {
            QVERIFY(program->itemText(program->findData(1)).contains(QStringLiteral("Renamed 17")));
        }
        emit source->sliceChanged(3, observation); // unchanged backend state is not an acknowledgement
        QCOMPARE(program->currentData().toInt(), 0);
        QCOMPARE(source->programRequests.size(), 1);
        observation.hdProgram = 1;
        observation.hdFmReception = measuredHd(1);
        emit source->sliceChanged(3, observation);
        QCOMPARE(program->currentData().toInt(), 1);
        QCOMPARE(source->programRequests.size(), 1);
        QTest::mouseClick(program, Qt::LeftButton);
        QTRY_VERIFY(view->isVisible());
        QTest::keyClick(view, Qt::Key_Return); // the accepted service is a no-op
        QCOMPARE(source->programRequests.size(), 1);
    }

    void hdPopupCancelsWhenReceiverOrServiceIdentityChanges_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("mouse");
        const QStringList changes = {QStringLiteral("service-removed"), QStringLiteral("service-added"),
            QStringLiteral("audio-unavailable"), QStringLiteral("session"), QStringLiteral("epoch"),
            QStringLiteral("revision"), QStringLiteral("frequency"), QStringLiteral("slice"),
            QStringLiteral("reused-slice-id"), QStringLiteral("radio-model"), QStringLiteral("confirmed-program"),
            QStringLiteral("reception-unavailable")};
        for (const QString& change : changes) {
            for (bool mouse : {false, true}) {
                const QByteArray name = (change + (mouse ? QStringLiteral("-mouse") : QStringLiteral("-keyboard"))).toUtf8();
                QTest::newRow(name.constData()) << change << mouse;
            }
        }
    }

    void hdPopupCancelsWhenReceiverOrServiceIdentityChanges()
    {
        QFETCH(QString, change);
        QFETCH(bool, mouse);
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        source->caps.broadcastFmReceive->hdStereo = true;
        emit model.capabilitiesChanged(true, source->caps);
        RadioModel replacementModel;
        WfmBackend* replacementSource = attachBackend(replacementModel);
        replacementSource->caps.broadcastFmReceive->hdStereo = true;
        emit replacementModel.capabilitiesChanged(true, replacementSource->caps);
        WfmHarness host;
        WfmApplet& applet = *host.applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        SliceDelta observation = initialWfm();
        observation.wfmAudioMode = WfmAudioMode::HdStereo;
        observation.hdFmReception = measuredHd();
        emit source->sliceChanged(3, observation);
        auto* program = applet.findChild<QComboBox*>(QStringLiteral("wfmHdProgram"));
        auto* settings = applet.findChild<QPushButton*>(QStringLiteral("wfmSettingsToggle"));
        QVERIFY(program && settings && program->isEnabled());
        host.resize(400, 600);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        if (!settings->isChecked()) { QTest::mouseClick(settings, Qt::LeftButton); }
        QTRY_VERIFY(program->isVisible());
        QTest::mouseClick(program, Qt::LeftButton);
        QAbstractItemView* view = program->view();
        QTRY_VERIFY(view->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(view->window()));
        if (mouse) { QTest::qWait(QApplication::doubleClickInterval() + 20); }
        const QModelIndex second = program->model()->index(program->findData(1), 0);
        const QPoint position = view->visualRect(second).center();
        QVERIFY(view->viewport()->rect().contains(position));
        if (mouse) {
            QTest::mouseMove(view->viewport(), position);
            QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        } else {
            QTest::keyClick(view, Qt::Key_Down);
        }
        QCOMPARE(view->currentIndex().data(Qt::UserRole).toInt(), 1);
        QVERIFY(source->programRequests.isEmpty());
        if (change == QStringLiteral("slice")) {
            emit source->sliceChanged(4, observation);
            applet.setSlice(model.slice(4));
        } else if (change == QStringLiteral("reused-slice-id")) {
            SliceModel* original = model.slice(3);
            emit source->sliceRemoved(3);
            emit source->sliceChanged(3, observation);
            QVERIFY(model.slice(3) != original);
            applet.setSlice(model.slice(3));
        } else if (change == QStringLiteral("radio-model")) {
            emit replacementSource->sliceChanged(3, observation);
            applet.setRadioModel(&replacementModel);
            applet.setSlice(replacementModel.slice(3));
        } else {
            if (change == QStringLiteral("service-removed")) {
                observation.hdFmReception->services.removeAt(1);
            } else if (change == QStringLiteral("service-added")) {
                observation.hdFmReception->services.append({4, QStringLiteral("New service"), true});
            } else if (change == QStringLiteral("audio-unavailable")) {
                observation.hdFmReception->services[1].audioAvailable = false;
            } else if (change == QStringLiteral("session")) {
                ++observation.hdFmReception->sessionId;
            } else if (change == QStringLiteral("epoch")) {
                ++observation.hdFmReception->receiverEpoch;
            } else if (change == QStringLiteral("revision")) {
                ++observation.hdFmReception->revision;
            } else if (change == QStringLiteral("frequency")) {
                observation.frequency = 101.1;
                observation.hdFmReception->frequencyHz = 101100000;
            } else if (change == QStringLiteral("confirmed-program")) {
                observation.hdProgram = 1;
                observation.hdFmReception->selectedProgram = 1;
            } else if (change == QStringLiteral("reception-unavailable")) {
                observation.hdFmReception->valid = false;
            }
            emit source->sliceChanged(3, observation);
        }
        QApplication::processEvents();
        QTRY_VERIFY(!view->isVisible());
        QVERIFY(source->programRequests.isEmpty());
        QVERIFY(replacementSource->programRequests.isEmpty());
        // A completion already aimed at the old popup must never select a
        // same-numbered row in its replacement receiver or service list.
        if (mouse) {
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        } else {
            QTest::keyClick(view, Qt::Key_Return);
        }
        QApplication::processEvents();
        QVERIFY(source->programRequests.isEmpty());
        QVERIFY(replacementSource->programRequests.isEmpty());
        if (program->isEnabled() && program->count() > 1) {
            const int acceptedProgram = program->currentData().toInt();
            const int targetRow = program->currentIndex() == 0 ? program->count() - 1 : 0;
            const int targetProgram = program->itemData(targetRow).toInt();
            QTest::mouseClick(program, Qt::LeftButton);
            QTRY_VERIFY(view->isVisible());
            QTest::keyClick(view, targetRow == 0 ? Qt::Key_Home : Qt::Key_End);
            QCOMPARE(view->currentIndex().data(Qt::UserRole).toInt(), targetProgram);
            QTest::keyClick(view, Qt::Key_Return);
            const bool replacement = change == QStringLiteral("radio-model");
            WfmBackend* activeSource = replacement ? replacementSource : source;
            WfmBackend* inactiveSource = replacement ? source : replacementSource;
            const int sliceId = change == QStringLiteral("slice") ? 4 : 3;
            QCOMPARE(activeSource->programRequests, (QVector<QPair<int, int>>{{sliceId, targetProgram}}));
            QVERIFY(inactiveSource->programRequests.isEmpty());
            QCOMPARE(program->currentData().toInt(), acceptedProgram);
        }
    }

    void hdTelemetryScopeIsMeasuredBoundedAndRetiresOnIdentityChanges()
    {
        AppSettings::instance().remove(QStringLiteral("WfmApplet"));
        RadioModel model;
        WfmBackend* source = attachBackend(model);
        source->caps.broadcastFmReceive->hdStereo = true;
        WfmApplet applet;
        applet.setRadioModel(&model);
        applet.setSlice(model.slice(3));
        applet.resize(300, 460);
        applet.show();
        QCoreApplication::processEvents();
        auto* scope = applet.findChild<WfmLockScope*>();
        auto* status = applet.findChild<QLabel*>(QStringLiteral("wfmStereoStatus"));
        auto* metadata = applet.findChild<QLabel*>(QStringLiteral("wfmBroadcastMetadata"));
        QVERIFY(scope && status && metadata);
        SliceDelta accepted;
        accepted.wfmAudioMode = WfmAudioMode::HdStereo;
        accepted.hdFmReception = measuredHd();
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 1);
        auto* accessible = QAccessible::queryAccessibleInterface(scope);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("16.5 dB")));
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("Digital audio valid")));
        const QString healthyStyle = status->styleSheet();
        const QString fixtureDirectory = qEnvironmentVariable("AETHER_WFM_APPLET_SCREENSHOT_DIR");
        if (!fixtureDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(fixtureDirectory));
            auto* fixtureLabel = new QLabel(QStringLiteral("TEST FIXTURE · synthetic Digital telemetry"), &applet);
            fixtureLabel->setWordWrap(true);
            qobject_cast<QVBoxLayout*>(applet.layout())->insertWidget(0, fixtureLabel);
            auto* diagnostics = applet.findChild<QCheckBox*>(QStringLiteral("wfmShowDiagnostics"));
            diagnostics->setChecked(true);
            applet.findChild<QPushButton*>(QStringLiteral("wfmSettingsToggle"))->setChecked(true);
            applet.resize(300, applet.sizeHint().height());
            QCoreApplication::processEvents();
            QVERIFY(applet.grab().save(QDir(fixtureDirectory).filePath(QStringLiteral("wfm-test-fixture-hd.png"))));
            diagnostics->setChecked(false);
            applet.findChild<QPushButton*>(QStringLiteral("wfmSettingsToggle"))->setChecked(false);
        }
        HdFmReception next = measuredHd();
        next.title = QStringLiteral("Updated title");
        accepted = {}; accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 1); // same observation never grows history
        QCOMPARE(metadata->text().contains(QStringLiteral("Updated title")), true);
        next.observationSequence = 2;
        next.audioValid = false;
        accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 2);
        QCOMPARE(status->text(), QStringLiteral("Digital synced · awaiting audio"));
        QVERIFY(status->styleSheet() != healthyStyle);
        next.audioValid = true;
        next.syncLossCount = 1;
        next.syncDurationMs = 100;
        accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QVERIFY(status->text().contains(QStringLiteral("Unstable")));
        QVERIFY(status->styleSheet() != healthyStyle);
        next.observationSequence = 3;
        next.receiverEpoch = 4;
        accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 1);
        scope->hide();
        next.observationSequence = 4;
        accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 0);
        scope->show();
        QCOMPARE(scope->sampleCount(), 0);
        next.observationSequence = 5;
        accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 1);
        accepted = {}; accepted.frequency = 101.1;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(scope->sampleCount(), 0);
        QVERIFY(!metadata->text().contains(QStringLiteral("Updated title")));
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("Awaiting")));
        for (int i = 0; i < 180; ++i) { scope->appendHdSample(16.5, 17.0, true, true); }
        QCOMPARE(scope->sampleCount(), 160);
        scope->appendHdSample(std::numeric_limits<double>::quiet_NaN(), 17.0, true, true);
        QCOMPARE(scope->sampleCount(), 0);
        scope->appendHdSample({}, {}, false, false);
        QCOMPARE(scope->sampleCount(), 1);
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("unavailable")));
        scope->setHdMode(false);
        QCOMPARE(scope->sampleCount(), 0);
        QVERIFY(accessible->text(QAccessible::Value).contains(QStringLiteral("pilot")));
        SliceDelta another = initialWfm();
        another.wfmAudioMode = WfmAudioMode::HdStereo;
        another.hdFmReception = measuredHd();
        emit source->sliceChanged(4, another);
        applet.setSlice(model.slice(4));
        QVERIFY(metadata->text().contains(QStringLiteral("Fixture HD station")));
        const QString selectedText = metadata->text();
        next = measuredHd(); next.frequencyHz = 101100000; next.title = QStringLiteral("Stale old slice song");
        accepted = {}; accepted.hdFmReception = next;
        emit source->sliceChanged(3, accepted);
        QCOMPARE(metadata->text(), selectedText);
        applet.setSlice(nullptr);
        QVERIFY(!metadata->text().contains(QStringLiteral("Fixture HD station")));
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
