#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "core/MemoryCsvCompat.h"
#include "gui/ProfileManagerDialog.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/MemoryDialog.h"
#include "core/ThemeManager.h"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <cstdio>
#include <utility>

using namespace AetherSDR;

namespace AetherSDR {
class StoredControlTestAccess {
public:
    static void memory(RadioModel& model, const MemoryDelta& delta) { model.applyMemoryChanges(delta); }
    static void profile(RadioModel& model, const ProfileDelta& delta) { model.applyProfileChanges(delta); }
};
class StoredDialogsTestAccess {
public:
    static QTableWidget* table(MemoryDialog& dialog) { return dialog.m_table; }
    static void populate(MemoryDialog& dialog) { dialog.populateTable(); }
    static void submit(MemoryDialog& dialog, int row, int column) { dialog.submitCellEdit(row, column); }
    static void importRows(MemoryDialog& dialog, const MemoryCsvParseResult& parsed)
    { dialog.startImport(parsed, QStringLiteral("test.csv"), dialog.m_model->controlSession(), dialog.m_model->usesLocalMemoryBank()); }
};
}

namespace {
int failures{0};
void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void profileDialog()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    ProfileManagerDialog dialog(&model);
    QLineEdit* edit = dialog.findChild<QLineEdit*>("profileNameEdit_global");
    QPushButton* save = dialog.findChild<QPushButton*>("profileSaveBtn_global");
    QLabel* status = dialog.findChild<QLabel*>("profileStatus_global");
    check(edit && save && status, "profile controls retain their identities");
    if (!edit || !save || !status) { return; }
    edit->setText("Test profile"); save->click();
    check(backend->commands == QStringList{"profile global save \"Test profile\""}, "dialog save goes through typed seam");
    const RadioControlCompletion first = backend->replies.at(0);
    edit->setText("Second profile"); save->click();
    const RadioControlCompletion second = backend->replies.at(1);
    first({RadioControlReply::Kind::Accepted, 0, {}});
    check(edit->text() == "Second profile" && status->text().contains("Saving"), "superseded save cannot clear new input or status");
    second({RadioControlReply::Kind::Failed, 9, "refused"});
    check(edit->text() == "Second profile" && status->text().contains("refused"), "failed save retains target");
    edit->setText("Bad\"name"); save->click();
    check(backend->commands.size() == 2 && edit->text() == "Bad\"name", "unsafe name neither writes nor clears input");
    edit->setText("Final profile"); save->click();
    const RadioControlCompletion pending = backend->replies.back();
    RadioSetupControlTestAccess::replaceConnectionGeneration(model);
    pending({RadioControlReply::Kind::Accepted, 0, {}});
    check(edit->text() == "Final profile" && !status->text().contains("Saved profile"), "stale save never reports success on replacement session");
}
void memoryDialog()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    MemoryDelta delta; delta.index = 7; delta.name = "Before"; delta.mode = "FM";
    delta.freq = 146.52; delta.group = "Test";
    StoredControlTestAccess::memory(model, delta);
    MemoryDialog dialog(&model);
    StoredDialogsTestAccess::populate(dialog);
    QTableWidget* table = StoredDialogsTestAccess::table(dialog);
    check(table && table->rowCount() == 1, "memory observation populates dialog");
    if (!table || table->rowCount() != 1) { return; }
    // Block itemChanged while constructing the edit, then use the actual submission path.
    table->blockSignals(true); table->item(0, 3)->setText("After name"); table->blockSignals(false);
    StoredDialogsTestAccess::submit(dialog, 0, 3);
    check(backend->commands == QStringList{QStringLiteral("memory set 7 name=After") + QChar(0x7f) + QStringLiteral("name")},
          "single-cell edit sends only its typed field");
    const RadioControlCompletion edit = backend->replies.at(0);
    edit({RadioControlReply::Kind::Accepted, 0, {}});
    check(model.memories().value(7).name == "After name", "accepted edit reconciles cache");
    table->blockSignals(true); table->item(0, 2)->setText("nan"); table->blockSignals(false);
    StoredDialogsTestAccess::submit(dialog, 0, 2);
    check(backend->commands.size() == 1 && model.memories().value(7).freq == 146.52,
          "malformed cell cannot reach transport or change memory");
}
void stoppedImport()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    MemoryDialog dialog(&model);
    QList<MemoryCsvRecord> records;
    for (const QString& name : {QStringLiteral("First row"), QStringLiteral("Second row"), QStringLiteral("Third row")}) {
        MemoryEntry entry; entry.name = name; entry.freq = 14.1; entry.mode = "USB"; entry.step = 1000;
        records.append(MemoryCsvCompat::fromMemoryEntry(entry));
    }
    const QByteArray bytes = MemoryCsvCompat::serialize(records);
    const MemoryCsvParseResult parsed = MemoryCsvCompat::parse(bytes);
    if (parsed.records.size() != 3 || !parsed.errors.isEmpty()) { check(false, "three synthetic CSV records parse"); return; }
    QString summary;
    QString details;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &dialog, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<CanonMessageBox*>(widget); box && box->isVisible()) {
                const auto* info = box->findChild<QLabel*>(QStringLiteral("canonMessageInfo"));
                const auto* list = box->findChild<QPlainTextEdit*>(QStringLiteral("canonMessageDetails"));
                summary = info ? info->text() : QString();
                details = list ? list->toPlainText() : QString();
                box->accept();
            }
        }
    });
    driver.start(20);
    QTimer::singleShot(10000, &dialog, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QDialog* modal = qobject_cast<QDialog*>(widget); modal && modal->isModal()) { modal->reject(); }
        }
    });
    StoredDialogsTestAccess::importRows(dialog, parsed);
    if (backend->commands.size() != 1 || backend->replies.empty()) { check(false, "import starts first typed create"); return; }
    const RadioControlCompletion created = backend->replies.at(0);
    created({RadioControlReply::Kind::Accepted, 0, "9"});
    if (backend->replies.size() != 2) { check(false, "import populates first created slot"); return; }
    const RadioControlCompletion populated = backend->replies.at(1);
    populated({RadioControlReply::Kind::Accepted, 0, {}});
    RadioSetupControlTestAccess::replaceConnectionGeneration(model);
    QTest::qWait(100);
    check(backend->commands.size() == 2, "queued import never writes to a replacement session");
    if (!summary.startsWith("2 rows were skipped") || !details.contains("Second row") || !details.contains("Third row")) {
        std::fprintf(stderr, "Import summary: %s\nImport details: %s\n", qPrintable(summary), qPrintable(details));
    }
    check(summary.startsWith("2 rows were skipped") && details.contains("Second row") && details.contains("Third row"),
          "queued import reports every unattempted row after session replacement");
}

