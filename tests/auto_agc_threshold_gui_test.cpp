// Socket-free proof of the real RX control and automation dispatcher. Status
// enters through normalized backend deltas; the fixture never starts a radio,
// listener, audio device, or transmitter. Optional artifacts describe this
// synthetic fixture explicitly and do not claim licensed-hardware convergence.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/ReceiveCommand.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/flex/FlexBackend.h"
#include "gui/RxApplet.h"
#include "gui/AgcCalibrationDialog.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QPoint>
#include <QPalette>
#include <QPushButton>
#include <QRect>
#include <QScopeGuard>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStringList>
#include <QVector>
#include <QtTest>

#include <cmath>
#include <memory>
#include <cstdlib>
#include <optional>
#include <utility>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

struct RadioModelWakeTestAccess
{
    static void license(RadioModel& radio, bool seen, bool enabled)
    {
        radio.m_licenseFeatures.insert(QStringLiteral("auto_agc"),
            LicenseFeatureState{seen, enabled,
                enabled ? QString() : QStringLiteral("No entitlement")});
        emit radio.licenseFeaturesChanged();
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

class AutoAgcBackend final : public IRadioBackend
{
public:
    struct ModeRequest {
        int sliceId;
        AgcThresholdMode mode;
    };

    bool connected{true};
    bool supported{true};
    bool refuse{false};
    std::optional<RadioCapabilities::AgcGainScale> gainScale;
    QVector<ModeRequest> modeRequests;
    QVector<SliceAgcRequest> agcRequests;
    QStringList wireCommands;
    FlexBackend flexEncoder;

    AutoAgcBackend()
    {
        flexEncoder.setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
        flexEncoder.setAutoAgcThresholdLicenseProvider([] { return true; });
        flexEncoder.setSliceCommandSinkForTest([this](const QString& command) {
            wireCommands.append(command);
        });
    }

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.hasAgcThreshold = true;
        caps.agcGainScale = gainScale;
        if (supported) {
            caps.autoAgcThresholdControl = RadioCapabilities::AutoAgcThresholdControl{
                QStringLiteral("auto_agc")};
        }
        return caps;
    }
    ReceiveControlPolicy receiveControlPolicy() const override
    {
        return ReceiveControlPolicy::Confirmed;
    }
    ReceiveDispatch requestSliceAgcThresholdMode(int sliceId, AgcThresholdMode mode) override
    {
        modeRequests.append({sliceId, mode});
        if (refuse) {
            return ReceiveDispatch::Unsupported;
        }
        return flexEncoder.requestSliceAgcThresholdMode(sliceId, mode);
    }
    void requestSliceAgc(int, const SliceAgcRequest& request) override
    {
        agcRequests.append(request);
    }
    void status(int sliceId, std::optional<AgcThresholdMode> mode, int threshold = 65)
    {
        SliceDelta delta;
        delta.active = true;
        delta.frequency = 14.25;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 100;
        delta.filterHigh = 2800;
        delta.agcMode = QStringLiteral("med");
        delta.agcThreshold = threshold;
        delta.agcThresholdMode = mode;
        emit sliceChanged(sliceId, delta);
    }
    void agcStatus(const QString& agcMode, int threshold, int offLevel)
    {
        SliceDelta delta;
        delta.active = true;
        delta.frequency = 14.25;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 100;
        delta.filterHigh = 2800;
        delta.agcMode = agcMode;
        delta.agcThreshold = threshold;
        delta.agcOffLevel = offLevel;
        delta.agcThresholdMode = AgcThresholdMode::Manual;
        emit sliceChanged(0, delta);
    }

    // Test helper for the source-selection matrix: set mode + agcMode + daxChannel
    // independently of the agcThresholdMode plumbing used by status().
    void modeStatus(int sliceId, const QString& mode, const QString& agcMode, int daxCh,
                    int threshold = 65)
    {
        SliceDelta delta;
        delta.active        = true;
        delta.frequency     = 14.25;
        delta.mode          = mode;
        delta.filterLow     = 100;
        delta.filterHigh    = 2800;
        delta.agcMode       = agcMode;
        delta.agcThreshold  = threshold;
        delta.agcThresholdMode = AgcThresholdMode::Manual;
        delta.daxChannel    = daxCh;
        emit sliceChanged(sliceId, delta);
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
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}

    // Test helper: remove a single slice without replacing the whole backend.
    // Named expelSlice to avoid hiding IRadioBackend::removeSlice (bool return).
    void expelSlice(int sliceId) { emit sliceRemoved(sliceId); }
};

void settle()
{
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

// Every tooltip a widget is given, in order. A confirmed-control slider is
// re-synced to the radio's value right after a drag step, so the drag's own
// tooltip is only seen here.
class ToolTipLog final : public QObject
{
public:
    explicit ToolTipLog(QWidget* widget)
        : m_widget(widget)
    {
        widget->installEventFilter(this);
    }
    QStringList seen;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_widget && event->type() == QEvent::ToolTipChange) {
            seen.append(m_widget->toolTip());
        }
        return false;
    }

private:
    QWidget* m_widget;
};

