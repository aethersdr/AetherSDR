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
//   5. a connected radio with a command plane (the Demo) is unchanged;
//   6. a dimmed row LOOKS dimmed: label, value and Mute of the Headphone and
//      Line Out rows render differently on the HL2 than on the Demo, in the
//      unavailable token's colour, under Default Dark and Default Light;
//   7. an engaged Mute on a dimmed row shows no engaged red: it renders as the
//      same dimmed Mute not engaged, and not as the Demo's engaged Mute.
//
// No hardware and no transport: the no-command-plane radios are an injected
// backend reporting connected with the capabilities the real backend declares;
// the command-plane radio is the Demo, whose connection is never dialled.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "core/ThemeManager.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/MixerControlAvailability.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSlider>
#include <QStringList>
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

// The two sentences every site reads (MixerControlAvailability.h); the title
// bar's use of them is pinned in mixer_control_availability_test.
const QString kLevelReason = QString::fromLatin1(kHeadphoneLevelUnavailableReason);
const QString kMuteReason = QString::fromLatin1(kHeadphoneMuteUnavailableReason);

bool announces(QWidget* w, const QString& reason)
{
    return w->accessibleDescription() == reason && w->toolTip() == reason;
}

// Label, slider and value say how to set the level; the mute says how to mute.
bool rowAnnouncesItsReasons(const OutputRow& r)
{
    return announces(r.label, kLevelReason) && announces(r.slider, kLevelReason)
        && announces(r.value, kLevelReason) && announces(r.mute, kMuteReason);
}

void connectDemo(RadioModel& model)
{
    RadioInfo demo;
    demo.name = QStringLiteral("FLEX-6700");
    demo.model = SimBackend::demoModelName();
    demo.serial = SimBackend::demoSerial();
    demo.family = SimBackend::familyName();
    demo.address = QHostAddress(QHostAddress::LocalHost);  // never dialled
    demo.port = 4992;
    model.connectToRadio(demo);
}

// The widget's own region, as the platform paints it.
QImage rendered(QWidget* w)
{
    return w->grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

// Exact comparison, no tolerance; -1 when the two regions differ in size.
int differingPixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull())
        return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y))
                ++n;
        }
    }
    return n;
}

// Pixels of exactly `colour`; -1 for a null image.
int pixelsOfColour(const QImage& image, const QColor& colour)
{
    if (image.isNull())
        return -1;
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) == colour.rgb())
                ++n;
        }
    }
    return n;
}

// Checks or unchecks a Mute with its toggled signal blocked, so the model's
// setter never runs and no command is built: only the widget's state moves.
void setCheckedSilently(QPushButton* button, bool checked)
{
    const QSignalBlocker blocker(button);
    button->setChecked(checked);
}