// The window is a canon workspace with its old geometry key, and its sheet is
// canon: every token it uses resolves in its canon/memory scope, under Default
// Dark and Default Light, and the old literal selection blue is gone.
void memoryCanonWindow()
{
    RadioModel model;
    installRadioSetupBackend(model);
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        MemoryDialog dialog(&model);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "memory window is a CanonWindow");
        check(dialog.windowTitle() == "Memory Channels", "memory window keeps its title");
        check(dialog.windowType() == Qt::Window, "memory window is a workspace, not a dialog");
        check(dialog.property("themeContainer").toString() == "canon/memory",
              "memory window uses the canon/memory scope");
        bool header = false;
        for (QLabel* label : dialog.findChildren<QLabel*>())
            header = header || (label->objectName() == "canonHeaderTitle" && label->text() == "Memory Channels");
        check(header, "canon header carries the title");
        const QString sheet = dialog.bodyWidget()->styleSheet();
        for (const char* token : {"color.canon.ink", "color.canon.inkSoft", "color.canon.muted",
                                  "color.canon.nested", "color.canon.raised", "color.canon.control",
                                  "color.canon.line", "color.canon.lineHi", "color.canon.cyan",
                                  "color.canon.aqua", "color.canon.onAccent"}) {
            const QString value = tm.cssFragment(dialog.bodyWidget(), QLatin1String(token));
            check(!value.isEmpty() && sheet.contains(value), token);
        }
        check(!sheet.contains(": ;"), "no token in the memory sheet resolved empty");
        check(!sheet.contains("#2060a0", Qt::CaseInsensitive)
                  && !StoredDialogsTestAccess::table(dialog)->styleSheet().contains("#2060a0", Qt::CaseInsensitive),
              "old literal selection blue is gone");
    }
    tm.setActiveTheme(before);
}