QJsonObject namedNode(const QJsonObject& node, const QString& name)
{
    if (node.value(QStringLiteral("objectName")).toString() == name) {
        return node;
    }
    for (const QJsonValue& child : node.value(QStringLiteral("children")).toArray()) {
        const QJsonObject found = namedNode(child.toObject(), name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

struct Fixture
{
    RadioModel radio;
    AutoAgcBackend* backend{nullptr};
    RxApplet rx;
    AutomationServer bridge;
    QJsonArray invocations;

    Fixture(bool supported = true, bool licensed = true,
            std::optional<AgcThresholdMode> initial = AgcThresholdMode::Manual)
    {
        auto owned = std::make_unique<AutoAgcBackend>();
        backend = owned.get();
        backend->supported = supported;
        radio.setBackendForTest(std::move(owned), QStringLiteral("auto-agct-test"));
        RadioModelWakeTestAccess::license(radio, licensed, licensed);
        backend->status(0, initial);
        rx.setObjectName(QStringLiteral("autoAgcProofRx"));
        QPalette palette = rx.palette();
        palette.setColor(QPalette::Window,
            ThemeManager::instance().color(&rx, QStringLiteral("color.background.app")));
        rx.setPalette(palette);
        rx.setAutoFillBackground(true);
        rx.setRadioModel(&radio);
        rx.setSlice(radio.slice(0));
        // RX applets occupy 248 logical pixels in the application's dock.
        rx.resize(248, rx.sizeHint().height());
        rx.show();
        bridge.setRadioModel(&radio);
        settle();
    }

    QJsonObject send(const QByteArray& line)
    {
        const QJsonObject reply = AutomationServerTestAccess::request(bridge, line);
        settle();
        if (line.startsWith("invoke ")) {
            invocations.append(QJsonObject{{QStringLiteral("request"), QString::fromUtf8(line)},
                {QStringLiteral("reply"), reply}});
        }
        return reply;
    }
    QJsonObject tree() { return send("dumpTree"); }
    QJsonObject buttonNode()
    {
        for (const QJsonValue& root : tree().value(QStringLiteral("roots")).toArray()) {
            const QJsonObject found = namedNode(root.toObject(), QStringLiteral("autoAgcT"));
            if (!found.isEmpty()) {
                return found;
            }
        }
        return {};
    }
    QPushButton* button() { return rx.findChild<QPushButton*>(QStringLiteral("autoAgcT")); }
    QSlider* threshold()
    {
        for (QSlider* slider : rx.findChildren<QSlider*>()) {
            if (slider->accessibleName() == QStringLiteral("AGC threshold")) {
                return slider;
            }
        }
        return nullptr;
    }
    QJsonObject slice() { return send("get slice 0").value(QStringLiteral("slice")).toObject(); }
    QJsonObject click() { return send("invoke autoAgcT click"); }

    bool capture(const QString& name, const QString& target = QStringLiteral("autoAgcProofRx"))
    {
        const QString output = qEnvironmentVariable("AETHER_AUTO_AGCT_PROOF_DIR");
        if (output.isEmpty()) {
            return true;
        }
        if (!QDir().mkpath(output)) {
            return false;
        }
        const QString path = QDir(output).filePath(name + QStringLiteral(".png"));
        const QJsonObject command{{QStringLiteral("cmd"), QStringLiteral("grab")},
            {QStringLiteral("target"), target},
            {QStringLiteral("path"), path}};
        const QJsonObject grabbed = send(QJsonDocument(command).toJson(QJsonDocument::Compact));
        if (!grabbed.value(QStringLiteral("ok")).toBool() || QImage(path).isNull()) {
            return false;
        }
        QJsonArray requests;
        for (const AutoAgcBackend::ModeRequest& request : backend->modeRequests) {
            requests.append(QJsonObject{{QStringLiteral("sliceId"), request.sliceId},
                {QStringLiteral("mode"), request.mode == AgcThresholdMode::Automatic
                    ? QStringLiteral("auto") : QStringLiteral("manual")}});
        }
        const QJsonObject evidence{{QStringLiteral("fixture"),
            QStringLiteral("synthetic normalized backend; no radio or transport")},
            {QStringLiteral("stage"), name},
            {QStringLiteral("requestedAppletWidth"), 248},
            {QStringLiteral("actualAppletWidth"), rx.width()},
            {QStringLiteral("devicePixelRatio"), rx.devicePixelRatioF()},
            {QStringLiteral("buttonGeometry"), QJsonObject{
                {QStringLiteral("x"), button()->x()}, {QStringLiteral("y"), button()->y()},
                {QStringLiteral("width"), button()->width()},
                {QStringLiteral("height"), button()->height()}}},
            {QStringLiteral("thresholdWidth"), threshold() ? threshold()->width() : -1},
            {QStringLiteral("modeRequests"), requests},
            {QStringLiteral("wireCommands"), QJsonArray::fromStringList(backend->wireCommands)},
            {QStringLiteral("invocations"), invocations},
            {QStringLiteral("getSlice"), send("get slice 0")},
            {QStringLiteral("dumpTree"), tree()},
            {QStringLiteral("grab"), grabbed}};
        QFile file(QDir(output).filePath(name + QStringLiteral(".json")));
        return file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(evidence).toJson(QJsonDocument::Indented)) > 0;
    }
};

} // namespace

class AutoAgcThresholdGuiTest : public QObject
{
    Q_OBJECT

private slots:
    void calibrationDialogDiscardsAndRebinds()
    {
        Fixture fixture;
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, nullptr, {});
        SliceModel* slice = fixture.radio.slice(0);
        dialog.setSlice(slice);
        QLabel* result = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        QVERIFY(result);
        for (const int value : {20, 50, 80}) {
            // Status (slider move) before levelChanged so the stale-level guard
            // sees fresh audio arriving after the value settles.
            fixture.backend->status(0, AgcThresholdMode::Manual, value);
            emit audio.levelChanged(static_cast<float>(value) / 100.0f);
            QTest::qWait(400);
        }
        QVERIFY(result->text().startsWith(QStringLiteral("Recommended AGC-T")));
        slice->requestAgcThresholdMode(AgcThresholdMode::Automatic);
        QCOMPARE(result->text(), QStringLiteral("Calibration stopped."));

        fixture.radio.stageSessionModelsForReconnectForTest();
        fixture.backend->status(0, AgcThresholdMode::Manual);
        QCOMPARE(fixture.radio.slice(0), slice);
        dialog.setSlice(slice);
        for (const int value : {25, 55, 85}) {
            fixture.backend->status(0, AgcThresholdMode::Manual, value);
            emit audio.levelChanged(static_cast<float>(value) / 100.0f);
            QTest::qWait(400);
        }
        QVERIFY(result->text().startsWith(QStringLiteral("Recommended AGC-T")));
    }

    void calibrationDialogSliceComboSelectsReceiver()
    {
        Fixture fixture;
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, nullptr, {});
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        settle();

        auto* combo = dialog.findChild<QComboBox*>();
        QVERIFY(combo);

        // Locate the start/stop button — calibration is blocked when the
        // targeted slice is in Automatic mode, so its enable state is a
        // reliable proxy for which slice the dialog is actually monitoring.
        QPushButton* startStop = nullptr;
        for (QPushButton* btn : dialog.findChildren<QPushButton*>()) {
            if (btn->text() == QStringLiteral("Auto Sweep")) {
                startStop = btn;
                break;
            }
        }
        QVERIFY(startStop);

        // Single receiver: combo must not be visible — no choice to present.
        QCOMPARE(combo->count(), 1);
        QVERIFY(!combo->isVisible());
        QVERIFY(startStop->isEnabled()); // slice 0 is in Manual — calibration allowed

        // Add a second slice — combo becomes visible with two items,
        // still targeting slice 0.
        fixture.backend->status(1, AgcThresholdMode::Manual);
        settle();
        QCOMPARE(combo->count(), 2);
        QVERIFY(combo->isVisible());
        QCOMPARE(combo->currentData().toInt(), 0);

        // Pick slice 1 in the combo — dialog must retarget to slice 1.
        const int idx1 = combo->findData(1);
        QVERIFY(idx1 >= 0);
        combo->setCurrentIndex(idx1);
        settle();
        QCOMPARE(combo->currentData().toInt(), 1);

        // Prove retargeting: flip slice 1 to Automatic — that blocks
        // calibration, so the button must be disabled.  If the dialog
        // were still watching slice 0 (which remains Manual) the button
        // would stay enabled and this assertion would fail.
        fixture.backend->status(1, AgcThresholdMode::Automatic);
        settle();
        QVERIFY(!startStop->isEnabled());

        // Restore slice 1 to Manual before triggering removal.
        fixture.backend->status(1, AgcThresholdMode::Manual);
        settle();
        QVERIFY(startStop->isEnabled());

        // Remove only slice 1 (the targeted slice), leaving slice 0 alive.
        // The sliceRemoved handler must fall back to slice 0; the combo
        // must shrink to one item and hide.
        fixture.backend->expelSlice(1);
        settle();
        QCOMPARE(combo->count(), 1);
        QVERIFY(!combo->isVisible());

        // Prove fallback: slice 0 is in Manual mode so calibration is
        // allowed — button must be enabled again.
        QVERIFY(startStop->isEnabled());

        // Stronger fallback proof: the previous assertion passes whether or
        // not the dialog actually re-targeted A, because A was already in
        // Manual before B was removed.  Flip A to Automatic — calibration
        // must block, proving the dialog is monitoring A's signals and not
        // some stale pre-removal state.
        fixture.backend->status(0, AgcThresholdMode::Automatic);
        settle();
        QVERIFY(!startStop->isEnabled());
    }

    void calibrationDialogSourceSelection()
    {
        // Source-selection matrix: isTciMode() gates TCI audio on
        //   DIGU/DIGL + daxChannel >= 1 + agcMode != "off"
        // Verified indirectly through the sourceLabel (agcRole="source"), which
        // is the public observable for the private isTciMode() predicate.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        dialog.setTciAudioProbe([] { return true; }); // a TCI client is streaming
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        settle();

        QLabel* source = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("source")) {
                source = label;
                break;
            }
        }
        QVERIFY(source);

        const auto setMode = [&](const QString& mode, const QString& agcMode, int daxCh) {
            fixture.backend->modeStatus(0, mode, agcMode, daxCh);
            settle();
        };

        QLabel* modeLabel = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("mode")) {
                modeLabel = label;
            }
        }
        QVERIFY(modeLabel);

        // 1. DIGU + daxChannel=1 + agcMode="med" → TCI
        setMode(QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: TCI (DIGU/DIGL)"));
        // Both sources name the slice mode in the heading's parenthesis.
        QVERIFY2(modeLabel->text().contains(QStringLiteral("via TCI audio (DIGU)")),
                 qPrintable(modeLabel->text()));

        // 1a. Mode-only flip: DIGU → USB (daxChannel and agcMode unchanged).
        //     Triggers a single modeChanged signal, so updateModeUi() runs once.
        //     The label must reflect the new (PC Audio) source immediately, not
        //     carry the previous TCI latch. Fails if rewireAudioSource() is
        //     called after the labels rather than before them.
        setMode(QStringLiteral("USB"), QStringLiteral("med"), 1);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: PC Audio (switch to DIGU/DIGL with a DAX channel for TCI)"));
        QVERIFY2(modeLabel->text().contains(QStringLiteral("via PC Audio (USB)")),
                 qPrintable(modeLabel->text()));

        // 2. DIGL + daxChannel=1 + agcMode="slow" → TCI
        setMode(QStringLiteral("DIGL"), QStringLiteral("slow"), 1);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: TCI (DIGU/DIGL)"));

        // 3a. DIGU + daxChannel=1 + agcMode="off" → PC Audio (AGC Off, dax≥1)
        setMode(QStringLiteral("DIGU"), QStringLiteral("off"), 1);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: PC Audio (AGC Off always uses PC Audio)"));

        // 3b. DIGU + daxChannel=0 + agcMode="off" → same message; AGC-Off branch
        //     must come before the daxChannel<1 branch so the user is not told to
        //     assign a DAX channel when doing so would still not enable TCI.
        setMode(QStringLiteral("DIGU"), QStringLiteral("off"), 0);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: PC Audio (AGC Off always uses PC Audio)"));

        // 4. DIGU + daxChannel=0 + agcMode="med" → PC Audio (no DAX channel)
        setMode(QStringLiteral("DIGU"), QStringLiteral("med"), 0);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: PC Audio (TCI needs a DAX channel in DIGU/DIGL)"));

        // 5. USB + daxChannel=1 + agcMode="med" → PC Audio (not DIGU/DIGL)
        setMode(QStringLiteral("USB"), QStringLiteral("med"), 1);
        QCOMPARE(source->text(),
            QStringLiteral("Audio source: PC Audio (switch to DIGU/DIGL with a DAX channel for TCI)"));
#else
        QSKIP("HAVE_WEBSOCKETS not set; TCI source-selection test requires TciServer");
#endif
    }

    void calibrationDialogTciWiringFollowsDaxChannel()
    {
        // The TCI source is wired, not just labelled: a manual AGC-T move
        // records only when rxLevel arrives on the slice's own DAX channel.
        // A settle with no level since the move reports "no TCI audio".
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        dialog.setTciAudioProbe([] { return true; }); // a TCI client is streaming
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        settle();

        QLabel* result = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        QVERIFY(result);
        const QString noTci =
            QStringLiteral("No TCI audio yet. Is your TCI client receiving on this slice?");

        // A level on the slice's DAX channel after the move: the point records.
        // The manual settle is 280 ms, so wait past it before checking.
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1, 40);
        settle();
        emit tci.rxLevel(1, 0.3f);
        QTest::qWait(600);
        QVERIFY2(!result->text().contains(noTci), qPrintable(result->text()));

        // A level on another DAX channel only: nothing reached the engine.
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1, 30);
        settle();
        emit tci.rxLevel(2, 0.3f);
        QTRY_COMPARE_WITH_TIMEOUT(result->text(), noTci, 2000);