// Returns the process to the theme it started on, on every exit from a slot.
class ThemeRestore {
public:
    ThemeRestore() : m_theme(ThemeManager::instance().activeTheme()) {}
    ~ThemeRestore() { ThemeManager::instance().setActiveTheme(m_theme); }
    Q_DISABLE_COPY_MOVE(ThemeRestore)
private:
    const QString m_theme;
};

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
        QVERIFY2(rowAnnouncesItsReasons(hp),
                 "level widgets and mute each carry the title bar's sentence");
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
        QVERIFY2(model.isConnected(), "the injected radio reports connected");
        QVERIFY2(!model.hasCommandPlane(), "ANAN has no command plane");

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Audio"));
        const OutputRow hp = findRow(dialog, QStringLiteral("Headphone:"));
        const OutputRow line = findRow(dialog, QStringLiteral("Line Out:"));
        QVERIFY2(hp.slider && hp.mute, "Headphone row found");
        QVERIFY2(line.label && line.slider && line.value && line.mute, "Line Out row found");

        QVERIFY2(rowDimmed(hp), "ANAN: the Headphone row is dimmed");
        QVERIFY2(rowAnnouncesItsReasons(hp), "ANAN: the same two sentences");
        QVERIFY2(backend->caps.lineoutControl.has_value(), "ANAN declares a line out");
        QVERIFY2(rowLive(line), "ANAN: Line Out stays live");
        QVERIFY2(rowHasNoReason(line), "ANAN: Line Out carries no reason");
    }

    // POSITIVE CONTROL: a connected radio with a command plane is unchanged, so
    // the rows above cannot pass on a row dimmed for every connected radio.
    void connectedCommandPlaneUnchanged()
    {
        RadioModel model;
        connectDemo(model);
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

    // Same caption, same value text, same mute state, same size: the only
    // difference between the two renders of a widget is its enabled state.
    // CONTROLS: a second render of the same widget differs in 0 pixels, and
    // the dimmed row re-enabled by a disconnect renders as the Demo's does.
    void dimmedRowsLookUnavailableInBothThemes()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            const QString unavailable =
                tm.color(QStringLiteral("color.control.unavailable")).name();
            // The engaged Mute's border is drawn in the danger token, solid.
            const QColor engagedRed = tm.color(QStringLiteral("color.accent.danger"));

            RadioModel hl2;
            NoCommandPlaneBackend* backend = installConnected(hl2, QStringLiteral("hl2"));
            QSignalSpy dropped(&hl2, &RadioModel::commandDropped);
            RadioSetupDialog dimmedDialog(&hl2);
            dimmedDialog.show();
            dimmedDialog.selectTab(QStringLiteral("Audio"));

            RadioModel demo;
            connectDemo(demo);
            QTRY_VERIFY_WITH_TIMEOUT(demo.isConnected(), 5000);
            RadioSetupDialog liveDialog(&demo);
            liveDialog.show();
            liveDialog.selectTab(QStringLiteral("Audio"));
            QCoreApplication::processEvents();

            struct Part { const char* name; QWidget* dimmed; QWidget* live; };
            QList<Part> parts;
            QList<QImage> dimmedImages;
            for (const char* caption : {"Headphone:", "Line Out:"}) {
                const OutputRow dim = findRow(dimmedDialog, QLatin1String(caption));
                const OutputRow live = findRow(liveDialog, QLatin1String(caption));
                QVERIFY2(dim.label && dim.value && dim.mute && live.label && live.value
                         && live.mute, caption);
                QVERIFY2(rowDimmed(dim), caption);
                QVERIFY2(rowLive(live), caption);
                QCOMPARE(dim.value->text(), live.value->text());
                QCOMPARE(dim.mute->isChecked(), live.mute->isChecked());
                parts.append({caption, dim.label, live.label});
                parts.append({caption, dim.value, live.value});
                parts.append({caption, dim.mute, live.mute});
            }
            const char* const kinds[] = {"label", "value", "mute"};
            for (int i = 0; i < parts.size(); ++i) {
                const Part& p = parts.at(i);
                const QString where = QStringLiteral("%1 | %2 %3")
                    .arg(QLatin1String(theme), QLatin1String(p.name), QLatin1String(kinds[i % 3]));
                const QImage dimmed = rendered(p.dimmed);
                const QImage live = rendered(p.live);
                dimmedImages.append(dimmed);
                const int total = dimmed.width() * dimmed.height();
                const int repeat = differingPixels(dimmed, rendered(p.dimmed));
                const int diff = differingPixels(dimmed, live);
                qInfo().noquote() << QStringLiteral(
                    "PIXELS %1: dimmed vs live %2 / %3 (same widget twice: %4)")
                    .arg(where).arg(diff).arg(total).arg(repeat);
                if (total <= 0 || repeat != 0)
                    failures << where + QStringLiteral(": render is not repeatable");
                if (diff < 0)
                    failures << where + QStringLiteral(": regions differ in size");
                else if (diff == 0)
                    failures << where + QStringLiteral(": dimmed renders as live");
                // The window's canon sheet paints a disabled label, value and
                // Mute in the unavailable token: its color is in the render.
                if (pixelsOfColour(dimmed, QColor(unavailable)) <= 0)
                    failures << where + QStringLiteral(": not the unavailable token ")
                                    + unavailable;
            }

            // An engaged Mute on a dimmed row. Only the two buttons' checked
            // state moves; the models are not told and nothing is sent.
            const bool hl2HpMute = hl2.headphoneMute();
            const bool hl2LineMute = hl2.lineoutMute();
            const bool demoHpMute = demo.headphoneMute();
            const bool demoLineMute = demo.lineoutMute();
            for (int i = 2; i < parts.size(); i += 3) {
                const Part& p = parts.at(i);
                const QString where = QStringLiteral("%1 | %2 engaged mute")
                    .arg(QLatin1String(theme), QLatin1String(p.name));
                auto* dimMute = qobject_cast<QPushButton*>(p.dimmed);
                auto* liveMute = qobject_cast<QPushButton*>(p.live);
                QVERIFY2(dimMute && liveMute, qPrintable(where));
                setCheckedSilently(dimMute, true);
                setCheckedSilently(liveMute, true);
                QVERIFY2(dimMute->isChecked() && !dimMute->isEnabled(), qPrintable(where));
                QVERIFY2(liveMute->isChecked() && liveMute->isEnabled(), qPrintable(where));
                const QImage dimmedEngaged = rendered(dimMute);
                const QImage liveEngaged = rendered(liveMute);
                const int redDimmed = pixelsOfColour(dimmedEngaged, engagedRed);
                const int redLive = pixelsOfColour(liveEngaged, engagedRed);
                const int vsLive = differingPixels(dimmedEngaged, liveEngaged);
                const int vsNotEngaged = differingPixels(dimmedEngaged, dimmedImages.at(i));
                qInfo().noquote() << QStringLiteral(
                    "PIXELS %1: engaged red, dimmed %2, live %3; dimmed vs live %4; "
                    "dimmed engaged vs dimmed not engaged %5 / %6")
                    .arg(where).arg(redDimmed).arg(redLive).arg(vsLive).arg(vsNotEngaged)
                    .arg(dimmedEngaged.width() * dimmedEngaged.height());
                // CONTROL: the live engaged Mute does show the engaged red.
                if (redLive <= 0)
                    failures << where + QStringLiteral(": the live Mute shows no engaged red");
                if (redDimmed != 0)
                    failures << where + QStringLiteral(": dimmed shows the engaged red in %1 pixels")
                                    .arg(redDimmed);
                if (vsLive < 0)
                    failures << where + QStringLiteral(": regions differ in size");
                else if (vsLive == 0)
                    failures << where + QStringLiteral(": dimmed renders as live");
                if (vsNotEngaged != 0)
                    failures << where + QStringLiteral(": dimmed differs from not engaged by %1")
                                    .arg(vsNotEngaged);
                setCheckedSilently(dimMute, false);
                setCheckedSilently(liveMute, false);
            }
            QCOMPARE(dropped.count(), 0);
            QCOMPARE(hl2.headphoneMute(), hl2HpMute);
            QCOMPARE(hl2.lineoutMute(), hl2LineMute);
            QCOMPARE(demo.headphoneMute(), demoHpMute);
            QCOMPARE(demo.lineoutMute(), demoLineMute);

            // Disconnect: the same widgets, live again, render as the Demo's.
            backend->connected = false;
            emit hl2.connectionStateChanged(false);
            QCoreApplication::processEvents();
            for (int i = 0; i < parts.size(); ++i) {
                const Part& p = parts.at(i);
                const QString where = QStringLiteral("%1 | %2 %3")
                    .arg(QLatin1String(theme), QLatin1String(p.name), QLatin1String(kinds[i % 3]));
                QVERIFY2(p.dimmed->isEnabled(), qPrintable(where));
                const int vsDemo = differingPixels(rendered(p.dimmed), rendered(p.live));
                const int vsDimmed = differingPixels(rendered(p.dimmed), dimmedImages.at(i));
                if (vsDemo != 0)
                    failures << where + QStringLiteral(": re-enabled differs from the Demo by %1")
                                    .arg(vsDemo);
                if (vsDimmed <= 0)
                    failures << where + QStringLiteral(": re-enabled renders as dimmed");
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-headphone-output-gate"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    // Text without antialiasing: every glyph pixel is the label's color, so
    // "the unavailable token is in the render" holds under any fontconfig. With
    // subpixel antialiasing (the Ubuntu CI image) no pixel of "Headphone:" is
    // fully covered, and the token's exact color never appears.
    QFont font = QApplication::font();
    font.setStyleStrategy(QFont::NoAntialias);
    QApplication::setFont(font);
    AppSettings::instance().load();
    RadioSetupHeadphoneOutputGateTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_headphone_output_gate_test.moc"