// Remove asks a CanonMessageBox: Cancel (the default) and closing it send
// nothing; Delete sends the removal.
void memoryRemoveConfirm()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    MemoryDelta delta; delta.index = 7; delta.name = "Doomed"; delta.mode = "FM";
    delta.freq = 146.52; delta.group = "Test";
    StoredControlTestAccess::memory(model, delta);
    MemoryDialog dialog(&model);
    StoredDialogsTestAccess::populate(dialog);
    QTableWidget* table = StoredDialogsTestAccess::table(dialog);
    QPushButton* remove = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>())
        if (button->text() == "Remove") remove = button;
    if (!table || table->rowCount() != 1 || !remove) { check(false, "memory row and Remove button exist"); return; }
    table->clearSelection();
    check(!remove->isEnabled()
              && remove->accessibleDescription() == "Select one or more memories to remove.",
          "a disabled Remove says why, to screen readers too");

    // answer: "Cancel", "Delete", or "" to close the box without a button.
    // A repeating driver answers once the box is up, however late exec()
    // shows it; a safety timer closes any box left open so a miss fails a
    // check instead of hanging.
    auto run = [&](const QString& answer) {
        QString seenTitle;
        QString seenDefault;
        bool answered = false;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, &dialog, [&] {
            if (answered) return;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<CanonMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                answered = true;
                seenTitle = box->windowTitle();
                for (QPushButton* button : box->findChildren<QPushButton*>())
                    if (button->isDefault()) seenDefault = button->text();
                if (answer.isEmpty()) { box->reject(); return; }
                for (QPushButton* button : box->findChildren<QPushButton*>())
                    if (button->text() == answer) { button->click(); return; }
                box->reject();
                return;
            }
        });
        driver.start(20);
        QTimer safety;
        safety.setSingleShot(true);
        QObject::connect(&safety, &QTimer::timeout, &dialog, [] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<CanonMessageBox*>(widget); box && box->isVisible())
                    box->reject();
        });
        safety.start(10000);
        table->selectRow(0);
        remove->click();
        QTest::qWait(20);
        return std::make_pair(seenTitle, seenDefault);
    };

    const auto [title, defaultButton] = run(QStringLiteral("Cancel"));
    check(title == "Delete Memory", "Remove asks in a CanonMessageBox");
    check(defaultButton == "Cancel", "Cancel is the delete question's default");
    check(backend->commands.isEmpty(), "Cancel sends nothing");
    run(QString());
    check(backend->commands.isEmpty(), "closing the question sends nothing");
    run(QStringLiteral("Delete"));
    check(backend->commands.size() == 1 && backend->commands.value(0).startsWith("memory remove 7"),
          "Delete sends the removal");
    if (backend->commands.size() != 1)
        std::fprintf(stderr, "commands: %s\n", qPrintable(backend->commands.join(" | ")));
}

// Profile Manager on CanonWindow (#7029): a workspace in canon/profileManager
// with no hex left in its sheets, no footer Close, and its result line coloured
// by a tone property the canon sheet matches.
void profileCanonWindow()
{
    RadioModel model;      // no backend: not connected, so Save reports an error
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        ProfileManagerDialog dialog(&model);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "profile manager is a CanonWindow");
        check(dialog.windowTitle() == "Profile Manager", "profile manager keeps its title");
        check(dialog.objectName() == "ProfileManagerDialog",
              "profile manager keeps the name screens.json waits for");
        check(dialog.windowType() == Qt::Window, "profile manager is a workspace, not a dialog");
        check(dialog.property("themeContainer").toString() == "canon/profileManager",
              "profile manager uses the canon/profileManager scope");
        bool header = false;
        bool closeButton = false;
        for (QLabel* label : dialog.findChildren<QLabel*>())
            header = header || (label->objectName() == "canonHeaderTitle" && label->text() == "Profile Manager");
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
            closeButton = closeButton || button->text() == "Close";
        check(header, "canon header carries the title");
        check(!closeButton, "the footer Close is gone; the canon close paths replace it");
        const QString sheet = dialog.bodyWidget()->styleSheet();
        check(!sheet.contains(": ;"), "no token in the profile sheet resolved empty");
        check(sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the sheet gives the field's menu a panel and its selection an ink");
        // The old sheet's literals that no theme token shares, anywhere in the window.
        bool literal = false;
        for (QWidget* widget : dialog.findChildren<QWidget*>())
            for (const char* hex : {"#6888a0", "#9bd1ff", "#ff8f8f"})
                literal = literal || widget->styleSheet().contains(QLatin1String(hex), Qt::CaseInsensitive);
        check(!literal, "the old hard-coded colours are gone");
        check(dialog.minimumHeight() <= 700, "the minimum height fits a 768 px screen");

        auto* edit = dialog.findChild<QLineEdit*>("profileNameEdit_global");
        auto* save = dialog.findChild<QPushButton*>("profileSaveBtn_global");
        auto* status = dialog.findChild<QLabel*>("profileStatus_global");
        if (!edit || !save || !status) { check(false, "profile controls exist"); continue; }
        edit->setText("Offline"); save->click();
        check(status->property("resultTone").toString() == "error", "a failed save sets the error tone");
        check(status->palette().color(QPalette::WindowText)
                  == tm.color(status, QStringLiteral("color.accent.danger")),
              "the error tone draws in color.accent.danger in this theme");
    }
    tm.setActiveTheme(before);
}

