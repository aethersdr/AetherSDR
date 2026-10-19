#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AcomConnection.h"
#include "core/PeripheralSettings.h"
#include "core/SpeConnection.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QEvent>
#include <QtTest>
#include <memory>

namespace AetherSDR {
class RadioSetupDialogTestAccess {
public:
    static QStringList pageNames(RadioSetupDialog& dialog) { return dialog.m_pageIndexes.keys(); }
};
}
using namespace AetherSDR;

// A visible widget with no parent is a top-level window. On macOS the transient
// NSWindow takes key status and the setup dialog drops behind MainWindow.
class TransientWindowRecorder final : public QObject {
public:
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
        model.setBackendForTest(std::move(owned), QStringLiteral("no-transient-window-test"));
        // Inert connection objects (never connected): their presence makes the Peripherals
        // tab build its serial-or-network device pages.
        AcomConnection acom;
        SpeConnection spe;
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, nullptr, nullptr, &acom, &spe);
        dialog.show();
        QCoreApplication::processEvents();

        TransientWindowRecorder recorder;
        recorder.allowed = &dialog;
        qApp->installEventFilter(&recorder);
        const QStringList pages = RadioSetupDialogTestAccess::pageNames(dialog);
        QVERIFY(!pages.isEmpty());
        for (const QString& page : pages) {
            dialog.selectTab(page);
            QCoreApplication::processEvents();
        }
        qApp->removeEventFilter(&recorder);
        QVERIFY2(recorder.seen.isEmpty(), qPrintable(recorder.seen.join(QStringLiteral("; "))));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-no-transient-window"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupNoTransientWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_no_transient_window_test.moc"
