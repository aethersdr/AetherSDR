// FreeDV Reporter status-message row (#4231), and the canon window it sits
// in: the resizer and saved geometry of a tool window, an opaque menu in the
// message field, and band buttons that take focus from Tab only.
//
// Covers the dialog-side logic only — no network. The send path stops at
// the messageChanged() signal; MainWindow is what forwards it to
// FreeDvClient::updateMessage() on the client thread, so nothing here can
// reach qso.freedv.org.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/CanonWindow.h"
#include "gui/FramelessResizer.h"
#include "gui/FreeDvReporterDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) ++g_failed;
}

QLineEdit* msgEdit(FreeDvReporterDialog& dlg)
{
    return dlg.findChild<QLineEdit*>(QStringLiteral("fdvReporterMessageEdit"));
}

QPushButton* msgSendBtn(FreeDvReporterDialog& dlg)
{
    return dlg.findChild<QPushButton*>(
        QStringLiteral("fdvReporterMessageSendButton"));
}

void resetSettings()
{
    auto& settings = AppSettings::instance();
    const QString path = settings.filePath();
    settings.reset();
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".bak"));
    QFile::remove(path + QStringLiteral(".tmp"));
    settings.load();
}

// The row is gated on full-participant reporting, because
// FreeDvClient::updateMessage() silently no-ops when reporting is off.
// Rather than leave a dead field, the row disables itself and says why.
void testReportingGate()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    auto* edit = msgEdit(dlg);
    auto* send = msgSendBtn(dlg);
    report("message edit exists", edit != nullptr);
    report("send button exists", send != nullptr);
    if (!edit || !send) return;

    report("row starts disabled while reporting is off", !edit->isEnabled());
    report("send starts disabled while reporting is off", !send->isEnabled());
    const QString offPlaceholder = edit->placeholderText();
    report("disabled placeholder explains why",
           offPlaceholder.contains("Enable FreeDV reporting"), offPlaceholder);

    dlg.setReportingActive(true);
    report("edit enables when reporting starts", edit->isEnabled());
    report("send enables when reporting starts", send->isEnabled());
    report("enabled placeholder changes",
           edit->placeholderText() != offPlaceholder, edit->placeholderText());

    dlg.setReportingActive(false);
    report("edit disables again when reporting stops", !edit->isEnabled());
    report("placeholder explains again",
           edit->placeholderText() == offPlaceholder, edit->placeholderText());
}

// Qt does not deliver tooltips to disabled widgets, so the "why is this
// off?" text has to live on an enabled ancestor or it is unreachable in
// exactly the state that needs explaining.
void testDisabledStateStillExplains()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    auto* edit = msgEdit(dlg);
    if (!edit) { report("message edit exists", false); return; }

    QWidget* row = edit->parentWidget();
    report("message row container exists", row != nullptr);
    if (!row) return;

    report("tooltip lives on an enabled ancestor, not the disabled field",
           edit->toolTip().isEmpty() && !row->toolTip().isEmpty(),
           QStringLiteral("edit='%1' row='%2'")
               .arg(edit->toolTip(), row->toolTip()));
    report("container stays enabled so the tooltip is reachable",
           row->isEnabled());
    report("disabled-state tooltip names the remedy",
           row->toolTip().contains("SpotHub") || row->toolTip().contains("RADE"),
           row->toolTip());
}

// Send trims, persists to the shared FreeDvMyMessage setting, and emits.
void testSendCommitsMessage()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    auto* edit = msgEdit(dlg);
    auto* send = msgSendBtn(dlg);
    if (!edit || !send) { report("message widgets exist", false); return; }

    dlg.setReportingActive(true);
    QSignalSpy spy(&dlg, &FreeDvReporterDialog::messageChanged);

    edit->setText("  QRT in 10 mins  ");
    send->click();

    report("send emits messageChanged once", spy.count() == 1,
           QString::number(spy.count()));
    if (!spy.isEmpty()) {
        report("emitted message is trimmed",
               spy.takeFirst().at(0).toString() == QStringLiteral("QRT in 10 mins"));
    }
    report("field shows the trimmed text",
           edit->text() == QStringLiteral("QRT in 10 mins"), edit->text());
    report("message persisted to FreeDvMyMessage",
           AppSettings::instance().value("FreeDvMyMessage", "").toString()
               == QStringLiteral("QRT in 10 mins"));
}

