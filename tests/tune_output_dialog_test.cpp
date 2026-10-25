// Socket-free production TX Band Settings controls with injected command replies.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "gui/CanonWindow.h"
#include "gui/TxBandDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QLineEdit>
#include <QImage>
#include <QTest>

#include <cstdio>
#include <functional>
#include <memory>

namespace AetherSDR {
class TuneOutputDialogTestAccess {
public:
    static void identity(RadioModel& radio, FlexBackend* backend, const QString& version)
    {
        radio.m_version = version;
        radio.m_lastInfo.family = QStringLiteral("flex");
        radio.m_lastInfo.serial = QStringLiteral("tune-output-dialog-test");
        radio.m_flexBackend = backend;
    }
    static void clearReadback(RadioModel& radio) { radio.resetTuneOutputControl(); }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed) {
        ++failures;
    }
}

class ConnectedFlex final : public FlexBackend {
public:
    bool connected = false;
    bool isConnected() const override { return connected; }
};

struct Fixture {
    QString version;
    RadioModel radio;
    ConnectedFlex* backend = nullptr;
    QStringList commands;
    QList<FlexBackend::CommandReply> replies;

    explicit Fixture(const QString& firmware) : version(firmware)
    {
        auto owned = std::make_unique<ConnectedFlex>();
        backend = owned.get();
        backend->setFirmwareVersionProvider([this] { return version; });
        backend->setCommandSink([this](const QString& command, FlexBackend::CommandReply reply) {
            commands.append(command);
            if (reply) { replies.append(std::move(reply)); }
            return true;
        });
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        TuneOutputDialogTestAccess::identity(radio, backend, version);
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "fixture creates an isolated slice");
        SliceDelta delta;
        delta.frequency = 14.225;
        delta.txSlice = true;
        radio.slice(0)->applyChanges(delta);
        backend->connected = true;
        current(5, false);
        current(9, true);
        commands.clear();
    }

    void current(int bandId, bool suppressed)
    {
        radio.handleStatusForTest(QStringLiteral("interlock band %1").arg(bandId),
            {{QStringLiteral("band_name"), bandId == 5 ? QStringLiteral("40") : QStringLiteral("20")},
             {QStringLiteral("tune_tx_suppress"), suppressed ? QStringLiteral("1") : QStringLiteral("0")}});
    }
};

QCheckBox* bandBox(TxBandDialog& dialog, int id)
{
    return dialog.findChild<QCheckBox*>(QStringLiteral("tuneOutputSuppressionBand%1").arg(id));
}