#else
        QSKIP("HAVE_WEBSOCKETS not set; TCI wiring test requires TciServer");
#endif
    }

    void calibrationDialogSweepNoAudioNamesLatchedSource()
    {
        // The sweep latches TCI at start. A mode change mid-sweep must not
        // change the source its "Stopped:" message names: onFinished()
        // re-latches m_tciActive from the live mode (USB, PC Audio) before
        // noAudio arrives.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        dialog.setTciAudioProbe([] { return true; }); // a TCI client is streaming
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        settle();

        QLabel* result = nullptr;
        QPushButton* startStop = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Auto Sweep")) {
                startStop = button;
            }
        }
        QVERIFY(result && startStop);
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));

        // DIGU -> USB mid-sweep. No TCI level arrives, so the first step
        // (280 ms) stops the sweep for want of audio. The status also carries
        // the sweep's first write (100) landing, so the restore is a change.
        fixture.backend->modeStatus(0, QStringLiteral("USB"), QStringLiteral("med"), 1, 100);
        settle();
        QTRY_COMPARE_WITH_TIMEOUT(startStop->text(), QStringLiteral("Auto Sweep"), 2000);
        QCOMPARE(result->text(),
            QStringLiteral("Stopped: no TCI audio. Is your TCI client receiving on this slice?"));

        // A late echo of the restore (as on a Confirmed backend such as
        // RTL-SDR) is the calibrator's own write: its silent settle must not
        // replace the stop message.
        fixture.backend->modeStatus(0, QStringLiteral("USB"), QStringLiteral("med"), 1, 65);
        QTest::qWait(600);
        QCOMPARE(result->text(),
            QStringLiteral("Stopped: no TCI audio. Is your TCI client receiving on this slice?"));