// Bounds checking per the issue's acceptance criteria — the field refuses
// overlong input rather than letting it reach the network layer.
void testLengthCap()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    auto* edit = msgEdit(dlg);
    if (!edit) { report("message edit exists", false); return; }

    report("max length is capped", edit->maxLength() == FreeDvReporterDialog::MessageMaxLength,
           QString::number(edit->maxLength()));

    dlg.setReportingActive(true);
    edit->setText(QString(FreeDvReporterDialog::MessageMaxLength + 50, QLatin1Char('x')));
    report("overlong input is truncated to the cap",
           edit->text().length() == FreeDvReporterDialog::MessageMaxLength,
           QString::number(edit->text().length()));
}

// SpotHub's FreeDV tab writes the same setting, so the dialog re-reads it
// on open — but must not clobber an edit the operator has not sent yet.
void testReloadFromSettings()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    auto* edit = msgEdit(dlg);
    if (!edit) { report("message edit exists", false); return; }
    dlg.setReportingActive(true);

    auto& s = AppSettings::instance();
    s.setValue("FreeDvMyMessage", "set from SpotHub");
    s.save();

    dlg.reloadMessage();
    report("reload picks up an external edit",
           edit->text() == QStringLiteral("set from SpotHub"), edit->text());

    // An untouched field tracks the setting; a typed-but-unsent draft is
    // left alone. The guard is the modified flag rather than focus — focus
    // has already moved away by the time the operator re-opens the dialog,
    // so a focus test would never fire in the case it exists to cover (and,
    // being unreachable offscreen, would never be tested either).
    // insert() rather than setText(): setText() clears the modified flag,
    // which is exactly the distinction under test here.
    edit->clear();
    edit->insert(QStringLiteral("half-typed, not sent"));
    report("typing marks the field modified", edit->isModified());

    s.setValue("FreeDvMyMessage", "changed underneath");
    s.save();
    dlg.reloadMessage();
    report("reload does not clobber an unsent draft",
           edit->text() == QStringLiteral("half-typed, not sent"), edit->text());

    // Sending clears the draft state, so the field tracks the setting again.
    msgSendBtn(dlg)->click();
    report("send clears the modified flag", !edit->isModified());
    s.setValue("FreeDvMyMessage", "changed again");
    s.save();
    dlg.reloadMessage();
    report("reload refreshes once the draft has been sent",
           edit->text() == QStringLiteral("changed again"), edit->text());
}

// A canon workspace that still resizes like the PersistentDialog it replaced,
// and restores the rectangle that dialog saved under the same key.
void testCanonToolWindow()
{
    resetSettings();
    static const QString kKey = QStringLiteral("FreeDvReporterGeometry");
    auto& settings = AppSettings::instance();
    {
        FreeDvReporterDialog dlg;
        report("FreeDV Reporter is a CanonWindow",
               qobject_cast<CanonWindow*>(&dlg) != nullptr);
        report("the window installs FramelessResizer",
               dlg.findChild<FramelessResizer*>() != nullptr);
    }

    // A rectangle saved before the migration: QWidget::saveGeometry() in base64.
    FreeDvReporterDialog dlg;
    // Taller than the minimum, at the minimum width: the offscreen platform's
    // screen is 800x600 and restoreGeometry() fits a larger rectangle to it.
    const QSize chosen = dlg.minimumSize() + QSize(0, 60);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    report("control: a fresh window is not born at the saved size", dlg.size() != chosen);
    dlg.show();
    report("the window is exposed", QTest::qWaitForWindowExposed(&dlg));
    for (int i = 0; i < 50 && dlg.size() != chosen; ++i)
        QTest::qWait(20);
    report("geometry saved by the old dialog restores under its key",
           dlg.size() == chosen,
           QStringLiteral("%1x%2").arg(dlg.width()).arg(dlg.height()));

    const QSize resized = chosen + QSize(40, 20);
    dlg.resize(resized);
    QCoreApplication::processEvents();
    dlg.hide();
    const QString saved = settings.value(kKey).toString();
    report("hiding the window saves its size under the same key",
           saved.endsWith(QStringLiteral(",%1,%2").arg(resized.width()).arg(resized.height())),
           saved);
}