void supportedDialog()
{
    Fixture f(QStringLiteral("4.3.16.42034"));
    TxBandDialog dialog(&f.radio);
    // The smallest the window can be: a restored rectangle below the minimum
    // is clamped up to it, so this is the worst case for clipping.
    dialog.resize(dialog.minimumSize());
    dialog.show();
    QApplication::processEvents();
    QCheckBox* forty = bandBox(dialog, 5);
    QCheckBox* twenty = bandBox(dialog, 9);
    check(forty && twenty && forty->isEnabled() && twenty->isEnabled()
              && !forty->isChecked() && twenty->isChecked(),
          "explicit band rows start with different confirmed radio values");
    check(f.commands.isEmpty(), "constructing the table sends no commands");
    int newHeaders = 0;
    for (QLabel* label : dialog.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("RCA off/TUNE")) {
            ++newHeaders;
            const QRect headerBounds(label->mapTo(&dialog, QPoint()), label->size());
            check(dialog.rect().contains(headerBounds)
                      && label->width() >= label->sizeHint().width()
                      && label->height() >= label->sizeHint().height(),
                  "the window at its minimum size cannot clip the header text");
            // Measured in the font the sheet gives it, not the default font: a
            // label measured before polish is sized for different text.
            const QMargins margins = label->contentsMargins();
            const int textWidth = label->fontMetrics().horizontalAdvance(label->text())
                + margins.left() + margins.right();
            check(qAbs(label->minimumWidth() - textWidth) <= 2,
                  "a header's minimum width is its text in the canon sheet's font");
        }
    }
    check(newHeaders == 1, "one added column has the approved concise label");
    if (!forty || !twenty) {
        return;
    }
    check(forty->accessibleName().contains(QStringLiteral("40"))
              && twenty->accessibleName().contains(QStringLiteral("20")),
          "checkbox accessible names identify their own band");
    forty->click();
    check(f.commands == QStringList{QStringLiteral("interlock bandset 5 tune_tx_suppress=1")}
              && !forty->isChecked() && !forty->isEnabled()
              && twenty->isEnabled() && twenty->isChecked(),
          "40m click targets only band5 and leaves20m editable and unchanged");
    check(!f.replies.isEmpty(), "injected reply captures the production request");
    if (f.replies.isEmpty()) {
        return;
    }
    f.replies.takeFirst()(0, QString());
    check(!forty->isChecked() && !forty->isEnabled(),
          "ACK alone cannot confirm the displayed row value");
    f.current(5, true);
    check(forty->isChecked() && forty->isEnabled(), "matching readback confirms the row");
    twenty->click();
    f.replies.takeFirst()(0x5000002d, QStringLiteral("unsupported field"));
    check(twenty->isChecked() && twenty->isEnabled()
              && twenty->accessibleDescription().contains(QStringLiteral("rejected"), Qt::CaseInsensitive),
          "rejection preserves confirmed value and exposes reason on that row");
    f.commands.clear();
    SliceDelta moved;
    moved.frequency = 7.135;
    f.radio.slice(0)->applyChanges(moved);
    check(forty->isChecked() && twenty->isChecked() && f.commands.isEmpty(),
          "TX slice frequency changes never retarget or change either row");
    f.current(9, false);
    check(!twenty->isChecked() && forty->isChecked() && f.commands.isEmpty(),
          "external band status reconciles only its checkbox without feedback");
    TxBandDialog reopened(&f.radio);
    check(bandBox(reopened, 5)->isChecked() && !bandBox(reopened, 9)->isChecked()
              && f.commands.isEmpty(), "reopening reads confirmed per-band values without writes");
    check(!f.radio.transmitModel().isTuning() && !f.radio.transmitModel().isTransmitting(),
          "table edits never start TUNE or RF");
    TuneOutputDialogTestAccess::clearReadback(f.radio);
    check(!forty->isEnabled() && !forty->isChecked()
              && !twenty->isEnabled() && !twenty->isChecked(),
          "session reset clears old values and availability in the open table");
}

void unavailableDialog()
{
    for (const QString& version : {QStringLiteral("4.2.20.41343"), QStringLiteral("4.1.5.39794"),
                                  QStringLiteral("unknown"), QStringLiteral("4.3.16.42034junk")}) {
        Fixture f(version);
        TxBandDialog dialog(&f.radio);
        QCheckBox* checkbox = bandBox(dialog, 5);
        check(checkbox && !checkbox->isEnabled() && !checkbox->accessibleDescription().isEmpty(),
              "older or unknown firmware leaves a dimmed checkbox with an accessible reason");
        check(f.commands.isEmpty(), "unsupported table construction sends nothing");
    }
}

void firmwareIdentityAfterReadback()
{
    Fixture f(QStringLiteral("1.4.0.0"));
    TxBandDialog dialog(&f.radio);
    QCheckBox* forty = bandBox(dialog, 5);
    QCheckBox* twenty = bandBox(dialog, 9);
    check(forty && twenty && !forty->isEnabled() && !twenty->isEnabled(),
          "status-first session leaves the open table dimmed until firmware is known");
    if (!forty || !twenty) {
        return;
    }
    f.version = QStringLiteral("4.3.16.42034");
    TuneOutputDialogTestAccess::identity(f.radio, f.backend, f.version);
    emit f.radio.infoChanged();
    check(forty->isEnabled() && !forty->isChecked() && twenty->isEnabled() && twenty->isChecked()
              && f.commands.isEmpty(),
          "late firmware identity enables the existing table with each earlier confirmed value");
}
// A field's standard right-click menu is a child of the field, so the canon
// body's transparent-everything rule reaches it; the sheet gives popups their
// panel back. Without that the menu draws no background at all.
void rightClickMenuIsOpaque()
{
    Fixture f(QStringLiteral("4.3.16.42034"));
    TxBandDialog dialog(&f.radio);
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);
    const QList<QLineEdit*> edits = dialog.findChildren<QLineEdit*>();
    check(!edits.isEmpty(), "the table exposes a power field");
    if (edits.isEmpty()) {
        return;
    }
    QMenu* menu = edits.first()->createStandardContextMenu();
    check(menu->parentWidget() == edits.first(),
          "control: the field's menu is parented inside the canon body");
    menu->popup(dialog.mapToGlobal(QPoint(40, 40)));
    QTest::qWait(100);
    const QImage image = menu->grab().toImage();
    check(!image.isNull()
              && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255,
          "a field's right-click menu has an opaque background");
    menu->close();
    delete menu;
    check(f.commands.isEmpty(), "opening the menu sends no commands");
}