#else
        QSKIP("HAVE_WEBSOCKETS not set; latched-source test requires TciServer");
#endif
    }

    void calibrationDialogManualNoAudioWording()
    {
        // A manual move that settles with no audio stopped nothing, so the
        // notice drops "Stopped:" and says what to check.
        Fixture fixture;
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, nullptr, {});
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        settle();
        QLabel* result = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        QVERIFY(result);
        fixture.backend->status(0, AgcThresholdMode::Manual, 40);
        QTRY_COMPARE_WITH_TIMEOUT(result->text(),
            QStringLiteral("No audio on this computer yet. Is PC Audio on?"), 2000);
    }

    void calibrationDialogManualNoAudioKeepsRecommendation()
    {
        // A silent manual move records nothing, so the recommendation on show
        // stays valid and its line must survive the no-audio notice.
        Fixture fixture;
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, nullptr, {});
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        settle();
        QLabel* result = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        QVERIFY(result);
        for (const int value : {20, 50, 80}) {
            fixture.backend->status(0, AgcThresholdMode::Manual, value);
            emit audio.levelChanged(static_cast<float>(value) / 100.0f);
            QTest::qWait(400);
        }
        QVERIFY2(result->text().startsWith(QStringLiteral("Recommended AGC-T")),
                 qPrintable(result->text()));
        const QString recommendation = result->text();

        // No level after this move: the settle (280 ms) reports no audio.
        fixture.backend->status(0, AgcThresholdMode::Manual, 35);
        QTest::qWait(600);
        QCOMPARE(result->text(), recommendation);
    }

    void calibrationDialogNrCheckReadsLatchedSource()
    {
        // Client NR2 runs in AudioEngine, which the TCI tap never passes
        // through. A sweep latched on TCI keeps judging that tap after a
        // mid-sweep switch to USB; the live mode would say PC Audio and warn.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        audio.setNr2Enabled(true);
        QVERIFY(audio.nr2Enabled());
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        dialog.setTciAudioProbe([] { return true; }); // a TCI client is streaming
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        settle();

        QLabel* hint = nullptr;
        QPushButton* startStop = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("hint")) {
                hint = label;
            }
        }
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Auto Sweep")) {
                startStop = button;
            }
        }
        QVERIFY(hint && startStop);
        const QString nrWarning = QStringLiteral("⚠ Disable");

        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));
        fixture.backend->modeStatus(0, QStringLiteral("USB"), QStringLiteral("med"), 1);
        settle();
        QVERIFY(dialog.isVisible());
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));
        QVERIFY2(!hint->text().startsWith(nrWarning), qPrintable(hint->text()));

        // Stopped, the dialog rewires to the live source, PC Audio, and NR2
        // now masks its tap.
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Auto Sweep"));
        QVERIFY2(hint->text().startsWith(nrWarning), qPrintable(hint->text()));
#else
        QSKIP("HAVE_WEBSOCKETS not set; latched NR test requires TciServer");
#endif
    }

    void tciDeliversAudioNeedsARunningServerAndAnAudioClient()
    {
#ifdef HAVE_WEBSOCKETS
        TciClientInfo idle;
        TciClientInfo streaming;
        streaming.audio = true;
        QVERIFY(!AgcCalibrationDialog::tciDeliversAudio(true, {}));
        QVERIFY(!AgcCalibrationDialog::tciDeliversAudio(true, {idle}));
        QVERIFY(!AgcCalibrationDialog::tciDeliversAudio(false, {streaming}));
        QVERIFY(AgcCalibrationDialog::tciDeliversAudio(true, {idle, streaming}));
#else
        QSKIP("HAVE_WEBSOCKETS not set; TciClientInfo requires TciServer");
#endif
    }

    void calibrationDialogTciNeedsAReceivingClient()
    {
        // DIGU with a DAX channel picks TCI only while a client streams
        // receive audio, and follows clientsChanged() while idle. A running
        // sweep keeps the source it latched.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        bool receiving = false;
        dialog.setTciAudioProbe([&receiving] { return receiving; });
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        settle();

        QLabel* source = nullptr;
        QPushButton* startStop = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("source")) {
                source = label;
            }
        }
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Auto Sweep")) {
                startStop = button;
            }
        }
        QVERIFY(source && startStop);
        const QString pcNoClient =
            QStringLiteral("Audio source: PC Audio (no TCI client is receiving audio)");
        const QString tciSource = QStringLiteral("Audio source: TCI (DIGU/DIGL)");
        QCOMPARE(source->text(), pcNoClient);

        receiving = true;
        emit tci.clientsChanged();
        settle();
        QCOMPARE(source->text(), tciSource);

        // The client stops mid-sweep: the sweep stays on TCI.
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));
        receiving = false;
        emit tci.clientsChanged();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));
        QCOMPARE(source->text(), tciSource);

        // Stopped, it falls back to PC Audio.
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Auto Sweep"));
        QCOMPARE(source->text(), pcNoClient);