// At the smallest size the shown window allows, on every tab, each visible
// control gets the height it needs (a wrapped label: its height for its
// width). The floor has to come from the shown, polished layout.
void profileLegibleAtMinimum()
{
    RadioModel model;
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        ProfileManagerDialog dialog(&model);
        dialog.show();
        QTest::qWaitForWindowExposed(&dialog);
        auto* tabs = dialog.findChild<QTabWidget*>();
        bool legible = tabs != nullptr;
        for (int t = 0; tabs && t < tabs->count(); ++t) {
            tabs->setCurrentIndex(t);
            dialog.resize(QSize(1, 1));
            QTest::qWait(30);
            for (QWidget* w : tabs->currentWidget()->findChildren<QWidget*>()) {
                if (!w->isVisible()) continue;
                int need = w->minimumSizeHint().height();
                if (auto* label = qobject_cast<QLabel*>(w); label && label->wordWrap())
                    need = label->heightForWidth(label->width());
                if (w->height() < need) {
                    legible = false;
                    std::fprintf(stderr, "  %s tab %d: %s h=%d need=%d\n", theme, t,
                                 w->metaObject()->className(), w->height(), need);
                }
            }
        }
        std::fprintf(stderr, "  %s: shown minimum %dx%d\n", theme, dialog.width(), dialog.height());
        check(legible, "every control on every tab gets its height at the smallest size");
        check(dialog.height() <= 700, "the shown minimum height fits a 768 px screen");
        dialog.hide();
    }
    tm.setActiveTheme(before);
}

// Every way out destroys the WA_DeleteOnClose window showOrRaiseCanon() makes;
// Return in the name field clicks nothing.
void profileClosePaths()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    {
        ProfileManagerDialog dialog(&model);
        dialog.show();
        check(QTest::qWaitForWindowExposed(&dialog), "profile manager is exposed");
        auto* edit = dialog.findChild<QLineEdit*>("profileNameEdit_global");
        if (edit) {
            edit->setText("Typed");
            edit->setFocus();
            QTest::keyClick(edit, Qt::Key_Return);
        }
        check(dialog.isVisible() && backend->commands.isEmpty(),
              "Return in the name field neither saves nor closes");
        dialog.hide();
    }
    for (int path = 0; path < 3; ++path) {
        QPointer<ProfileManagerDialog> dialog = new ProfileManagerDialog(&model);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        const bool exposed = QTest::qWaitForWindowExposed(dialog);
        if (path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
        if (path == 1) {
            // A window shortcut: it needs the window active, which a desktop
            // gives a newly shown window but a background test process may not.
            dialog->activateWindow();
            QTest::qWaitForWindowActive(dialog);
            QTest::keySequence(dialog, QKeySequence::Close);
        }
        if (path == 2) {
            // Press the corner ✕ itself, so its connection is pinned too.
            QToolButton* corner = nullptr;
            for (QToolButton* b : dialog->findChildren<QToolButton*>())
                if (b->accessibleName() == QLatin1String("Close")) corner = b;
            check(corner != nullptr, "the corner close button exists");
            if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && dialog.isNull(),
              path == 0 ? "Esc destroys the profile manager"
              : path == 1 ? "Ctrl+W / Cmd+W destroys the profile manager"
                          : "the corner close button destroys the profile manager");
        delete dialog.data();
    }
}

