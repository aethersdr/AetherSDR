// Socket-free production Icom RC-28 Button Mapping window: canon chrome, the
// resizer and saved geometry a tool window keeps, an opaque log menu, and the
// mapping fields it reads and writes. No encoder, no hidapi device.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/HidEncoderManager.h"
#include "gui/CanonWindow.h"
#include "gui/FramelessResizer.h"
#include "gui/RC28MappingDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>

#include <cstdio>

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

// A canon dialog (#6744) that still resizes like the PersistentDialog it
// replaced, and restores the rectangle that dialog saved.
void canonToolWindow()
{
    static const QString kKey = QStringLiteral("RC28MappingDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    {
        RC28MappingDialog dialog(nullptr);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "RC-28 Button Mapping is a CanonWindow");
        check(dialog.findChild<FramelessResizer*>() != nullptr, "the window installs FramelessResizer");
    }

    // A rectangle saved before the migration: QWidget::saveGeometry() in base64.
    RC28MappingDialog restored(nullptr);
    const QSize chosen(restored.minimumWidth() + 60, restored.minimumSizeHint().height() + 80);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.size() != chosen, "control: a fresh window is not born at the saved size");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "the window is exposed");
    for (int i = 0; i < 50 && restored.size() != chosen; ++i) {
        QTest::qWait(20);
    }
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");

    const QSize resized = chosen + QSize(40, 20);
    restored.resize(resized);
    QCoreApplication::processEvents();
    restored.hide();
    check(settings.value(kKey).toString().endsWith(
              QStringLiteral(",%1,%2").arg(resized.width()).arg(resized.height())),
          "hiding the window saves its size under the same key");
}

// The log's standard right-click menu is a child of the log, so the canon
// sheet's transparent-everything rule reaches it unless QMenu gets its panel.
void logMenuIsOpaque()
{
    RC28MappingDialog dialog(nullptr);
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    auto* log = dialog.findChild<QPlainTextEdit*>();
    check(log != nullptr, "the window has an activity log");
    if (!log) {
        return;
    }
    QMenu* menu = log->createStandardContextMenu();
    check(menu->parentWidget() == log, "control: the log's menu is parented inside the canon body");
    menu->popup(dialog.mapToGlobal(QPoint(40, 40)));
    QTest::qWait(100);
    const QImage image = menu->grab().toImage();
    check(!image.isNull() && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255,
          "the log's right-click menu has an opaque background");
    menu->close();
    delete menu;
}

// The value beside the Pulses per step slider fits its widest value.
void sensitivityValueFits()
{
    RC28MappingDialog dialog(nullptr);
    dialog.show();
    check(QTest::qWaitForWindowExposed(&dialog), "the window is exposed");
    auto* slider = dialog.findChild<QSlider*>();
    check(slider != nullptr, "the window has the Pulses per step slider");
    if (!slider) {
        return;
    }
    slider->setValue(10);
    QLabel* value = nullptr;
    for (QLabel* l : dialog.findChildren<QLabel*>()) {
        if (l->text() == QStringLiteral("10")) {
            value = l;
        }
    }
    check(value != nullptr, "the slider's value label shows 10");
    if (value) {
        check(value->contentsRect().width() >= value->fontMetrics().horizontalAdvance(value->text()),
              "the value label is wide enough for 10");
    }
}

// The combos read the saved mapping and write a change back, announcing it.
void mappingFieldsRoundTrip()
{
    HidEncoderManager::setRc28MappingField(QStringLiteral("f1Press"), QStringLiteral("BandCycle"));
    RC28MappingDialog dialog(nullptr);
    const QList<QComboBox*> combos = dialog.findChildren<QComboBox*>();
    check(combos.size() == 4, "the window has the four button-assignment combos");
    if (combos.size() != 4) {
        return;
    }
    QComboBox* f1Press = combos.first();
    check(f1Press->currentData().toString() == QStringLiteral("BandCycle"),
          "F1 short press shows its saved action");

    QSignalSpy spy(&dialog, &RC28MappingDialog::mappingFieldChanged);
    f1Press->setCurrentIndex(f1Press->findData(QStringLiteral("VolumeUp")));
    check(HidEncoderManager::rc28MappingField(QStringLiteral("f1Press"), {}) == QStringLiteral("VolumeUp"),
          "choosing an action saves it");
    check(spy.size() == 1 && spy.first().at(0).toString() == QStringLiteral("f1Press")
              && spy.first().at(1).toString() == QStringLiteral("VolumeUp"),
          "choosing an action announces the changed field");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-rc28-mapping-dialog"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    canonToolWindow();
    logMenuIsOpaque();
    sensitivityValueFits();
    mappingFieldsRoundTrip();
    return failures == 0 ? 0 : 1;
}
