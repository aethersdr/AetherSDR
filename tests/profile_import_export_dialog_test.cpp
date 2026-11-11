// Import/Export Profiles on the style guide's CanonWindow (RFC #6226, #7032):
// a canon workspace whose "transfer still running" question guards every
// close path (Esc, ⌘W / Ctrl+W, the corner ✕), not just the close event, and
// whose import confirmation defaults to Cancel. Socket-free: transfers are
// marked busy through ProfileTransferTestAccess, never started.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ProfileTransfer.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/ProfileImportExportDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace AetherSDR {
class ProfileTransferTestAccess {
public:
    static void markBusy(ProfileTransfer& transfer)
    {
        transfer.begin(ProfileTransfer::Operation::ExportDatabase,
                       ProfileTransfer::Phase::DownloadPackage);
    }
};
class ProfileImportExportDialogTestAccess {
public:
    static bool confirmImport(ProfileImportExportDialog& dialog) { return dialog.confirmImport(); }
    static QLineEdit* importPath(ProfileImportExportDialog& dialog) { return dialog.m_importPath; }
};
} // namespace AetherSDR

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

// Answers the first CanonMessageBox titled `title` with `answer` once it is
// up, and dismisses any other box (a "did not complete" notice) with its
// default. A safety timer closes whatever is left so a miss fails a check
// rather than hanging. Returns the box's default button text.
struct BoxDriver {
    QString title;
    std::function<void(CanonMessageBox*)> answer;
    QString seenDefault;
    QStringList seenButtons;
    bool seen{false};
    QTimer timer;
    QTimer safety;

    BoxDriver(QString t, std::function<void(CanonMessageBox*)> a)
        : title(std::move(t)), answer(std::move(a))
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<CanonMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                if (box->windowTitle() != title || seen) { box->reject(); return; }
                seen = true;
                for (QPushButton* button : box->findChildren<QPushButton*>()) {
                    seenButtons << button->text();
                    if (button->isDefault()) seenDefault = button->text();
                }
                answer(box);
                return;
            }
        });
        timer.start(20);
        safety.setSingleShot(true);
        QObject::connect(&safety, &QTimer::timeout, [] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<CanonMessageBox*>(widget); box && box->isVisible())
                    box->reject();
        });
        safety.start(10000);
    }
};

std::function<void(CanonMessageBox*)> clickButton(const QString& text)
{
    return [text](CanonMessageBox* box) {
        for (QPushButton* button : box->findChildren<QPushButton*>())
            if (button->text() == text) { button->click(); return; }
        box->reject();
    };
}

std::function<void(CanonMessageBox*)> pressKey(Qt::Key key)
{
    return [key](CanonMessageBox* box) {
        QTest::keyClick(box->focusWidget() ? box->focusWidget() : box, key);
    };
}

void canonWorkspace()
{
    RadioModel model;
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        ProfileImportExportDialog dialog(&model);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "Import/Export is a CanonWindow");
        check(dialog.windowType() == Qt::Window
                  && dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
              "it is a frameless workspace window");
        check(dialog.windowTitle() == "Import/Export Profiles", "the title is unchanged");
        check(dialog.objectName() == "ProfileImportExportDialog",
              "it keeps the name screens.json waits for");
        check(dialog.property("themeContainer").toString() == "canon/profileIO",
              "it uses the canon/profileIO scope");
        check(dialog.findChild<QLabel*>("canonHeaderTitle") != nullptr, "it carries the canon header");
        bool closeButton = false;
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
            closeButton = closeButton || button->text() == "Close";
        check(!closeButton, "the footer Close is gone; the canon close paths replace it");
        auto* cancel = dialog.findChild<QPushButton*>("profileIoCancel");
        check(cancel && !cancel->isEnabled(), "the transfer Cancel is there, disabled while idle");
        const QString sheet = dialog.bodyWidget()->styleSheet();
        check(!sheet.contains(": ;"), "no token in the sheet resolved empty");
        check(sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the path fields' menus keep a panel and their selection an ink");
        bool literal = false;
        for (QWidget* widget : dialog.findChildren<QWidget*>())
            for (const char* hex : {"#9fb0c0", "#406080", "#607080"})
                literal = literal || widget->styleSheet().contains(QLatin1String(hex), Qt::CaseInsensitive);
        check(!literal, "the old hard-coded colours are gone");
    }
    tm.setActiveTheme(before);
}