// The message field's standard right-click menu is a child of the field, so
// the canon sheet's transparent-everything rule reaches it unless QMenu gets
// its panel.
void testMessageMenuIsOpaque()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    dlg.show();
    report("the window is exposed", QTest::qWaitForWindowExposed(&dlg));
    auto* edit = msgEdit(dlg);
    if (!edit) { report("message edit exists", false); return; }

    QMenu* menu = edit->createStandardContextMenu();
    report("control: the field's menu is parented inside the canon body",
           menu->parentWidget() == edit);
    menu->popup(dlg.mapToGlobal(QPoint(40, 40)));
    QTest::qWait(100);
    const QImage image = menu->grab().toImage();
    report("the message field's right-click menu has an opaque background",
           !image.isNull() && qAlpha(image.pixel(image.width() / 2, image.height() / 2)) == 255);
    menu->close();
    delete menu;
}

// The explicit minimum must not let the window shrink below what the band
// row needs, or the Track / Band labels clip. Checked on the shown window at
// its minimum size, after the canon sheet has polished every control.
void testMinimumFitsBandRow()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    report("minimum width fits the layout",
           dlg.minimumWidth() >= dlg.minimumSizeHint().width(),
           QStringLiteral("min %1 hint %2").arg(dlg.minimumWidth()).arg(dlg.minimumSizeHint().width()));

    dlg.resize(dlg.minimumSize());
    dlg.show();
    report("the window is exposed", QTest::qWaitForWindowExposed(&dlg));
    QTest::qWait(50);
    QStringList clipped;
    for (QAbstractButton* b : dlg.findChildren<QAbstractButton*>()) {
        if (!b->isVisible() || b->text().isEmpty())
            continue;
        if (b->width() < b->minimumSizeHint().width())
            clipped << QStringLiteral("%1 %2<%3").arg(b->text()).arg(b->width())
                                                     .arg(b->minimumSizeHint().width());
    }
    report("at minimum size no labelled control is narrower than it needs",
           clipped.isEmpty(), clipped.join(QStringLiteral(", ")));
}

// A band clicked off must not keep the focus border, which matches the
// checked border; keyboard users still reach the buttons with Tab.

void testBandButtonsTakeTabFocusOnly()
{
    resetSettings();

    FreeDvReporterDialog dlg;
    int bands = 0;
    bool tabOnly = true;
    for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
        if (!b->property("fdvBand").toBool()) continue;
        ++bands;
        tabOnly = tabOnly && b->focusPolicy() == Qt::TabFocus;
    }
    report("the window has every band button",
           bands == FreeDvReporterDialog::BandCount, QString::number(bands));
    report("band buttons take focus from Tab, not from a click", tabOnly);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-fdv-reporter-msg-test"));
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    std::printf("FreeDV Reporter status-message test harness\n\n");

    testReportingGate();
    testDisabledStateStillExplains();
    testSendCommitsMessage();
    testLengthCap();
    testReloadFromSettings();
    testCanonToolWindow();
    testMessageMenuIsOpaque();
    testMinimumFitsBandRow();
    testBandButtonsTakeTabFocusOnly();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
