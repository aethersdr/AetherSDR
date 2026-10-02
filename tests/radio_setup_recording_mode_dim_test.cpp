// Radio Setup → Audio → Recording: the Record Mode pair on a radio with no
// command plane (HL2, ANAN, Icom, RTL), where there is no radio-side recorder.
//
// Pinned here, each with its control:
//   1. connected with no command plane, Radio Side is dimmed (disabled, not
//      hidden) and says why on the tooltip AND accessibleDescription;
//   2. there Client Side shows as the mode in effect, even with Radio Side
//      saved;
//   3. the saved RecordingMode is never written, not even by a click on the
//      Client Side already in effect: still "Radio" after the dialog, after a
//      commit, and after re-reading the store from disk;
//   4. on disconnect the pair lets go and shows the saved choice again;
//   5. a CONNECTED radio with a command plane is unchanged: Radio Side
//      enabled, no reason, the saved choice shown, and a click still saves it.
//
// No hardware and no transport: the no-command-plane radio is an injected
// backend that reports itself connected, and the command-plane radio is the
// Demo, whose connection is synthetic and never dialled.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QHostAddress>
#include <QPushButton>
#include <QtTest>

#include <memory>

using namespace AetherSDR;

namespace {

class NoCommandPlaneBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct ModeButtons {
    QPushButton* radioSide{nullptr};
    QPushButton* clientSide{nullptr};
};

ModeButtons findModeButtons(QWidget& dialog)
{
    ModeButtons out;
    for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("Radio Side")) {
            out.radioSide = b;
        } else if (b->text() == QStringLiteral("Client Side")) {
            out.clientSide = b;
        }
    }
    return out;
}

void saveRecordingMode(const QString& mode)
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RecordingMode"), mode);
    s.save();
    s.load();
}

QString savedRecordingMode()
{
    return AppSettings::instance().value(QStringLiteral("RecordingMode")).toString();
}

NoCommandPlaneBackend* installNoCommandPlaneRadio(RadioModel& model)
{
    auto owned = std::make_unique<NoCommandPlaneBackend>();
    NoCommandPlaneBackend* backend = owned.get();
    backend->caps.family = QStringLiteral("hl2");
    model.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    return backend;
}

} // namespace

class RadioSetupRecordingModeDimTest : public QObject {
    Q_OBJECT
private slots:

    void noCommandPlaneDimsRadioSideAndKeepsTheSavedChoice()
    {
        saveRecordingMode(QStringLiteral("Radio"));

        RadioModel model;
        NoCommandPlaneBackend* backend = installNoCommandPlaneRadio(model);
        // The premises, asserted rather than assumed.
        QVERIFY2(model.isConnected(), "the injected radio reports connected");
        QVERIFY2(!model.hasCommandPlane(), "no command plane");
        QVERIFY2(!model.radioSideRecordingReachable(), "no radio-side recorder");

        {
            RadioSetupDialog dialog(&model);
            dialog.show();
            dialog.selectTab(QStringLiteral("Audio"));
            const ModeButtons mode = findModeButtons(dialog);
            QVERIFY2(mode.radioSide && mode.clientSide, "Record Mode pair found");

            QVERIFY2(mode.radioSide->isVisible(), "dimmed, never hidden");
            QVERIFY2(!mode.radioSide->isEnabled(), "Radio Side is dimmed");
            QVERIFY2(mode.radioSide->accessibleDescription().startsWith(
                         QStringLiteral("Unavailable:")),
                     qPrintable(mode.radioSide->accessibleDescription()));
            QCOMPARE(mode.radioSide->toolTip(), mode.radioSide->accessibleDescription());
            QVERIFY2(mode.clientSide->isEnabled(), "Client Side stays selectable");
            QVERIFY2(mode.clientSide->isChecked(), "Client Side is in effect");
            QVERIFY2(!mode.radioSide->isChecked(), "Radio Side is not shown in effect");
            QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));

            // Clicking the Client Side already in effect changes nothing saved.
            mode.clientSide->click();
            QVERIFY(mode.clientSide->isChecked());
            QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));

            // The radio goes away: the gate lets go and the saved choice shows.
            backend->connected = false;
            emit model.connectionStateChanged(false);
            QVERIFY2(mode.radioSide->isEnabled(), "disconnected: Radio Side live again");
            QVERIFY2(mode.radioSide->isChecked(), "the saved Radio Side is shown");
            QVERIFY(!mode.clientSide->isChecked());
            QVERIFY(mode.radioSide->accessibleDescription().isEmpty());
            QVERIFY(mode.radioSide->toolTip().isEmpty());

            // And back: dimmed again, still without writing.
            backend->connected = true;
            emit model.connectionStateChanged(true);
            QVERIFY(!mode.radioSide->isEnabled());
            QVERIFY(mode.clientSide->isChecked());
        }

        // The saved choice survives a commit and a re-read from disk.
        QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));
        AppSettings::instance().save();
        AppSettings::instance().load();
        QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));
    }

    // POSITIVE CONTROL: a connected radio with a command plane is unchanged
    // from main, so the assertions above cannot be passed by a pair dimmed on
    // every radio, or on every connected one.
    void connectedCommandPlaneUnchangedFromMain()
    {
        saveRecordingMode(QStringLiteral("Radio"));

        RadioModel model;
        RadioInfo demo;
        demo.name = QStringLiteral("FLEX-6700");
        demo.model = SimBackend::demoModelName();
        demo.serial = SimBackend::demoSerial();
        demo.family = SimBackend::familyName();
        demo.address = QHostAddress(QHostAddress::LocalHost);  // never dialled
        demo.port = 4992;
        model.connectToRadio(demo);
        QTRY_VERIFY_WITH_TIMEOUT(model.isConnected(), 5000);
        QVERIFY2(model.hasCommandPlane(), "the Demo has a command plane");

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Audio"));
        const ModeButtons mode = findModeButtons(dialog);
        QVERIFY2(mode.radioSide && mode.clientSide, "Record Mode pair found");

        QVERIFY(mode.radioSide->isEnabled());
        QVERIFY(mode.radioSide->isChecked());
        QVERIFY(!mode.clientSide->isChecked());
        QVERIFY(mode.radioSide->accessibleDescription().isEmpty());
        QVERIFY(mode.radioSide->toolTip().isEmpty());

        mode.clientSide->click();
        QCOMPARE(savedRecordingMode(), QStringLiteral("Client"));
        mode.radioSide->click();
        QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));
        AppSettings::instance().load();
        QCOMPARE(savedRecordingMode(), QStringLiteral("Radio"));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-recording-mode-dim"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    RadioSetupRecordingModeDimTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_recording_mode_dim_test.moc"