void close(ProfileImportExportDialog* dialog, int path)
{
    if (path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
    if (path == 1) {
        // ⌘W / Ctrl+W is a window shortcut, so it needs the window active. A
        // desktop hands activation back when the question closes; the
        // offscreen platform does not, so the test gives it back.
        dialog->activateWindow();
        QTest::qWaitForWindowActive(dialog);
        QTest::keySequence(dialog, QKeySequence::Close);
    }
    if (path == 2) {
        // Press the corner ✕ itself, so its connection is pinned too.
        QToolButton* corner = nullptr;
        for (QToolButton* b : dialog->findChildren<QToolButton*>())
            if (b->accessibleName() == QLatin1String("Close")) corner = b;
        if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        else { check(false, "the corner close button exists"); dialog->close(); }
    }
}

const char* pathName(int path)
{
    return path == 0 ? "Esc" : path == 1 ? "Ctrl+W / Cmd+W" : "the corner close button";
}

// Idle, every close path destroys the WA_DeleteOnClose window at once.
void idleClosePaths()
{
    RadioModel model;
    for (int path = 0; path < 3; ++path) {
        QPointer<ProfileImportExportDialog> dialog = new ProfileImportExportDialog(&model);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        const bool exposed = QTest::qWaitForWindowExposed(dialog);
        close(dialog, path);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::printf("  idle: %s\n", pathName(path));
        check(exposed && dialog.isNull(), "an idle window closes and is destroyed");
        delete dialog.data();
    }
}

// Busy, every close path asks first, Keep Transferring by default. Keep
// Transferring, Return, Esc and closing the question leave the window open and
// the transfer running; Cancel Transfer stops it and closes the window with no
// further notice.
void busyClosePathsAsk()
{
    RadioModel model;
    for (int path = 0; path < 3; ++path) {
        QPointer<ProfileImportExportDialog> dialog = new ProfileImportExportDialog(&model);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        QTest::qWaitForWindowExposed(dialog);
        auto* transfer = dialog->findChild<ProfileTransfer*>();
        if (!transfer) { check(false, "the window owns a ProfileTransfer"); delete dialog.data(); continue; }
        ProfileTransferTestAccess::markBusy(*transfer);
        std::printf("  busy: %s\n", pathName(path));

        const QString question = dialog->windowTitle();
        for (const auto& [answer, label] : std::initializer_list<std::pair<std::function<void(CanonMessageBox*)>, const char*>>{
                 {clickButton("Keep Transferring"), "Keep Transferring"},
                 {pressKey(Qt::Key_Return), "Return on the question"},
                 {pressKey(Qt::Key_Escape), "Esc on the question"},
                 {[](CanonMessageBox* box) { box->close(); }, "closing the question"}}) {
            BoxDriver driver(question, answer);
            close(dialog, path);
            QTest::qWait(20);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(driver.seen, "closing a busy window asks first");
            check(driver.seenDefault == "Keep Transferring", "Keep Transferring is the default");
            std::printf("    answer: %s\n", label);
            check(!dialog.isNull() && dialog->isVisible() && transfer->isBusy(),
                  "the window stays open and the transfer keeps running");
        }

        BoxDriver driver(question, clickButton("Cancel Transfer"));
        int notices = 0;
        QTimer counter;
        QObject::connect(&counter, &QTimer::timeout, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<CanonMessageBox*>(widget);
                    box && box->isVisible() && box->windowTitle().contains("Did Not Complete"))
                    ++notices;
        });
        counter.start(5);
        close(dialog, path);
        QTest::qWait(30);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(dialog.isNull(), "Cancel Transfer stops the transfer and closes the window");
        check(notices == 0, "no 'did not complete' notice stands in the way of that close");
        delete dialog.data();
    }
}

void reentrantCloseAsksOnce()
{
    class QuestionCounter : public QObject {
    public:
        int count{0};
        bool eventFilter(QObject* object, QEvent* event) override
        {
            if (event->type() == QEvent::Show && qobject_cast<CanonMessageBox*>(object)) {
                ++count;
            }
            return false;
        }
    } counter;
    RadioModel model;
    ProfileImportExportDialog dialog(&model);
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);
    auto* transfer = dialog.findChild<ProfileTransfer*>();
    if (!transfer) {
        check(false, "the window owns a transfer for the re-entry check");
        return;
    }
    ProfileTransferTestAccess::markBusy(*transfer);
    qApp->installEventFilter(&counter);
    BoxDriver driver(dialog.windowTitle(), [&](CanonMessageBox* box) {
        const QPointer<CanonMessageBox> question(box);
        dialog.close();
        if (question) {
            clickButton("Keep Transferring")(question);
        }
    });
    dialog.reject();
    qApp->removeEventFilter(&counter);
    check(driver.seen && counter.count == 1 && dialog.isVisible() && transfer->isBusy(),
          "a programmatic close during the busy question does not stack another question");
}

