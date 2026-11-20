#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AcomConnection.h"
#include "core/PeripheralSettings.h"
#include "core/SpeConnection.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "gui/ClientEqOutputFader.h"
#include "gui/ShackSwitchApplet.h"
#include "models/AntennaGeniusModel.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QtTest>
#include <memory>

namespace AetherSDR {
class RadioSetupDialogTestAccess {
public:
    static QStringList pageNames(RadioSetupDialog& dialog) { return dialog.m_pageIndexes.keys(); }
    // Exercise the real builders even when this observation-only backend lacks
    // a page's radio capability. selectTab() deliberately skips those pages.
    static void buildPage(RadioSetupDialog& dialog, const QString& name)
    {
        dialog.buildDeferredTab(dialog.m_pageIndexes.value(name, -1));
    }
    static bool allPagesBuilt(RadioSetupDialog& dialog) { return dialog.m_deferredBuilders.isEmpty(); }
};

struct AntennaGeniusModelTestAccess {
    static void supplyAntennas(AntennaGeniusModel& model, int radioPorts)
    {
        model.m_device.radioPorts = radioPorts;
        model.m_antennas = {{1, QStringLiteral("Dipole")},
                            {2, QStringLiteral("Vertical")},
                            {3, QStringLiteral("Dummy load")}};
        emit model.antennasChanged();
    }
};
}
using namespace AetherSDR;

// A visible widget with no parent is a top-level window. On macOS the transient
// NSWindow takes key status and the setup dialog drops behind MainWindow.
class TransientWindowRecorder final : public QObject {
public:
    TransientWindowRecorder() { qApp->installEventFilter(this); }
    ~TransientWindowRecorder() override { qApp->removeEventFilter(this); }
    QWidget* allowed{nullptr};
    QStringList seen;
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::Show) {
            return false;
        }
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || !w->isWindow() || w == allowed) {
            return false;
        }
        const Qt::WindowType type = w->windowType();
        if (type == Qt::ToolTip || type == Qt::Popup) {
            return false;
        }
        seen << QStringLiteral("%1 '%2'").arg(w->metaObject()->className(), w->objectName());
        return false;
    }
};

// Only supplies observations to the real model/dialog. No transport or firmware peer.
class CapabilityBackend final : public IRadioBackend {
public:
    bool connected{false};
    RadioCapabilities caps;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAudioGain(int, int) override {}
    void setSliceAudioMute(int, bool) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setPanBandwidth(const QString&, double) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

class RadioSetupNoTransientWindowTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        AppSettings::instance().remove(QStringLiteral("SerialPortName"));
        for (const QString& device : {QStringLiteral("Acom"), QStringLiteral("Spe")}) {
            PeripheralSettings::setDeviceString(device, QStringLiteral("SerialPort"), QString());
        }
    }

    void visitingEveryPageCreatesNoWindow_data()
    {
        QTest::addColumn<bool>("connected");
        QTest::addColumn<QString>("savedPort");
        QTest::newRow("disconnected") << false << QString();
        QTest::newRow("disconnected-custom-port") << false << QStringLiteral("/dev/ttyAether-custom-test");
        QTest::newRow("connected") << true << QString();
    }
    void visitingEveryPageCreatesNoWindow()
    {
        QFETCH(bool, connected);
        QFETCH(QString, savedPort);
        if (!savedPort.isEmpty()) {
            AppSettings::instance().setValue(QStringLiteral("SerialPortName"), savedPort);
            // The Peripherals serial pages show their custom-path edit for a saved port
            // that is not in the enumerated list.
            for (const QString& device : {QStringLiteral("Acom"), QStringLiteral("Spe")}) {
                PeripheralSettings::setDeviceString(device, QStringLiteral("SerialPort"), savedPort);
            }
        }
        RadioModel model;
        auto owned = std::make_unique<CapabilityBackend>();
        owned->connected = connected;
        owned->caps.hasAudioCompression = true;
        model.setBackendForTest(std::move(owned), QStringLiteral("no-transient-window-test"));
        // Inert connection objects (never connected): their presence makes the Peripherals
        // tab build its serial-or-network device pages.
        AcomConnection acom;
        SpeConnection spe;
        TransientWindowRecorder recorder;
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, nullptr, nullptr, &acom, &spe);
        recorder.allowed = &dialog;
        dialog.show();
        QCoreApplication::processEvents();

        const QStringList pages = RadioSetupDialogTestAccess::pageNames(dialog);
        QVERIFY(!pages.isEmpty());
        for (const QString& page : pages) {
            dialog.selectTab(page);
            RadioSetupDialogTestAccess::buildPage(dialog, page);
            QCoreApplication::processEvents();
        }
        QVERIFY(RadioSetupDialogTestAccess::allPagesBuilt(dialog));
        qInfo() << pages.size() << "Radio Setup pages constructed; transient windows:" << recorder.seen;
        QVERIFY2(recorder.seen.isEmpty(), qPrintable(recorder.seen.join(QStringLiteral("; "))));
    }

    void eqReadoutStaysOwned_data()
    {
        QTest::addColumn<bool>("horizontal");
        QTest::newRow("vertical") << false;
        QTest::newRow("horizontal") << true;
    }

    void eqReadoutStaysOwned()
    {
        QFETCH(bool, horizontal);
        QWidget host;
        host.show();
        TransientWindowRecorder recorder;
        auto fader = std::make_unique<ClientEqOutputFader>(&host);
        if (horizontal) {
            fader->setOrientation(Qt::Horizontal);
        }
        fader->show();
        // RX mode shows the readout before rebuilding the cap layout.
        fader->setGainControlEnabled(false);
        QPointer<QLineEdit> editor = fader->findChild<QLineEdit*>();
        const auto labels = fader->findChildren<QLabel*>();
        QVERIFY(editor);
        QCOMPARE(labels.size(), 2);
        QPointer<QLabel> readout = labels[0]->text() == QStringLiteral("OUT")
            ? labels[1] : labels[0];
        for (bool gainControl : {false, true, false}) {
            fader->setGainControlEnabled(gainControl);
            QCoreApplication::processEvents();
            QCOMPARE(editor->isVisible(), gainControl);
            QCOMPARE(readout->isVisible(), !gainControl);
            QVERIFY(!readout->isWindow());
        }
        QVERIFY2(recorder.seen.isEmpty(), qPrintable(recorder.seen.join(QStringLiteral("; "))));
        fader.reset();
        QVERIFY(editor.isNull());
        QVERIFY(readout.isNull());
    }

    void shackSwitchRowsStayOwned_data()
    {
        QTest::addColumn<int>("radioPorts");
        QTest::newRow("single-port") << 1;
        QTest::newRow("dual-port") << 2;
    }

    void shackSwitchRowsStayOwned()
    {
        QFETCH(int, radioPorts);
        AntennaGeniusModel model; // No discovery, connection or firmware peer.
        QWidget host;
        host.show();
        TransientWindowRecorder recorder;
        ShackSwitchApplet applet(&host);
        applet.setModel(&model);
        AntennaGeniusModelTestAccess::supplyAntennas(model, radioPorts);
        applet.show();
        QCoreApplication::processEvents();
        int bButtons = 0;
        for (QPushButton* button : applet.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("B")) {
                ++bButtons;
                QVERIFY(!button->isWindow());
                QCOMPARE(button->isVisible(), radioPorts == 2);
            }
        }
        QCOMPARE(bButtons, 3);
        QVERIFY2(recorder.seen.isEmpty(), qPrintable(recorder.seen.join(QStringLiteral("; "))));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-no-transient-window"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    RadioSetupNoTransientWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_no_transient_window_test.moc"
