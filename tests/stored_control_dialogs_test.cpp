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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
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
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("stored-control-dialogs-test"));
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!profile.isValid()) { return 2; }
    AppSettings::instance().load();
    profileDialog(); memoryDialog(); stoppedImport(); memoryCanonWindow(); memoryRemoveConfirm();
    return failures == 0 ? 0 : 1;
}
