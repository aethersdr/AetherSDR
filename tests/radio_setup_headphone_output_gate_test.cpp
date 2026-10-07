// Radio Setup -> Audio -> "Radio Audio Outputs": the Headphone row on a radio
// with no command plane (MixerControlAvailability.h, the title bar's predicate).
//
// Pinned here, each with its control:
//   1. connected HL2 (its real capabilities): the Headphone label, slider,
//      value and mute are dimmed, not hidden, and the slider and mute carry
//      the reason on tooltip AND accessibleDescription;
//   2. dimming moves no level and no mute, and no command is dropped;
//   3. disconnect restores the row with no reason left on it;
//   4. ANAN (its real capabilities): the Headphone row is dimmed while Line
//      Out, which that backend declares (lineoutControl), stays live with no
//      reason: the two rows are gated separately;
//   5. a connected radio with a command plane (the Demo) is unchanged.
//
// No hardware and no transport: the no-command-plane radios are an injected
// backend reporting connected with the capabilities the real backend declares;
// the command-plane radio is the Demo, whose connection is never dialled.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
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

// The capabilities the production backend for `family` declares, read from a
// real one built through the production family switch.
RadioCapabilities realCapabilities(const QString& family)
{
    RadioModel scratch;
    if (!scratch.rebuildBackendForTest(family))
        return {};
    return scratch.backendCapabilities();
}

NoCommandPlaneBackend* installConnected(RadioModel& model, const QString& family)
{
    auto owned = std::make_unique<NoCommandPlaneBackend>();
    NoCommandPlaneBackend* backend = owned.get();
    backend->caps = realCapabilities(family);
    model.setBackendForTest(std::move(owned), family);
    return backend;
}

struct OutputRow {
    QLabel* label{nullptr};
    QSlider* slider{nullptr};
    QLabel* value{nullptr};
    QPushButton* mute{nullptr};
};

// Found from the caption the operator reads, so the lookup works on the
// dialog before this change too and the assertions fail rather than skip.
OutputRow findRow(QWidget& dialog, const QString& caption)
{
    OutputRow out;
    for (QGroupBox* group : dialog.findChildren<QGroupBox*>()) {
        if (group->title() != QStringLiteral("Radio Audio Outputs"))
            continue;
        for (QHBoxLayout* row : group->findChildren<QHBoxLayout*>()) {
            if (row->count() < 4)
                continue;
            auto* label = qobject_cast<QLabel*>(row->itemAt(0)->widget());
            if (!label || label->text() != caption)
                continue;
            out.label = label;
            out.slider = qobject_cast<QSlider*>(row->itemAt(1)->widget());
            out.value = qobject_cast<QLabel*>(row->itemAt(2)->widget());
            out.mute = qobject_cast<QPushButton*>(row->itemAt(3)->widget());
            return out;
        }
    }
    return out;
}

bool rowLive(const OutputRow& r)
{
    return r.label->isEnabled() && r.slider->isEnabled() && r.value->isEnabled()
        && r.mute->isEnabled();
}

bool rowDimmed(const OutputRow& r)
{
    return !r.label->isEnabled() && !r.slider->isEnabled() && !r.value->isEnabled()
        && !r.mute->isEnabled();
}

bool rowHasNoReason(const OutputRow& r)
{
    for (QWidget* w : {static_cast<QWidget*>(r.label), static_cast<QWidget*>(r.slider),
                       static_cast<QWidget*>(r.value), static_cast<QWidget*>(r.mute)}) {
        if (!w->toolTip().isEmpty() || !w->accessibleDescription().isEmpty())
            return false;
    }
    return true;
}

bool announcesReason(QWidget* w)
{
    const QString d = w->accessibleDescription();
    return d.startsWith(QLatin1String("Unavailable:"))
        && d.contains(QLatin1String("no headphone output"))
        && d.contains(QLatin1String("master volume"))
        && w->toolTip() == d;
}

} // namespace

class RadioSetupHeadphoneOutputGateTest : public QObject {
    Q_OBJECT
private slots:

    void hl2DimsTheHeadphoneRowWithItsReason()
    {
        RadioModel model;
        NoCommandPlaneBackend* backend = installConnected(model, QStringLiteral("hl2"));
        QVERIFY2(backend->caps.family == QLatin1String("hl2"), "real HL2 capabilities read");
        QVERIFY2(model.isConnected(), "the injected radio reports connected");
        QVERIFY2(!model.hasCommandPlane(), "HL2 has no command plane");
        const int gain = model.headphoneGain();
        const bool muted = model.headphoneMute();
        QSignalSpy dropped(&model, &RadioModel::commandDropped);

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Audio"));
        const OutputRow hp = findRow(dialog, QStringLiteral("Headphone:"));
        QVERIFY2(hp.label && hp.slider && hp.value && hp.mute, "Headphone row found");
        QVERIFY2(hp.slider->isVisible() && hp.mute->isVisible(), "dimmed, never hidden");

        QVERIFY2(rowDimmed(hp), "HL2: the whole Headphone row is dimmed");
        QVERIFY2(announcesReason(hp.slider), "slider announces the reason");
        QVERIFY2(announcesReason(hp.mute), "mute announces the reason");
        QCOMPARE(hp.slider->value(), gain);
        QCOMPARE(hp.mute->isChecked(), muted);
        QCOMPARE(model.headphoneGain(), gain);
        QCOMPARE(dropped.count(), 0);

        backend->connected = false;
        emit model.connectionStateChanged(false);
        QVERIFY2(rowLive(hp), "disconnected: the row is live again");
        QVERIFY2(rowHasNoReason(hp), "disconnected: no reason left behind");

        backend->connected = true;
        emit model.connectionStateChanged(true);
        QVERIFY2(rowDimmed(hp), "reconnected: dimmed again");
    }

    void ananDimsHeadphoneButKeepsLineOut()
    {
        RadioModel model;
        NoCommandPlaneBackend* backend = installConnected(model, QStringLiteral("anan"));
        QVERIFY2(backend->caps.family == QLatin1String("anan"), "real ANAN capabilities read");
        QVERIFY2(!model.hasCommandPlane(), "ANAN has no command plane");

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Audio"));
        const OutputRow hp = findRow(dialog, QStringLiteral("Headphone:"));
        const OutputRow line = findRow(dialog, QStringLiteral("Line Out:"));
        QVERIFY2(hp.slider && hp.mute, "Headphone row found");
        QVERIFY2(line.label && line.slider && line.value && line.mute, "Line Out row found");

        QVERIFY2(rowDimmed(hp), "ANAN: the Headphone row is dimmed");
        QVERIFY2(backend->caps.lineoutControl.has_value(), "ANAN declares a line out");
        QVERIFY2(rowLive(line), "ANAN: Line Out stays live");
        QVERIFY2(rowHasNoReason(line), "ANAN: Line Out carries no reason");
    }

    // POSITIVE CONTROL: a connected radio with a command plane is unchanged, so
    // the rows above cannot pass on a row dimmed for every connected radio.
    void connectedCommandPlaneUnchanged()
    {
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
        const OutputRow hp = findRow(dialog, QStringLiteral("Headphone:"));
        const OutputRow line = findRow(dialog, QStringLiteral("Line Out:"));
        QVERIFY2(hp.slider && hp.mute && line.slider, "rows found");
        QVERIFY2(rowLive(hp) && rowLive(line), "command plane: both rows live");
        QVERIFY2(rowHasNoReason(hp) && rowHasNoReason(line), "command plane: no reason");
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-headphone-output-gate"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    RadioSetupHeadphoneOutputGateTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_headphone_output_gate_test.moc"