// TX Band Settings is a canon workspace (#6239) that restores geometry saved
// under TxBandDialogGeometry, including the base64 rectangles earlier
// releases wrote.
void canonWorkspace()
{
    static const QString kKey = QStringLiteral("TxBandDialogGeometry");
    Fixture f(QStringLiteral("4.3.16.42034"));
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    {
        TxBandDialog dialog(&f.radio);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "TX Band Settings is a CanonWindow");
        check(dialog.windowType() == Qt::Window
                  && dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
              "it is a frameless workspace window, not a dialog on its parent");
    }

    // A rectangle saved before the migration: QWidget::saveGeometry() in base64.
    // Compared by height: the offscreen screen is 800 px wide, narrower than
    // the table's minimum, so a restored width is clamped to the minimum.
    TxBandDialog restored(&f.radio);
    const QSize chosen(restored.minimumWidth(), restored.minimumHeight() + 110);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.height() != chosen.height(), "control: a fresh window is not born at the saved height");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "the window is exposed");
    for (int i = 0; i < 50 && restored.height() != chosen.height(); ++i) {
        QTest::qWait(20);
    }
    check(restored.height() == chosen.height(), "geometry saved by the old dialog restores under its key");

    const QSize resized = chosen + QSize(40, 20);
    restored.resize(resized);
    QCoreApplication::processEvents();
    restored.hide();
    const QString saved = settings.value(kKey).toString();
    check(saved.endsWith(QStringLiteral(",%1,%2").arg(resized.width()).arg(resized.height())),
          "hiding the window saves its size under the same key");
    check(f.commands.isEmpty(), "opening, resizing and hiding the window sends no commands");
}

QLineEdit* fieldNamed(TxBandDialog& dialog, const QString& name)
{
    for (QLineEdit* edit : dialog.findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == name) {
            return edit;
        }
    }
    return nullptr;
}

QLabel* headerTitle(TxBandDialog& dialog)
{
    return dialog.findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
}

