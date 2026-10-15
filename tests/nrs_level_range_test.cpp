// Firmware identity, operator requests and passive NRS readback through the
// production model/backend/widget bindings. No peer, socket dial or radio.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QMap>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QStringList>
#include <QTest>
#include <QWidget>

#include <array>
#include <utility>

namespace AetherSDR {
class RadioNicknameTestAccess {
public:
    static void firmware(RadioModel& radio, const QString& version)
    {
        radio.m_version = version;
        radio.m_model = QStringLiteral("FLEX-8600");
        emit radio.infoChanged();
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
} // namespace

class NrsLevelRangeTest : public QObject {
    Q_OBJECT

private slots:
    void firmwareAndEncoding()
    {
        Fixture f;
        QVERIFY(f.init());
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
        RadioNicknameTestAccess::firmware(f.radio, QString());
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