// A destructive profile question (Delete, or CW Reset) asks a CanonMessageBox
// with Cancel as the default: Cancel, Return, Esc and closing it send nothing;
// only the destructive button acts.
void profileDestructiveConfirm(const QString& type, const QString& buttonName,
                               const QString& confirmText, const QString& title,
                               const QString& verb)
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    // CW profiles need SmartSDR 4.3.16; the page is dimmed without it.
    backend->setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
    ProfileDelta delta; delta.type = type;
    delta.list = QStringList{"Doomed"};
    StoredControlTestAccess::profile(model, delta);
    ProfileManagerDialog dialog(&model);
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("profileList_%1").arg(type));
    auto* remove = dialog.findChild<QPushButton*>(buttonName);
    if (!list || list->count() != 1 || !remove) {
        check(false, "the profile row and its destructive button exist");
        return;
    }
    list->setCurrentRow(0);
    check(remove->isEnabled(), "the destructive button is enabled for the selected row");

    enum class Answer { Cancel, Delete, Close, Return, Escape };
    auto run = [&](Answer answer) {
        QString seenTitle, seenDefault, seenDestructive;
        bool answered = false;
        QTimer driver;
        QObject::connect(&driver, &QTimer::timeout, &dialog, [&] {
            if (answered) return;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<CanonMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                answered = true;
                seenTitle = box->windowTitle();
                for (QPushButton* button : box->findChildren<QPushButton*>())
                    if (button->isDefault()) seenDefault = button->text();
                if (answer == Answer::Close) { box->close(); return; }
                if (answer == Answer::Return) { QTest::keyClick(box->focusWidget() ? box->focusWidget() : box, Qt::Key_Return); return; }
                if (answer == Answer::Escape) { QTest::keyClick(box, Qt::Key_Escape); return; }
                const QString text = answer == Answer::Delete ? confirmText : QStringLiteral("Cancel");
                for (QPushButton* button : box->findChildren<QPushButton*>())
                    if (button->text() == text) { button->click(); return; }
                box->reject();
                return;
            }
        });
        driver.start(20);
        QTimer safety;
        safety.setSingleShot(true);
        QObject::connect(&safety, &QTimer::timeout, &dialog, [] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* box = qobject_cast<CanonMessageBox*>(widget); box && box->isVisible())
                    box->reject();
        });
        safety.start(10000);
        list->setCurrentRow(0);
        remove->click();
        QTest::qWait(20);
        return std::make_pair(seenTitle, seenDefault);
    };

    std::fprintf(stderr, "  %s\n", qPrintable(title));
    const auto [seenTitle, defaultButton] = run(Answer::Cancel);
    check(seenTitle == title, "it asks in a CanonMessageBox");
    check(defaultButton == "Cancel", "Cancel is the question's default");
    check(backend->commands.isEmpty(), "Cancel sends nothing");
    run(Answer::Return);
    check(backend->commands.isEmpty(), "Return on the question sends nothing");
    run(Answer::Escape);
    check(backend->commands.isEmpty(), "Esc on the question sends nothing");
    run(Answer::Close);
    check(backend->commands.isEmpty(), "closing the question sends nothing");
    run(Answer::Delete);
    check(backend->commands.size() == 1 && backend->commands.value(0).contains("Doomed")
              && backend->commands.value(0).startsWith(QStringLiteral("profile %1 %2").arg(type == "global" ? "global" : type, verb)),
          "the destructive button sends its command");
    if (backend->commands.size() != 1)
        std::fprintf(stderr, "commands: %s\n", qPrintable(backend->commands.join(" | ")));
}

// Size and position persist under ProfileManagerDialogGeometry, the key the
// old dialog wrote, including its base64 rectangles.
void profileGeometryCarriesOver()
{
    static const QString kKey = QStringLiteral("ProfileManagerDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    RadioModel model;
    ProfileManagerDialog restored(&model);
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 20);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    check(restored.size() != chosen, "control: a fresh profile manager is not born at the saved size");
    restored.show();
    check(QTest::qWaitForWindowExposed(&restored), "profile manager is exposed");
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");
    restored.hide();
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("stored-control-dialogs-test"));
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!profile.isValid()) { return 2; }
    AppSettings::instance().load();
    profileDialog(); memoryDialog(); stoppedImport(); memoryCanonWindow(); memoryRemoveConfirm();
    profileCanonWindow(); profileLegibleAtMinimum(); profileClosePaths(); profileDestructiveConfirm("global", "profileDeleteBtn_global", "Delete", "Delete Profile", "delete");
    profileDestructiveConfirm("cw", "profileResetBtn_cw", "Reset", "Reset CW Profile", "reset"); profileGeometryCarriesOver();
    return failures == 0 ? 0 : 1;
}