// The table belongs to one TX profile: a new profile rebuilds it under a new
// heading, and band values the radio reports later reach the open table.
void followsTheRadio()
{
    Fixture f(QStringLiteral("4.3.16.42034"));
    f.radio.transmitModel().setActiveProfile(QStringLiteral("Default"));
    f.radio.handleStatusForTest(QStringLiteral("transmit band 5"),
        {{QStringLiteral("band_name"), QStringLiteral("40")}, {QStringLiteral("rfpower"), QStringLiteral("30")}});
    f.commands.clear();
    TxBandDialog dialog(&f.radio);
    dialog.show();
    QApplication::processEvents();
    QLabel* title = headerTitle(dialog);
    check(title && title->text().endsWith(QStringLiteral("Profile: Default)")),
          "the heading names the profile the table was built from");
    QPointer<QLineEdit> rf = fieldNamed(dialog, QStringLiteral("RF power percent, band 40"));
    check(rf && rf->text() == QStringLiteral("30"), "the RF power field shows the band's value");

    f.radio.handleStatusForTest(QStringLiteral("transmit band 5"), {{QStringLiteral("rfpower"), QStringLiteral("55")}});
    QApplication::processEvents();
    check(rf && rf->text() == QStringLiteral("55"), "a band value the radio reports reaches the open table");

    f.radio.handleStatusForTest(QStringLiteral("interlock band 5"), {{QStringLiteral("tx1_enabled"), QStringLiteral("1")}});
    QApplication::processEvents();
    QCheckBox* tx1 = nullptr;
    for (QCheckBox* box : dialog.findChildren<QCheckBox*>()) {
        if (box->accessibleName() == QStringLiteral("RCA TX1, band 40")) {
            tx1 = box;
        }
    }
    check(tx1 && tx1->isChecked(), "a keying output the radio reports reaches its check box");

    rf->setFocus();
    rf->selectAll();
    QTest::keyClicks(rf, QStringLiteral("4"));
    f.radio.handleStatusForTest(QStringLiteral("transmit band 5"), {{QStringLiteral("rfpower"), QStringLiteral("60")}});
    QApplication::processEvents();
    check(rf->text() == QStringLiteral("4"), "a field being typed in keeps the operator's text");
    rf->undo();
    rf->setModified(false);
    rf->clearFocus();

    f.radio.transmitModel().setActiveProfile(QStringLiteral("Contest"));
    QApplication::processEvents();
    check(title && title->text().endsWith(QStringLiteral("Profile: Contest)"))
              && dialog.windowTitle().endsWith(QStringLiteral("Profile: Contest)")),
          "a new TX profile renames the heading and the window");
    QLineEdit* rebuilt = fieldNamed(dialog, QStringLiteral("RF power percent, band 40"));
    check(!rf && rebuilt && rebuilt->text() == QStringLiteral("60"),
          "a new TX profile rebuilds the table from the radio's values");
    check(dialog.findChildren<QLineEdit*>().size() == 2 * f.radio.txBandSettings().size(),
          "the rebuilt table has one row per band");
    f.radio.handleStatusForTest(QStringLiteral("transmit band 7"),
        {{QStringLiteral("band_name"), QStringLiteral("30")}, {QStringLiteral("rfpower"), QStringLiteral("25")}});
    QApplication::processEvents();
    QLineEdit* added = fieldNamed(dialog, QStringLiteral("RF power percent, band 30"));
    check(added && added->text() == QStringLiteral("25"), "a band the radio adds gets its own row");
    f.radio.handleStatusForTest(QStringLiteral("transmit band 8"), {{QStringLiteral("rfpower"), QStringLiteral("20")}});
    QApplication::processEvents();
    f.radio.handleStatusForTest(QStringLiteral("interlock band 8"), {{QStringLiteral("band_name"), QStringLiteral("17")}});
    QApplication::processEvents();
    check(fieldNamed(dialog, QStringLiteral("RF power percent, band 17")) != nullptr,
          "a band whose name arrives later is rebuilt under its name");

    // Esc hides the window; showing it again builds a fresh table.
    QPointer<QLineEdit> beforeHide = fieldNamed(dialog, QStringLiteral("RF power percent, band 40"));
    dialog.hide();
    dialog.show();
    QApplication::processEvents();
    check(!beforeHide && fieldNamed(dialog, QStringLiteral("RF power percent, band 40")),
          "reopening a hidden window builds a fresh table");

    // This fixture's backend takes no bandset writes, so every click is refused.
    QCheckBox* inhibit = nullptr;
    for (QCheckBox* box : dialog.findChildren<QCheckBox*>()) {
        if (box->accessibleName() == QStringLiteral("PTT inhibit, band 40")) {
            inhibit = box;
        }
    }
    check(inhibit && !inhibit->isChecked(), "control: band 40 starts without PTT inhibit");
    if (inhibit) {
        inhibit->click();
    }
    check(inhibit && !inhibit->isChecked(), "a click the radio cannot take shows the radio's value again");
    check(f.commands.isEmpty(), "following the radio sends no commands");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tune-output-dialog"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    supportedDialog();
    unavailableDialog();
    firmwareIdentityAfterReadback();
    canonWorkspace();
    rightClickMenuIsOpaque();
    followsTheRadio();
    return failures == 0 ? 0 : 1;
}