void cancelSurvivesOwnerDeletion()
{
    RadioModel model;
    QPointer<ProfileImportExportDialog> dialog = new ProfileImportExportDialog(&model);
    dialog->show();
    QTest::qWaitForWindowExposed(dialog);
    auto* transfer = dialog->findChild<ProfileTransfer*>();
    if (!transfer) {
        check(false, "the window owns a transfer for the lifetime check");
        delete dialog.data();
        return;
    }
    ProfileTransferTestAccess::markBusy(*transfer);
    QObject::connect(transfer, &ProfileTransfer::failed, qApp, [&] {
        delete dialog.data();
    });
    BoxDriver driver(dialog->windowTitle(), clickButton("Cancel Transfer"));
    dialog->close();
    check(driver.seen && dialog.isNull(),
          "Cancel Transfer survives an observer deleting the owner synchronously");
}

// The import confirmation: Cancel is the default, and Cancel, Return, Esc and
// closing it import nothing; Import says yes. Not connected, so no Export
// Backup First is offered.
void importConfirmDefaultsToCancel()
{
    RadioModel model;
    ProfileImportExportDialog dialog(&model);
    ProfileImportExportDialogTestAccess::importPath(dialog)->setText(
        QStringLiteral("/tmp/ASDR_Config_2026-01-01_00-00-00_v4.1.5.ssdr_cfg"));
    const QString title = QStringLiteral("Confirm Profile Import");

    for (const auto& [answer, label] : std::initializer_list<std::pair<std::function<void(CanonMessageBox*)>, const char*>>{
             {clickButton("Cancel"), "Cancel imports nothing"},
             {pressKey(Qt::Key_Return), "Return imports nothing"},
             {pressKey(Qt::Key_Escape), "Esc imports nothing"},
             {[](CanonMessageBox* box) { box->close(); }, "closing the confirmation imports nothing"}}) {
        BoxDriver driver(title, answer);
        const bool confirmed = ProfileImportExportDialogTestAccess::confirmImport(dialog);
        check(driver.seen && driver.seenDefault == "Cancel", "the confirmation defaults to Cancel");
        check(!confirmed, label);
    }
    BoxDriver driver(title, clickButton("Import"));
    const bool confirmed = ProfileImportExportDialogTestAccess::confirmImport(dialog);
    check(confirmed, "Import confirms");
    check(!driver.seenButtons.contains("Export Backup First"),
          "Export Backup First is offered only when a transfer could start");
}

// Size and position persist under ProfileImportExportDialogGeometry, the key
// the old dialog wrote, including its base64 rectangles.
void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("ProfileImportExportDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    RadioModel model;
    ProfileImportExportDialog restored(&model);
    // Well above the floor the shown layout sets (the constructor's minimum is
    // only a lower bound until then).
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 160);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.size() != chosen, "control: a fresh window is not born at the saved size");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "the window is exposed");
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");
    restored.hide();

    // A size saved under the old 430 px minimum is shorter than the layout
    // needs: it opens at the floor, with nothing crushed.
    QDialog shortLegacy;
    shortLegacy.resize(QSize(560, 430));
    settings.setValue(kKey, QString::fromLatin1(shortLegacy.saveGeometry().toBase64()));
    ProfileImportExportDialog reopened(&model);
    reopened.show();
    QTest::qWaitForWindowExposed(&reopened);
    QTest::qWait(50);
    bool legible = true;
    for (QCheckBox* box : reopened.findChildren<QCheckBox*>())
        legible = legible && (!box->isVisible() || box->height() >= box->minimumSizeHint().height());
    check(reopened.height() >= reopened.minimumHeight() && legible,
          "an old saved size below the floor opens at the floor, legible");
    reopened.hide();
}

// At the minimum size the window can be resized to, every control gets its
// full height: the floor is measured from the shown, polished layout, not
// from the constructor's hint (which the canon sheet has not reached yet).
void controlsLegibleAtMinimum()
{
    RadioModel model;
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        ProfileImportExportDialog dialog(&model);
        dialog.show();
        QTest::qWaitForWindowExposed(&dialog);
        dialog.resize(QSize(1, 1));
        QTest::qWait(50);
        bool legible = true;
        for (QWidget* w : dialog.findChildren<QWidget*>()) {
            if (!w->isVisible() || qobject_cast<QCheckBox*>(w) == nullptr) continue;
            if (w->height() < w->minimumSizeHint().height()) {
                legible = false;
                std::printf("  %s: %s h=%d hint=%d\n", theme, qPrintable(static_cast<QCheckBox*>(w)->text()),
                            w->height(), w->minimumSizeHint().height());
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, dialog.width(), dialog.height());
        check(legible, "every category check box gets its full height at the smallest size");
        check(dialog.height() <= 700, "the shown minimum height fits a 768 px screen");
        dialog.hide();
    }
    tm.setActiveTheme(before);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("profile-import-export-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    canonWorkspace();
    controlsLegibleAtMinimum();
    idleClosePaths();
    busyClosePathsAsk();
    reentrantCloseAsksOnce();
    cancelSurvivesOwnerDeletion();
    importConfirmDefaultsToCancel();
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
