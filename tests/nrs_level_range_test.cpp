// Firmware identity, operator requests and passive NRS readback through the
// production model/backend/widget bindings. No peer, socket dial or radio.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/AutomationServer.h"
#include "core/QsoRecorder.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QScopeGuard>
#include <QLabel>
#include <QLayout>
#include <QMap>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QStyle>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <array>
#include <utility>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    {
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
};

// Shared firmware/session access; reuses an existing friend without a new seam.
class RadioNicknameTestAccess {
public:
    static void firmware(RadioModel& radio, const QString& version)
    {
        radio.adoptSoftwareVersion(version);
        radio.m_model = QStringLiteral("FLEX-8600");
        emit radio.infoChanged();
    }
    static void protocol(RadioModel& radio)
    {
        radio.onVersionReceived(QStringLiteral("1.4.0.0"));
    }
    static void disconnect(RadioModel& radio)
    {
        radio.m_lastInfo.address = QHostAddress(QHostAddress::LocalHost);
        radio.m_intentionalDisconnect = false;
        radio.onDisconnected();
        radio.m_reconnectTimer.stop();
    }
    static void retryWithoutTransport(RadioModel& radio)
    {
        // Retain the backend but detach its admission pointers while the real
        // retry handler runs, so no dial can be enqueued on either transport.
        auto backend = std::move(radio.m_backend);
        RadioConnection* connection = std::exchange(radio.m_connection, nullptr);
        const auto restore = qScopeGuard([&] {
            radio.m_backend = std::move(backend);
            radio.m_connection = connection;
        });
        radio.triggerAutoReconnectForTest();
        radio.m_reconnectTimer.stop();
    }
};

class RadioConnectionSessionTestAccess {
public:
    static void connected(RadioConnection& connection, bool connected)
    {
        // Only the atomic admission predicate changes. No handshake, timer or
        // socket callback is invoked, and the connection is never dialed.
        connection.m_state.store(connected ? ConnectionState::Connected
                                           : ConnectionState::Disconnected);
    }
};