#else
        QSKIP("HAVE_WEBSOCKETS not set; TCI availability test requires TciServer");
#endif
    }

    void calibrationDialogSourceSwitchResetsLevel()
    {
        // PC Audio is post-volume and TCI is pre-AF-gain DAX. After a switch,
        // the first point must be the new source's level, not a blend.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        TciServer tci(&fixture.radio);
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, &tci, {});
        dialog.setTciAudioProbe([] { return true; });
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        fixture.backend->modeStatus(0, QStringLiteral("USB"), QStringLiteral("med"), 1);
        settle();
        for (int i = 0; i < 20; ++i) {
            emit audio.levelChanged(1.0f); // PC Audio near 0 dB
        }

        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1, 40);
        settle();
        emit tci.rxLevel(1, 0.01f); // TCI at -40 dB
        QTest::qWait(600);
        const QVector<AgcTCalibrator::Point> curve = dialog.calibrator().curve();
        QCOMPARE(curve.size(), 1);
        // 20 log10(0.01) = -40 dB, from the TCI level alone.
        QVERIFY2(std::abs(curve.constFirst().rmsDb + 40.0f) < 1.0f,
                 qPrintable(QString::number(curve.constFirst().rmsDb)));
#else
        QSKIP("HAVE_WEBSOCKETS not set; source switch test requires TciServer");
#endif
    }

    void calibrationDialogStopEchoKeepsStopMessage()
    {
        // Stop restores the AGC-T it started from. That write's echo is the
        // calibrator's own, so its silent settle must not replace
        // "Calibration stopped." with the manual no-audio notice.
        Fixture fixture;
        AudioEngine audio;
        AgcCalibrationDialog dialog(&fixture.radio, &audio, nullptr, {});
        dialog.setSlice(fixture.radio.slice(0));
        dialog.show();
        settle();
        QLabel* result = nullptr;
        QPushButton* startStop = nullptr;
        for (QLabel* label : dialog.findChildren<QLabel*>()) {
            if (label->property("agcRole").toString() == QStringLiteral("result")) {
                result = label;
            }
        }
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Auto Sweep")) {
                startStop = button;
            }
        }
        QVERIFY(result && startStop);
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));
        // The sweep's first write lands, so the restore is a real change.
        fixture.backend->status(0, AgcThresholdMode::Manual, 100);
        settle();
        startStop->click();
        settle();
        QCOMPARE(result->text(), QStringLiteral("Calibration stopped."));
        fixture.backend->status(0, AgcThresholdMode::Manual, 65); // the restore's echo
        QTest::qWait(600);
        QCOMPARE(result->text(), QStringLiteral("Calibration stopped."));

        // An operator move afterwards still gets the notice.
        fixture.backend->status(0, AgcThresholdMode::Manual, 40);
        QTRY_COMPARE_WITH_TIMEOUT(result->text(),
            QStringLiteral("No audio on this computer yet. Is PC Audio on?"), 2000);
    }

    void calibrationDialogDestroyedMidSweepRestoresWhileSourcesLive()
    {
        // ~MainWindow deletes this dialog before it frees TciServer and
        // AudioEngine (pinned below): its destructor stops the sweep, and
        // onFinished() rewires through both raw pointers. Same order here,
        // so the sanitizer sweep sees that path run against live sources.
#ifdef HAVE_WEBSOCKETS
        Fixture fixture;
        auto tci = std::make_unique<TciServer>(&fixture.radio);
        auto audio = std::make_unique<AudioEngine>();
        auto dialog = std::make_unique<AgcCalibrationDialog>(
            &fixture.radio, audio.get(), tci.get(), AgcCalibrationDialog::NoiseFloorFn{});
        dialog->setTciAudioProbe([] { return true; });
        dialog->setSlice(fixture.radio.slice(0));
        dialog->show();
        fixture.backend->modeStatus(0, QStringLiteral("DIGU"), QStringLiteral("med"), 1);
        settle();

        QPushButton* startStop = nullptr;
        for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Auto Sweep")) {
                startStop = button;
            }
        }
        QVERIFY(startStop);
        startStop->click();
        settle();
        QCOMPARE(startStop->text(), QStringLiteral("Stop"));

        fixture.backend->agcRequests.clear();
        dialog.reset();
        QVERIFY(!fixture.backend->agcRequests.isEmpty());
        QCOMPARE(fixture.backend->agcRequests.constLast().threshold, 65);
        tci.reset();
        audio.reset();
#else
        QSKIP("HAVE_WEBSOCKETS not set; teardown test requires TciServer");
