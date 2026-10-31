// Socket-free proof of the real RX control and automation dispatcher. Status
// enters through normalized backend deltas; the fixture never starts a radio,
// listener, audio device, or transmitter. Optional artifacts describe this
// synthetic fixture explicitly and do not claim licensed-hardware convergence.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
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
    QVector<ModeRequest> modeRequests;
    QVector<SliceAgcRequest> agcRequests;
    QStringList wireCommands;
    FlexBackend flexEncoder;

    AutoAgcBackend()
    {
        flexEncoder.setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
        flexEncoder.setAutoAgcThresholdLicenseProvider([] { return true; });
        flexEncoder.setSliceCommandSink([this](const QString& command) {
            wireCommands.append(command);
        });
    }

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.hasAgcThreshold = true;
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
};

void settle()
{
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

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
        AgcCalibrationDialog dialog(&fixture.radio, &audio, {});
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
            emit audio.levelChanged(static_cast<float>(value) / 100.0f);
            fixture.backend->status(0, AgcThresholdMode::Manual, value);
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
            emit audio.levelChanged(static_cast<float>(value) / 100.0f);
            fixture.backend->status(0, AgcThresholdMode::Manual, value);
            QTest::qWait(400);
        }
        QVERIFY(result->text().startsWith(QStringLiteral("Recommended AGC-T")));
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
