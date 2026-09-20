#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QtTest>
#include <memory>

namespace AetherSDR {
class RadioSetupDialogTestAccess {
public:
    static QWidget* infoField(RadioSetupDialog& dialog) { return dialog.m_flexControlInfoField; }
    static QGroupBox* knobGroup(RadioSetupDialog& dialog) { return dialog.m_flexControlGroup; }
};
}
using namespace AetherSDR;

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

class FlexControlVisibilityTest : public QObject {
    Q_OBJECT
private slots:
    void hostSettings_data()
    {
        QTest::addColumn<bool>("radioSupport");
        QTest::addColumn<bool>("initiallyConnected");
        for (bool support : {false, true}) {
            for (bool connected : {false, true}) {
                const QByteArray name = QByteArray(support ? "supported" : "unsupported")
                    + (connected ? "-connected" : "-disconnected");
                QTest::newRow(name.constData()) << support << connected;
            }
        }
    }
    void hostSettings()
    {
        QFETCH(bool, radioSupport);
        QFETCH(bool, initiallyConnected);
        RadioModel model;
        auto owned = std::make_unique<CapabilityBackend>();
        CapabilityBackend* backend = owned.get();
        backend->caps.hasFlexControlIntegration = radioSupport;
        backend->connected = initiallyConnected;
        model.setBackendForTest(std::move(owned), QStringLiteral("visibility-test"));
        RadioSetupDialog dialog(&model);
        dialog.show();
        QWidget* info = RadioSetupDialogTestAccess::infoField(dialog);
        QVERIFY(info);
        QCOMPARE(info->isHidden(), initiallyConnected && !radioSupport);

#ifdef HAVE_SERIALPORT
        dialog.revealFlexControlSettings();
        QCoreApplication::processEvents();
        QGroupBox* knob = RadioSetupDialogTestAccess::knobGroup(dialog);
        QVERIFY(knob);
        QVERIFY(knob->isVisible());
#endif
        // Exercise refresh on the already-constructed production page, then
        // disconnect/reveal again. No connected handler starts a transport.
        for (bool connected : {true, false, true}) {
            backend->connected = connected;
            emit model.connectionStateChanged(connected);
            QCoreApplication::processEvents();
            QCOMPARE(info->isHidden(), connected && !radioSupport);
#ifdef HAVE_SERIALPORT
            QVERIFY(knob->isVisible());
            dialog.revealFlexControlSettings();
            QVERIFY(knob->isVisible());
#endif
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-control-visibility"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    FlexControlVisibilityTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_control_visibility_test.moc"