#endif
    }

    void mainWindowDestroysCalibrationDialogBeforeItsSources()
    {
        // MainWindow is not linked into this target, so the teardown order
        // inside ~MainWindow is pinned from the source.
        QFile file(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString source = QString::fromUtf8(file.readAll());
        const qsizetype dtor = source.indexOf(QStringLiteral("MainWindow::~MainWindow()"));
        const qsizetype dialogDelete =
            source.indexOf(QStringLiteral("delete m_agcCalibrationDialog.data();"), dtor);
        const qsizetype tciFree =
            source.indexOf(QStringLiteral("m_session->shutdownTciServer();"), dtor);
        const qsizetype audioFree = source.indexOf(QStringLiteral("delete m_audio;"), dtor);
        QVERIFY(dtor >= 0 && tciFree > dtor && audioFree > dtor);
        QVERIFY2(dialogDelete > dtor && dialogDelete < tciFree && dialogDelete < audioFree,
            "~MainWindow must delete the AGC-T calibration dialog before freeing "
            "TciServer and AudioEngine");
    }

    void initTestCase()
    {
        QVERIFY2(QFile::exists(QStringLiteral(":/themes/default-dark.json")),
            "Link THEME_TEST_RESOURCES so screenshots use the production theme resources");
    }

    void compactLayoutFitsDock_data()
    {
        QTest::addColumn<QString>("fontFamily");
        // DejaVu Sans is the row that needs the compact preset padding (#6641);
        // a host without it runs only rows that already fit.
        QTest::newRow("system") << QString();
        const QStringList families = QFontDatabase::families();
        for (const QString& family : {QStringLiteral("DejaVu Sans"),
                                      QStringLiteral("Liberation Sans")}) {
            if (families.contains(family)) {
                QTest::newRow(qPrintable(family)) << family;
            }
        }
    }

    void compactLayoutFitsDock()
    {
        QFETCH(QString, fontFamily);
        const QFont defaultFont = QApplication::font();
        const auto restoreFont = qScopeGuard([defaultFont] {
            QApplication::setFont(defaultFont);
        });
        if (!fontFamily.isEmpty()) {
            QFont font = defaultFont;
            font.setFamily(fontFamily);
            QApplication::setFont(font);
        }

        Fixture fixture;
        QVERIFY(fixture.capture(QStringLiteral("compact-layout-")
            + (fontFamily.isEmpty() ? QStringLiteral("system") : fontFamily)));
        QCOMPARE(fixture.rx.width(), 248);
        QVERIFY(fixture.rx.minimumSizeHint().width() <= 248);
        QVERIFY(fixture.threshold());
        QVERIFY(fixture.threshold()->width() >= 60);
        for (QWidget* widget : fixture.rx.findChildren<QWidget*>()) {
            if (widget->isVisibleTo(&fixture.rx) && !widget->isWindow()) {
                const QRect bounds(widget->mapTo(&fixture.rx, QPoint()), widget->size());
                QVERIFY2(fixture.rx.rect().contains(bounds),
                    qPrintable(widget->accessibleName().isEmpty()
                        ? QString::fromLatin1(widget->metaObject()->className())
                        : widget->accessibleName()));
            }
        }

        const QStringList presets{QStringLiteral("1.8K"), QStringLiteral("2.1K"),
            QStringLiteral("2.4K"), QStringLiteral("2.7K"),
            QStringLiteral("2.9K"), QStringLiteral("3.3K")};
        int presetCount = 0;
        for (QPushButton* button : fixture.rx.findChildren<QPushButton*>()) {
            if (presets.contains(button->text())) {
                ++presetCount;
                QVERIFY(button->isVisibleTo(&fixture.rx));
                QStyleOptionButton option;
                option.initFrom(button);
                const QRect contents = button->style()->subElementRect(
                    QStyle::SE_PushButtonContents, &option, button);
                QVERIFY(button->fontMetrics().horizontalAdvance(button->text())
                    <= contents.width());
            }
        }
        QCOMPARE(presetCount, 6);
    }

    void bridgeClickWaitsForConfirmedStatus()
    {
        Fixture fixture;
        QVERIFY(fixture.button());
        QCOMPARE(fixture.button()->accessibleName(), QStringLiteral("Auto AGC-T"));
        QVERIFY(fixture.button()->focusPolicy() & Qt::TabFocus);
        QVERIFY(fixture.button()->isEnabled());
        QVERIFY(!fixture.button()->isChecked());
        QCOMPARE(fixture.rx.width(), 248);
        QVERIFY(fixture.threshold());
        QCOMPARE(fixture.button()->text(), QStringLiteral("A"));
        QVERIFY(fixture.threshold()->width() >= 60);
        QCOMPARE(fixture.button()->parentWidget(), fixture.threshold()->parentWidget());
        QVERIFY(fixture.button()->x() > fixture.threshold()->geometry().right());
        QVERIFY(std::abs(fixture.button()->geometry().center().y()
            - fixture.threshold()->geometry().center().y()) <= 2);
        QVERIFY(fixture.button()->toolTip().contains(QStringLiteral("Move the slider")));
        QVERIFY(fixture.capture(QStringLiteral("01-manual-before")));

        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QCOMPARE(fixture.backend->modeRequests.size(), 1);
        QCOMPARE(fixture.backend->modeRequests.first().sliceId, 0);
        QCOMPARE(fixture.backend->modeRequests.first().mode, AgcThresholdMode::Automatic);
        QCOMPARE(fixture.backend->wireCommands,
            QStringList{QStringLiteral("slice set 0 agc_threshold_mode=auto")});
        QVERIFY(!fixture.button()->isChecked());
        QCOMPARE(fixture.slice().value(QStringLiteral("agcThresholdMode")).toString(), QStringLiteral("manual"));
        QVERIFY(fixture.slice().value(QStringLiteral("automaticAgcThresholdActiveOrPending")).toBool());
        QVERIFY(fixture.capture(QStringLiteral("02-automatic-request-pending")));

        fixture.backend->status(0, AgcThresholdMode::Automatic, 41);
        QCOMPARE(fixture.backend->modeRequests.size(), 1);
        QCOMPARE(fixture.backend->wireCommands.size(), 1);
        QVERIFY(fixture.backend->agcRequests.isEmpty());
        QVERIFY(fixture.buttonNode().value(QStringLiteral("checked")).toBool());
        QCOMPARE(fixture.threshold()->value(), 41);
        QCOMPARE(fixture.slice().value(QStringLiteral("agcThresholdMode")).toString(), QStringLiteral("auto"));
        const QJsonObject property = fixture.send("get slice 0 agcThresholdMode");
        QVERIFY(property.value(QStringLiteral("ok")).toBool());
        QCOMPARE(property.value(QStringLiteral("value")).toString(), QStringLiteral("auto"));
        QVERIFY(fixture.capture(QStringLiteral("03-automatic-confirmed")));

        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QCOMPARE(fixture.backend->modeRequests.size(), 2);
        QCOMPARE(fixture.backend->modeRequests.last().mode, AgcThresholdMode::Manual);
        const QStringList expectedCommands{QStringLiteral("slice set 0 agc_threshold_mode=auto"),
            QStringLiteral("slice set 0 agc_threshold_mode=manual")};
        QCOMPARE(fixture.backend->wireCommands, expectedCommands);
        QVERIFY(fixture.button()->isChecked());
        fixture.backend->status(0, AgcThresholdMode::Manual, 52);
        QVERIFY(!fixture.button()->isChecked());
        QCOMPARE(fixture.threshold()->value(), 52);
        QCOMPARE(fixture.backend->modeRequests.size(), 2);
        QCOMPARE(fixture.backend->wireCommands, expectedCommands);
        QVERIFY(fixture.backend->agcRequests.isEmpty());
        QVERIFY(fixture.capture(QStringLiteral("04-manual-confirmed")));
    }

    void sliceFlagAndRxShareAutomaticThresholdState()
    {
        Fixture fixture;
        VfoWidget flag;
        flag.setObjectName(QStringLiteral("autoAgcProofFlag"));
        flag.setRadioModel(&fixture.radio);
        flag.setSlice(fixture.radio.slice(0));
        flag.show();
        QPushButton* audioTab = nullptr;
        QSlider* threshold = nullptr;
        for (QPushButton* button : flag.findChildren<QPushButton*>()) {
            if (button->accessibleName() == QStringLiteral("Audio settings")) {
                audioTab = button;
            }
        }
        for (QSlider* slider : flag.findChildren<QSlider*>()) {
            if (slider->accessibleName() == QStringLiteral("AGC threshold")) {
                threshold = slider;
            }
        }
        auto* automatic = flag.findChild<QPushButton*>(QStringLiteral("sliceAutoAgcT"));
        QVERIFY(audioTab && threshold && automatic);
        audioTab->click();
        settle();
        QCOMPARE(automatic->text(), QStringLiteral("Auto"));
        QVERIFY(automatic->isVisible() && automatic->isEnabled());
        QVERIFY(automatic->toolTip().contains(QStringLiteral("AGC speed stays independent")));
        QVERIFY(threshold->isEnabled());
        QVERIFY(automatic->mapTo(&flag, QPoint()).x()
            > threshold->mapTo(&flag, threshold->rect().topRight()).x());

        QVERIFY(fixture.send("invoke sliceAutoAgcT click").value(QStringLiteral("ok")).toBool());
        QCOMPARE(fixture.backend->modeRequests.size(), 1);
        QVERIFY(!automatic->isChecked() && !fixture.button()->isChecked());
        fixture.backend->status(0, AgcThresholdMode::Automatic, 41);
        QVERIFY(automatic->isChecked() && fixture.button()->isChecked());
        QVERIFY(threshold->isEnabled() && fixture.threshold()->isEnabled());
        QCOMPARE(threshold->value(), 41);
        QCOMPARE(fixture.radio.slice(0)->agcMode(), QStringLiteral("med"));
        QVERIFY(fixture.backend->agcRequests.isEmpty());
        QVERIFY(fixture.capture(QStringLiteral("05-slice-automatic"), flag.objectName()));

        QTest::keyClick(threshold, Qt::Key_Left);
        QCOMPARE(fixture.backend->agcRequests.size(), 1);
        QCOMPARE(fixture.backend->agcRequests.last().field, SliceAgcRequest::Field::Threshold);
        QCOMPARE(fixture.backend->agcRequests.last().threshold, 40);
        QVERIFY(automatic->isChecked() && fixture.button()->isChecked());
        fixture.backend->status(0, AgcThresholdMode::Manual, 40);
        QVERIFY(!automatic->isChecked() && !fixture.button()->isChecked());
        QVERIFY(fixture.capture(QStringLiteral("06-slice-manual"), flag.objectName()));

        RadioModelWakeTestAccess::license(fixture.radio, true, false);
        QVERIFY(!automatic->isEnabled());
        QVERIFY(!automatic->accessibleDescription().isEmpty());
        RadioModelWakeTestAccess::license(fixture.radio, true, true);
        QVERIFY(automatic->isEnabled());
        flag.setSlice(nullptr);
        QVERIFY(!automatic->isEnabled() && !automatic->isChecked());
    }

    // The AGC-T tooltips print the gain a declared agcGainScale gives the knob,
    // in the RX applet and on the slice flag, both for the radio's value and
    // during a drag. Without a scale they print the knob with "dB", as before.
    void thresholdTooltipsPrintTheDeclaredGain()
    {
        Fixture fixture;
        VfoWidget flag;
        flag.setRadioModel(&fixture.radio);
        flag.setSlice(fixture.radio.slice(0));
        QSlider* rxSlider = fixture.threshold();
        QSlider* flagSlider = nullptr;
        for (QSlider* slider : flag.findChildren<QSlider*>()) {
            if (slider->accessibleName() == QStringLiteral("AGC threshold")) {
                flagSlider = slider;
            }
        }
        QVERIFY(rxSlider && flagSlider);
        QVERIFY(rxSlider->toolTip().startsWith(QStringLiteral("AGC Threshold: 65 dB")));
        QVERIFY(flagSlider->toolTip().startsWith(QStringLiteral("AGC Threshold: 65 dB")));

        fixture.backend->gainScale = RadioCapabilities::AgcGainScale{-20.0, 1.4};
        fixture.backend->agcStatus(QStringLiteral("med"), 71, 29);
        QCOMPARE(rxSlider->toolTip(), QStringLiteral("AGC Threshold: 79.4 dB"));
        QCOMPARE(flagSlider->toolTip(), QStringLiteral("AGC Threshold: 79.4 dB"));

        ToolTipLog rxLog(rxSlider);
        ToolTipLog flagLog(flagSlider);
        rxSlider->setValue(70);
        flagSlider->setValue(69);
        QVERIFY2(rxLog.seen.contains(QStringLiteral(
                     "AGC Threshold: 78.0 dB\nRight-click to calibrate against the noise floor")),
            qPrintable(rxLog.seen.join(QStringLiteral(" | "))));
        QVERIFY2(flagLog.seen.contains(QStringLiteral("AGC Threshold: 76.6 dB")),
            qPrintable(flagLog.seen.join(QStringLiteral(" | "))));

        fixture.backend->agcStatus(QStringLiteral("off"), 71, 29);
        QCOMPARE(rxSlider->toolTip(), QStringLiteral("AGC Off Level: 20.6 dB"));
        QCOMPARE(flagSlider->toolTip(), QStringLiteral("AGC Off Level: 20.6 dB"));
    }

    void refusedDispatchDoesNotEngageAutomaticMode()
    {
        Fixture fixture;
        fixture.backend->refuse = true;
        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QCOMPARE(fixture.backend->modeRequests.size(), 1);
        QVERIFY(fixture.backend->wireCommands.isEmpty());
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(!fixture.radio.slice(0)->automaticAgcThresholdActiveOrPending());
        QCOMPARE(fixture.radio.slice(0)->agcThresholdMode(),
            std::optional<AgcThresholdMode>(AgcThresholdMode::Manual));
    }

    void pendingRequestCanBeCancelledWithoutAnEcho()
    {
        Fixture fixture;
        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QVERIFY(fixture.radio.slice(0)->automaticAgcThresholdActiveOrPending());
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(fixture.button()->accessibleDescription().contains(QStringLiteral("Waiting")));
        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QVERIFY(!fixture.radio.slice(0)->automaticAgcThresholdActiveOrPending());
        QVERIFY(!fixture.button()->isChecked());
        QCOMPARE(fixture.backend->wireCommands, QStringList({
            QStringLiteral("slice set 0 agc_threshold_mode=auto"),
            QStringLiteral("slice set 0 agc_threshold_mode=manual")}));
    }

    void manualSliderCancelsPendingAutoWithConfirmedControls()
    {
        Fixture fixture;
        SliceModel* slice = fixture.radio.slice(0);
        QVERIFY(slice->receiveControlsConfirmed());
        QVERIFY(fixture.click().value(QStringLiteral("ok")).toBool());
        QVERIFY(slice->automaticAgcThresholdActiveOrPending());
        QVERIFY(!slice->agcCalibrationAllowed());

        QTest::keyClick(fixture.threshold(), Qt::Key_Left);
        QCOMPARE(fixture.backend->agcRequests.size(), 1);
        QCOMPARE(fixture.backend->agcRequests.last().field, SliceAgcRequest::Field::Threshold);
        QCOMPARE(fixture.backend->agcRequests.last().threshold, 64);
        QVERIFY(!slice->automaticAgcThresholdActiveOrPending());
        QVERIFY(slice->agcCalibrationAllowed());
        QCOMPARE(slice->agcThreshold(), 65);
        QCOMPARE(fixture.threshold()->value(), 65);
        QCOMPARE(fixture.backend->wireCommands,
            QStringList{QStringLiteral("slice set 0 agc_threshold_mode=auto")});

        fixture.backend->status(0, AgcThresholdMode::Manual, 64);
        QCOMPARE(slice->agcThreshold(), 64);
        QCOMPARE(fixture.threshold()->value(), 64);
    }

    void spaceKeyRequestsAutomaticMode()
    {
        Fixture fixture;
        QVERIFY(fixture.button());
        QTest::keyClick(fixture.button(), Qt::Key_Space);
        settle();
        QCOMPARE(fixture.backend->modeRequests.size(), 1);
        QCOMPARE(fixture.backend->modeRequests.first().mode, AgcThresholdMode::Automatic);
        QCOMPARE(fixture.backend->wireCommands,
            QStringList{QStringLiteral("slice set 0 agc_threshold_mode=auto")});
        QVERIFY(!fixture.button()->isChecked());
        fixture.backend->status(0, AgcThresholdMode::Automatic);
        QVERIFY(fixture.button()->isChecked());
    }

    void unavailableControlsRemainVisible_data()
    {
        QTest::addColumn<bool>("supported");
        QTest::addColumn<int>("license");
        QTest::addColumn<bool>("knownMode");
        QTest::newRow("unsupported") << false << 2 << true;
        QTest::newRow("unknown-license") << true << 0 << true;
        QTest::newRow("unlicensed") << true << 1 << true;
        QTest::newRow("unknown-mode") << true << 2 << false;
    }

    void unavailableControlsRemainVisible()
    {
        QFETCH(bool, supported);
        QFETCH(int, license);
        QFETCH(bool, knownMode);
        Fixture fixture(supported, false, knownMode
            ? std::optional<AgcThresholdMode>(AgcThresholdMode::Manual) : std::nullopt);
        RadioModelWakeTestAccess::license(fixture.radio, license > 0, license == 2);
        const QJsonObject button = fixture.buttonNode();
        QVERIFY(!button.isEmpty());
        QVERIFY(fixture.button()->isVisible());
        QVERIFY(!button.value(QStringLiteral("enabled")).toBool());
        QVERIFY(!fixture.button()->accessibleDescription().isEmpty());
        QVERIFY(!fixture.button()->toolTip().isEmpty());
        const QJsonObject reply = fixture.click();
        QVERIFY(!reply.value(QStringLiteral("ok")).toBool());
        QVERIFY(reply.value(QStringLiteral("disabled")).toBool());
        QVERIFY(fixture.backend->modeRequests.isEmpty());
        QVERIFY(fixture.backend->wireCommands.isEmpty());
        QVERIFY(fixture.backend->agcRequests.isEmpty());
        if (!knownMode) {
            QVERIFY(fixture.slice().value(QStringLiteral("agcThresholdMode")).isNull());
            fixture.backend->status(0, AgcThresholdMode::Manual);
            QVERIFY(fixture.button()->isEnabled());
            QVERIFY(fixture.backend->modeRequests.isEmpty());
        }
    }

    void sliceSelectionAndReplacementDiscardOldState()
    {
        Fixture fixture(true, true, AgcThresholdMode::Automatic);
        QVERIFY(fixture.button()->isChecked());
        fixture.backend->status(1, AgcThresholdMode::Manual);
        fixture.rx.setSlice(fixture.radio.slice(1));
        QVERIFY(!fixture.button()->isChecked());
        fixture.backend->status(0, AgcThresholdMode::Automatic, 33);
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(fixture.backend->modeRequests.isEmpty());
        QVERIFY(fixture.backend->wireCommands.isEmpty());
        fixture.rx.setSlice(nullptr);
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(!fixture.button()->isEnabled());

        QPointer<SliceModel> old = fixture.radio.slice(0);
        auto replacement = std::make_unique<AutoAgcBackend>();
        fixture.backend = replacement.get();
        fixture.radio.setBackendForTest(std::move(replacement), QStringLiteral("auto-agct-test"));
        RadioModelWakeTestAccess::license(fixture.radio, true, true);
        fixture.backend->status(0, std::nullopt);
        // Retired objects may survive until DeferredDelete delivery. Their
        // numeric id must never authorize a request to its new-session owner.
        QVERIFY(old.data() != fixture.radio.slice(0));
        if (old) {
            old->requestAgcThresholdMode(AgcThresholdMode::Automatic);
            old->requestAgcThresholdMode(AgcThresholdMode::Manual);
        }
        QVERIFY(fixture.backend->modeRequests.isEmpty());
        QVERIFY(fixture.backend->wireCommands.isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        fixture.rx.setRadioModel(&fixture.radio);
        fixture.rx.setSlice(fixture.radio.slice(0));
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(!fixture.button()->isEnabled());
        QVERIFY(fixture.click().value(QStringLiteral("disabled")).toBool());
        QVERIFY(fixture.backend->modeRequests.isEmpty());
        fixture.backend->status(0, AgcThresholdMode::Manual);
        QVERIFY(fixture.button()->isEnabled());
        QVERIFY(!fixture.button()->isChecked());
        QVERIFY(fixture.backend->modeRequests.isEmpty());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("auto-agc-threshold-gui"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    AutoAgcThresholdGuiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "auto_agc_threshold_gui_test.moc"