class WanConnectionTestAccess {
public:
    static void connected(WanConnection& connection, bool connected)
    {
        connection.m_connected = connected;
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
QMap<QString, QString> sliceStatus(int level = 0)
{
    return {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.2")},
            {QStringLiteral("mode"), QStringLiteral("USB")},
            {QStringLiteral("nrs"), QStringLiteral("1")},
            {QStringLiteral("nrs_level"), QString::number(level)}};
}

struct Fixture {
    RadioModel radio;
    FlexBackend* backend = nullptr;
    QStringList commands;

    bool init(const QString& version = QStringLiteral("4.2.20.41343"))
    {
        if (!radio.rebuildBackendForTest(QStringLiteral("flex"))) {
            return false;
        }
        backend = qobject_cast<FlexBackend*>(radio.backend());
        if (!backend) {
            return false;
        }
        RadioConnectionSessionTestAccess::connected(*backend->connection(), true);
        backend->setSliceCommandSink([this](const QString& command) {
            commands.append(command);
        });
        RadioNicknameTestAccess::firmware(radio, version);
        radio.handleSliceStatusForTest(0, sliceStatus());
        commands.clear();
        return radio.slice(0) != nullptr;
    }

    ~Fixture()
    {
        // The recorder cannot outlive its captured storage during teardown.
        if (backend) {
            backend->setSliceCommandSink({});
            RadioConnectionSessionTestAccess::connected(*backend->connection(), false);
        }
    }
};

QLabel* valueLabel(QSlider& slider)
{
    QLayout* layout = slider.parentWidget()->layout();
    if (!layout) {
        return nullptr;
    }
    const int index = layout->indexOf(&slider);
    return index >= 0 && index + 1 < layout->count()
        ? qobject_cast<QLabel*>(layout->itemAt(index + 1)->widget()) : nullptr;
}
QJsonObject findWidget(const QJsonObject& node, const QString& name)
{
    if (node.value(QStringLiteral("objectName")).toString() == name) {
        return node;
    }
    for (const QString& key : {QStringLiteral("roots"), QStringLiteral("children")}) {
        for (const QJsonValue& child : node.value(key).toArray()) {
            const QJsonObject match = findWidget(child.toObject(), name);
            if (!match.isEmpty()) {
                return match;
            }
        }
    }
    return {};
}

bool writeEvidence(const QString& path, const QJsonObject& data)
{
    QFile file(path);
    const QByteArray bytes = QJsonDocument(data).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
} // namespace

class NrsLevelRangeTest : public QObject {
    Q_OBJECT

private slots:
    void firmwareAndEncoding()
    {
        Fixture f;
        QVERIFY(f.init());
        test::SeamThreadAffinityProbe probe(f.backend);
        test::attachAllSeamSignals(probe);
        SliceModel* slice = f.radio.slice(0);
        QSignalSpy capabilities(&f.radio, &RadioModel::capabilitiesChanged);
        struct VersionCase { const char* version; int maximum; };
        const std::array<VersionCase, 10> cases{{
            {"4.2.20.41343", 100}, {"4.3.15", 100},
            {"4.3.16", 50}, {"4.3.16.42034", 50}, {"5.0.0.0", 50},
            {"", 100}, {"unknown", 100}, {"4.3.16junk", 100},
            {"4.3.16.42034-beta", 100}, {"1.4.0.0", 100}}};
        for (const VersionCase& entry : cases) {
            const int previous = slice->nrsLevelMaximum();
            capabilities.clear();
            f.commands.clear();
            RadioNicknameTestAccess::firmware(f.radio, QString::fromLatin1(entry.version));
            QCOMPARE(slice->nrsLevelMaximum(), entry.maximum);
            QVERIFY(f.backend->capabilities().nrsLevelControl.has_value());
            QCOMPARE(f.backend->capabilities().nrsLevelControl->levelMaximum, entry.maximum);
            QCOMPARE(capabilities.size(), previous == entry.maximum ? 0 : 1);
            QVERIFY(f.commands.isEmpty());
            RadioNicknameTestAccess::firmware(f.radio, QString::fromLatin1(entry.version));
            QCOMPARE(capabilities.size(), previous == entry.maximum ? 0 : 1);

            slice->setNrsLevel(0);
            f.commands.clear();
            slice->setNrsLevel(100);
            QCOMPARE(slice->nrsLevel(), entry.maximum);
            QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=%1")
                .arg(entry.maximum)});
            f.commands.clear();
            QCOMPARE(f.backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nrs,
                SliceDspRequest::Field::Level, true, 72}), ReceiveDispatch::Dispatched);
            QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=%1")
                .arg(entry.maximum == 50 ? 50 : 72)});
        }
        f.commands.clear();
        QCOMPARE(f.backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nrs,
            SliceDspRequest::Field::Level, true, 101}), ReceiveDispatch::Unsupported);
        QVERIFY(f.commands.isEmpty());
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16"));
        f.commands.clear();
        QCOMPARE(f.backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nrl,
            SliceDspRequest::Field::Level, true, 100}), ReceiveDispatch::Dispatched);
        QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 lms_nr_level=100")});
        QVERIFY(probe.violations().isEmpty());
        QVERIFY(probe.afterDisconnect().isEmpty());
    }

    void passiveStatusAndWidgetRange()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* slice = f.radio.slice(0);
        f.backend->decodeSliceStatus(0, sliceStatus(80));
        VfoWidget vfo;
        vfo.setHasExtendedDsp(true);
        vfo.setRadioModel(&f.radio);
        vfo.setSlice(slice);
        QSlider* slider = vfo.findChild<QSlider*>(QStringLiteral("dspLevelSlider"));
        QVERIFY(slider);
        QLabel* label = valueLabel(*slider);
        QVERIFY(label);
        QCOMPARE(slider->maximum(), 100);
        QCOMPARE(slider->value(), 80);
        QCOMPARE(label->text(), QStringLiteral("80"));
        QSignalSpy requests(slice, &SliceModel::receiveDspRequested);
        QSignalSpy sliderChanges(slider, &QSlider::valueChanged);
        f.commands.clear();

        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16.42034"));
        QCOMPARE(slider->maximum(), 50);
        QCOMPARE(slider->value(), 50);
        QCOMPARE(slice->nrsLevel(), 80);
        QCOMPARE(label->text(), QStringLiteral("80"));
        QVERIFY(sliderChanges.isEmpty());
        QVERIFY(requests.isEmpty());
        QVERIFY(f.commands.isEmpty());
        for (const int reported : {50, 80}) {
            f.backend->decodeSliceStatus(0, sliceStatus(reported));
            QCOMPARE(slice->nrsLevel(), reported);
            QCOMPARE(label->text(), QString::number(reported));
            QCOMPARE(slider->maximum(), 50);
            QVERIFY(requests.isEmpty());
            QVERIFY(f.commands.isEmpty());
        }

        QPushButton* nr = vfo.findChild<QPushButton*>(QStringLiteral("dspNRBtn"));
        QPushButton* nrs = vfo.findChild<QPushButton*>(QStringLiteral("dspNRSBtn"));
        QVERIFY(nr);
        QVERIFY(nrs);
        nr->click();
        QCOMPARE(slider->maximum(), 100);
        nr->click();
        QCOMPARE(slider->maximum(), 50);
        nrs->click();
        nrs->click();
        QCOMPARE(slider->maximum(), 50);
        QCOMPARE(f.commands, (QStringList{QStringLiteral("slice set 0 nr=1"),
                                         QStringLiteral("slice set 0 nr=0"),
                                         QStringLiteral("slice set 0 speex_nr=0"),
                                         QStringLiteral("slice set 0 speex_nr=1")}));
        QCOMPARE(requests.size(), 4);
        f.commands.clear();
        requests.clear();
        slider->setValue(49);
        QCOMPARE(slice->nrsLevel(), 49);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=49")});

        f.commands.clear();
        requests.clear();
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.2.20"));
        QCOMPARE(slider->maximum(), 100);
        QCOMPARE(slice->nrsLevel(), 49);
        QVERIFY(requests.isEmpty());
        QVERIFY(f.commands.isEmpty());
    }

    void simulatedBridgeProof()
    {
        // Real production bridge dispatch, widgets, model and Flex encoder;
        // only firmware identity/status and the command transport are injected.
        // start() is never called: there is no listener, peer or radio traffic.
        Fixture f;
        QVERIFY(f.init(QStringLiteral("4.3.16.42034")));
        SliceModel* slice = f.radio.slice(0);
        VfoWidget vfo;
        vfo.setHasExtendedDsp(true);
        vfo.setRadioModel(&f.radio);
        vfo.setSlice(slice);
        vfo.show();
        AutomationServer bridge;
        bridge.setRadioModel(&f.radio);
        QVERIFY(!bridge.isRunning());
        QVERIFY(!bridge.txAllowed());

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString configured = qEnvironmentVariable("AETHER_NRS_PROOF_DIR");
        const QDir proofDir(configured.isEmpty() ? temporary.path() : configured);
        QVERIFY(QDir().mkpath(proofDir.absolutePath()));
        QJsonArray transcript;
        const auto request = [&](const QJsonObject& command) {
            const QJsonObject reply = AutomationServerTestAccess::request(bridge, command);
            transcript.append(QJsonObject{{QStringLiteral("request"), command},
                                          {QStringLiteral("reply"), reply}});
            for (int i = 0; i < 8; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            if (!reply.value(QStringLiteral("ok")).toBool()) {
                qWarning().noquote() << QJsonDocument(reply).toJson(QJsonDocument::Compact);
            }
            return reply;
        };
        const auto invoke = [&](const QString& target, const QString& action,
                                const QString& value = QString()) {
            return request({{QStringLiteral("cmd"), QStringLiteral("invoke")},
                            {QStringLiteral("target"), target},
                            {QStringLiteral("action"), action},
                            {QStringLiteral("value"), value}});
        };
        const auto capture = [&](const QString& name) {
            const QJsonObject tree = request({{QStringLiteral("cmd"), QStringLiteral("dumpTree")}});
            const QJsonObject screenshot = request({{QStringLiteral("cmd"), QStringLiteral("grab")},
                {QStringLiteral("target"), QStringLiteral("VfoWidgetRoot")},
                {QStringLiteral("path"), proofDir.filePath(name + QStringLiteral(".png"))}});
            const QJsonObject slider = findWidget(tree, QStringLiteral("dspLevelSlider"));
            const QJsonObject evidence{
                {QStringLiteral("kind"), QStringLiteral("in-process simulation; no radio or socket")},
                {QStringLiteral("firmware"), f.radio.softwareVersion()},
                {QStringLiteral("slider"), slider},
                {QStringLiteral("modelLevel"), slice->nrsLevel()},
                {QStringLiteral("encodedCommands"), QJsonArray::fromStringList(f.commands)},
                {QStringLiteral("screenshot"), screenshot},
                {QStringLiteral("transcript"), transcript}};
            const bool saved = writeEvidence(proofDir.filePath(name + QStringLiteral(".json")), evidence);
            return saved && tree.value(QStringLiteral("ok")).toBool()
                && screenshot.value(QStringLiteral("ok")).toBool() ? slider : QJsonObject{};
        };

        QVERIFY(invoke(QStringLiteral("DSP settings (DSP active)"), QStringLiteral("click"))
                    .value(QStringLiteral("ok")).toBool());
        QVERIFY(invoke(QStringLiteral("dspLevelSlider"), QStringLiteral("setValue"), QStringLiteral("25"))
                    .value(QStringLiteral("ok")).toBool());
        f.commands.clear();
        QVERIFY(invoke(QStringLiteral("dspLevelSlider"), QStringLiteral("setValue"), QStringLiteral("100"))
                    .value(QStringLiteral("ok")).toBool());
        const QJsonObject modern = capture(QStringLiteral("modern"));
        QVERIFY(!modern.isEmpty());
        QVERIFY(modern.value(QStringLiteral("visible")).toBool());
        QCOMPARE(modern.value(QStringLiteral("range")).toObject().value(QStringLiteral("max")).toInt(), 50);
        QCOMPARE(modern.value(QStringLiteral("value")).toString(), QStringLiteral("50"));
        QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=50")});

        f.commands.clear();
        f.backend->decodeSliceStatus(0, sliceStatus(17));
        const QJsonObject readback = capture(QStringLiteral("readback"));
        QVERIFY(!readback.isEmpty());
        QCOMPARE(readback.value(QStringLiteral("value")).toString(), QStringLiteral("17"));
        QCOMPARE(slice->nrsLevel(), 17);
        QVERIFY(f.commands.isEmpty());
        f.backend->decodeSliceStatus(0, sliceStatus(50));
        QCOMPARE(slice->nrsLevel(), 50);
        QVERIFY(f.commands.isEmpty());

        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.2.20.41343"));
        QVERIFY(f.commands.isEmpty());
        QVERIFY(invoke(QStringLiteral("dspLevelSlider"), QStringLiteral("setValue"), QStringLiteral("100"))
                    .value(QStringLiteral("ok")).toBool());
        const QJsonObject legacy = capture(QStringLiteral("legacy"));
        QVERIFY(!legacy.isEmpty());
        QCOMPARE(legacy.value(QStringLiteral("range")).toObject().value(QStringLiteral("max")).toInt(), 100);
        QCOMPARE(legacy.value(QStringLiteral("value")).toString(), QStringLiteral("100"));
        QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=100")});
        f.commands.clear();
        f.backend->decodeSliceStatus(0, sliceStatus(80));
        QVERIFY(f.commands.isEmpty());
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16.42034"));
        const QJsonObject changed = capture(QStringLiteral("range-change"));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(changed.value(QStringLiteral("range")).toObject().value(QStringLiteral("max")).toInt(), 50);
        QCOMPARE(changed.value(QStringLiteral("value")).toString(), QStringLiteral("50"));
        QCOMPARE(slice->nrsLevel(), 80);
        QVERIFY(f.commands.isEmpty());
        QVERIFY(!bridge.isRunning());
        QVERIFY(!bridge.txAllowed());
    }

    void disconnectedIdentityDoesNotEmitBackendSignals()
    {
        Fixture f;
        QVERIFY(f.init(QStringLiteral("4.3.16.42034")));
        test::SeamThreadAffinityProbe probe(f.backend);
        test::attachAllSeamSignals(probe);
        RadioConnectionSessionTestAccess::connected(*f.backend->connection(), false);
        emit f.backend->disconnected();
        RadioNicknameTestAccess::disconnect(f.radio);
        QVERIFY(probe.violations().isEmpty());
        QVERIFY2(probe.afterDisconnect().isEmpty(),
                 qPrintable(probe.afterDisconnect().join(QStringLiteral(", "))));
        QCOMPARE(f.radio.slice(0)->nrsLevelMaximum(), 100);
        QVERIFY(f.commands.isEmpty());
    }

    void protocolIdentityIsNotFirmware()
    {
        Fixture f;
        QVERIFY(f.init(QString()));
        RadioNicknameTestAccess::protocol(f.radio);
        QCOMPARE(f.radio.protocolVersion(), QStringLiteral("1.4.0.0"));
        QVERIFY(f.radio.softwareVersion().isEmpty());
        QCOMPARE(f.radio.slice(0)->nrsLevelMaximum(), 100);
        QVERIFY(f.commands.isEmpty());
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16.42034"));
        QCOMPARE(f.radio.slice(0)->nrsLevelMaximum(), 50);
        QVERIFY(f.commands.isEmpty());
    }

    void wanIdentityDoesNotReuseLanFirmware()
    {
        // The WAN socket stays inert: connectViaWan only attaches an already
        // constructed transport; WanConnection::connectToRadio is never called.
        WanConnection wan;
        Fixture f;
        const QString lanVersion = QStringLiteral("4.3.16.42034");
        QVERIFY(f.init(lanVersion));
        f.radio.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 0);
        QVERIFY(f.radio.softwareVersion().isEmpty());
        RadioNicknameTestAccess::protocol(f.radio);
        QVERIFY(f.radio.softwareVersion().isEmpty());
        QCOMPARE(f.backend->capabilities().nrsLevelControl->levelMaximum, 100);
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.2.20.41343"));
        QCOMPARE(f.radio.softwareVersion(), QStringLiteral("4.2.20.41343"));
        QCOMPARE(f.radio.lastRadioInfo().version, lanVersion);
        QCOMPARE(f.backend->capabilities().nrsLevelControl->levelMaximum, 100);
        QVERIFY(f.commands.isEmpty());
        QVERIFY(wan.isSocketIdle());
        RadioConnectionSessionTestAccess::connected(*f.backend->connection(), false);
        WanConnectionTestAccess::connected(wan, true);
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16.42034"));
        QCOMPARE(f.radio.slice(0)->nrsLevelMaximum(), 50);
        QCOMPARE(f.radio.lastRadioInfo().version, lanVersion);
        QVERIFY(f.commands.isEmpty());
        QVERIFY(wan.isSocketIdle());
        WanConnectionTestAccess::connected(wan, false);
    }

    void autoReconnectBeforeFirmwareInfo()
    {
        Fixture f;
        const QString version = QStringLiteral("4.3.16.42034");
        QVERIFY(f.init(version));
        SliceModel* original = f.radio.slice(0);
        RadioConnectionSessionTestAccess::connected(*f.backend->connection(), false);
        QCOMPARE(f.radio.lastRadioInfo().version, version);
        RadioNicknameTestAccess::disconnect(f.radio);
        QVERIFY(f.radio.softwareVersion().isEmpty());
        f.radio.stageSessionModelsForReconnectForTest();
        RadioNicknameTestAccess::retryWithoutTransport(f.radio);
        RadioNicknameTestAccess::protocol(f.radio);
        RadioConnectionSessionTestAccess::connected(*f.backend->connection(), true);
        emit f.radio.connectionStateChanged(true);
        f.commands.clear();
        f.radio.handleSliceStatusForTest(0, sliceStatus(80));
        QCOMPARE(f.radio.slice(0), original);
        QCOMPARE(original->nrsLevelMaximum(), 50);
        QCOMPARE(original->nrsLevel(), 80);
        QVERIFY(f.commands.isEmpty());
        original->setNrsLevel(100);
        QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=50")});
        f.commands.clear();
        RadioNicknameTestAccess::firmware(f.radio, version);
        QCOMPARE(original->nrsLevelMaximum(), 50);
        QVERIFY(f.commands.isEmpty());
    }

    void cappedEndpointAcceptsOperatorIntent()
    {
        Fixture f;
        QVERIFY(f.init(QStringLiteral("4.3.16.42034")));
        SliceModel* slice = f.radio.slice(0);
        VfoWidget vfo;
        vfo.setHasExtendedDsp(true);
        vfo.setRadioModel(&f.radio);
        vfo.setSlice(slice);
        vfo.show();
        QSlider* slider = vfo.findChild<QSlider*>(QStringLiteral("dspLevelSlider"));
        QVERIFY(slider);
        for (QPushButton* button : vfo.findChildren<QPushButton*>()) {
            if (button->accessibleName() == QStringLiteral("DSP settings (DSP active)")) {
                button->click();
                break;
            }
        }
        QCoreApplication::processEvents();
        QVERIFY(slider->isVisible());
        for (int gesture = 0; gesture < 3; ++gesture) {
            f.backend->decodeSliceStatus(0, sliceStatus(80));
            QCOMPARE(slider->value(), 50);
            QCOMPARE(slice->nrsLevel(), 80);
            f.commands.clear();
            if (gesture == 0) {
                QTest::keyClick(slider, Qt::Key_End);
            } else {
                const int handleLength = slider->style()->pixelMetric(
                    QStyle::PM_SliderLength, nullptr, slider);
                const QPoint handle(slider->width() - handleLength / 2 - 1,
                                    slider->height() / 2);
                QSignalSpy released(slider, &QSlider::sliderReleased);
                QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, handle);
                QVERIFY(slider->isSliderDown());
                QCOMPARE(slice->nrsLevel(), 80);
                const QPoint end = gesture == 1 ? handle : handle + QPoint(10, 0);
                if (gesture == 2) {
                    QTest::mouseMove(slider, end);
                }
                QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, end);
                QCOMPARE(released.size(), 1);
            }
            QCOMPARE(slice->nrsLevel(), 50);
            QCOMPARE(f.commands, QStringList{QStringLiteral("slice set 0 speex_nr_level=50")});
        }
    }

    void reconnectReclaimsWithCurrentRange()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* original = f.radio.slice(0);
        QSignalSpy requests(original, &SliceModel::receiveDspRequested);
        QSignalSpy added(&f.radio, &RadioModel::sliceAdded);
        for (const auto& entry : {std::pair{QStringLiteral("4.3.16.42034"), 50},
                                  std::pair{QStringLiteral("4.2.20.41343"), 100}}) {
            f.radio.stageSessionModelsForReconnectForTest();
            QVERIFY(f.radio.slice(0) == nullptr);
            RadioNicknameTestAccess::firmware(f.radio, entry.first);
            f.commands.clear();
            f.radio.handleSliceStatusForTest(0, sliceStatus(80));
            QCOMPARE(f.radio.slice(0), original);
            QCOMPARE(original->nrsLevelMaximum(), entry.second);
            QCOMPARE(original->nrsLevel(), 80);
            QVERIFY(added.isEmpty());
            QVERIFY(requests.isEmpty());
            QVERIFY(f.commands.isEmpty());
        }
        RadioNicknameTestAccess::firmware(f.radio, QStringLiteral("4.3.16.42034"));
        QCOMPARE(original->nrsLevelMaximum(), 50);
        requests.clear();
        f.commands.clear();
        RadioConnectionSessionTestAccess::connected(*f.backend->connection(), false);
        emit f.radio.connectionStateChanged(false);
        QCOMPARE(original->nrsLevelMaximum(), 100);
        QCOMPARE(original->nrsLevel(), 80);
        QVERIFY(requests.isEmpty());
        QVERIFY(f.commands.isEmpty());
    }

    void nonFlexFallback()
    {
        RadioModel radio;
        QVERIFY(radio.rebuildBackendForTest(QStringLiteral("sim")));
        QVERIFY(!radio.backendCapabilities().nrsLevelControl.has_value());
        SliceDelta delta;
        delta.inUse = true;
        delta.mode = QStringLiteral("USB");
        delta.nrsLevel = 80;
        radio.emitBackendSliceChangedForTest(0, delta);
        SliceModel* slice = radio.slice(0);
        QVERIFY(slice);
        QCOMPARE(slice->nrsLevelMaximum(), 100);
        slice->setNrsLevel(100);
        QCOMPARE(slice->nrsLevel(), 100);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("nrs-level-range"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    NrsLevelRangeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "nrs_level_range_test.moc"
