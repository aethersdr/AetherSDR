// Settings Browser guardrail tests (RFC #4603 PR 5, review round #4631).
//
// These pin the two claims the automation bridge cannot drive — it has no
// double-click and no key-injection verb — so they were argued rather than
// demonstrated in review. Both reviewers built standalone harnesses that
// REPRODUCED the wiring; this drives the real dialog instead, so the test
// stays true when the wiring changes.
//
// Each case is falsifiable by construction: break the guard on purpose and
// the paired control assertion diverges.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/FramelessMessageBox.h"
#include "gui/SettingsBrowserDialog.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QToolButton>
#include <functional>
#include <tuple>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QTableWidget* tableOf(SettingsBrowserDialog& dlg)
{
    return dlg.findChild<QTableWidget*>();
}

QTreeWidget* treeOf(SettingsBrowserDialog& dlg)
{
    return dlg.findChild<QTreeWidget*>();
}

int rowForKey(QTableWidget* table, const QString& key)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0) != nullptr
            && table->item(row, 0)->text() == key) {
            return row;
        }
    }
    return -1;
}

// Select the tree item whose label contains `needle` (walks the whole tree).
bool selectScope(QTreeWidget* tree, const QString& needle)
{
    QTreeWidgetItemIterator it(tree);
    while (*it != nullptr) {
        if ((*it)->text(0).contains(needle)
            && ((*it)->flags() & Qt::ItemIsSelectable)) {
            tree->setCurrentItem(*it);
            return true;
        }
        ++it;
    }
    return false;
}

// Deliver a double-click as real events. Under the offscreen platform the
// window is never "active", so QTest::mouseDClick's synthetic clicks are
// dropped before reaching the viewport; press + DblClick is what
// QAbstractItemView actually consumes, and mouseDoubleClickEvent() is the
// emitter of BOTH cellDoubleClicked and cellActivated.
void sendDoubleClick(QTableWidget* table, int row, int column)
{
    const QRect cell = table->visualRect(table->model()->index(row, column));
    const QPointF pos = cell.center();
    const QPointF global = table->viewport()->mapToGlobal(cell.center());
    QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &press);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, global, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &dbl);
}

void settle()
{
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
        QTest::qWait(10);
    }
}

// QObject::receivers() is protected. Reaching it by declaring a subclass and
// downcasting the widget to it is undefined behaviour — no object of that
// subclass ever exists, and UBSan's vptr check rejects the cast outright
// (#4740: "downcast of address ... which does not point to an object of type
// 'Probe'", which reds the weekly ASan+UBSan run since the flags include
// -fno-sanitize-recover=undefined).
//
// Access control is what we need to satisfy, not the object model: a
// pointer-to-member formed inside a derived class may name a protected base
// member, and converting it back to a base member pointer lets us apply it to
// the real object. No fake type, no cast of the instance.
//
// The member belongs to QObject, so that is what the probe derives from — the
// widget type is irrelevant to the access we are borrowing. Keep the
// static_cast: on GCC and Clang &Probe::receivers already has the QObject
// member-pointer type and it converts nothing, but derived-to-base is NOT an
// implicit member-pointer conversion (only base-to-derived is), so on any
// implementation that types it as Probe's the cast is what makes this compile.
bool cellActivatedIsUnconnected(QTableWidget* table)
{
    struct Probe : QObject {
        static int receiversOf(const QObject* object, const char* signal)
        {
            return (object->*static_cast<int (QObject::*)(const char*) const>(
                        &Probe::receivers))(signal);
        }
    };
    return Probe::receiversOf(table, SIGNAL(cellActivated(int, int))) == 0;
}

// Double-click the centre of a cell and count how many modal viewers the
// gesture opened. The viewer runs a nested exec(), so the counting timer
// fires INSIDE that loop: it closes whatever modal is up and tallies it,
// which is what makes a second viewer observable rather than invisible.
int viewersOpenedByDoubleClick(QTableWidget* table, int row, int column)
{
    int opened = 0;
    int dblSignals = 0;
    QObject::connect(table, &QTableWidget::cellDoubleClicked, table,
                     [&dblSignals](int, int) { ++dblSignals; });
    int activatedSignals = 0;
    QObject::connect(table, &QTableWidget::cellActivated, table,
                     [&activatedSignals](int, int) { ++activatedSignals; });
    auto* closer = new QTimer(table);
    closer->setInterval(10);
    QObject::connect(closer, &QTimer::timeout, table, [&opened] {
        if (QWidget* modal = QApplication::activeModalWidget()) {
            ++opened;
            modal->close();
        }
    });
    closer->start();

    sendDoubleClick(table, row, column);
    settle();
    closer->stop();
    closer->deleteLater();
    std::printf("       (cellDoubleClicked: %d  cellActivated: %d  "
                "ActivateItemOnSingleClick: %d  itemAt non-null: %d)\n",
                dblSignals, activatedSignals,
                table->style()->styleHint(
                    QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr,
                    table),
                table->itemAt(table->visualRect(table->model()->index(row, column))
                                  .center()) != nullptr);
    return opened;
}

void seedStore()
{
    AppSettings& s = AppSettings::instance();
    s.load();

    // A flat row whose KEY is innocuous but whose VALUE hides a credential
    // field — the bot's data-loss finding. Display masks it; editability
    // must follow, or committing writes "[REDACTED]" over the real value.
    s.setValue(QStringLiteral("VoiceChainCfg"),
               QStringLiteral("{\"enabled\":true,"
                              "\"asr_remote_api_key\":\"sk-live-PROBE\"}"));
    // The control: same shape, no credential-shaped field anywhere. The
    // key ORDER and number format are deliberately not canonical — a
    // re-serialising display path would reorder to {"alpha":2.5,...} and
    // the divergence would be visible, which is what makes the
    // display-equals-stored-bytes assertion discriminating rather than
    // accidentally true.
    s.setValue(QStringLiteral("PlainCfg"),
               QStringLiteral("{\"zulu\":1, \"alpha\":2.50}"));
    s.save();

    // Radio-scope documents: clean, and present-but-unparseable.
    s.setRadioFeature(QStringLiteral("hl2"), QStringLiteral("AA:BB:CC:DD:EE:01"),
                      QStringLiteral("CleanDoc"), 1,
                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("CW")}});
}

void testMaskedRowsAreReadOnly()
{
    SettingsBrowserDialog dlg;
    QTableWidget* table = tableOf(dlg);
    expect(table != nullptr, "the browser builds its table");
    if (table == nullptr) {
        return;
    }

    const int secretRow = rowForKey(table, QStringLiteral("VoiceChainCfg"));
    const int plainRow = rowForKey(table, QStringLiteral("PlainCfg"));
    expect(secretRow >= 0 && plainRow >= 0,
           "both probe rows are present in the App scope");
    if (secretRow < 0 || plainRow < 0) {
        return;
    }

    const QTableWidgetItem* secretItem = table->item(secretRow, 1);
    const QTableWidgetItem* plainItem = table->item(plainRow, 1);

    expect(secretItem->text().contains(QStringLiteral("[REDACTED]"))
               && !secretItem->text().contains(QStringLiteral("sk-live-PROBE")),
           "a nested credential field renders redacted in the value cell");
    expect(!(secretItem->flags() & Qt::ItemIsEditable),
           "a row whose DISPLAY is masked is read-only — editing it would "
           "commit the mask over the real value");

    // The control proves the guard is doing work rather than the whole
    // table being read-only: an identically-shaped clean row IS editable.
    expect(plainItem->text().contains(QStringLiteral("zulu")),
           "control: the clean JSON row renders verbatim");
    expect(plainItem->flags() & Qt::ItemIsEditable,
           "control: a clean JSON row stays editable");
}

void testDoubleClickOpensOneViewer()
{
    SettingsBrowserDialog dlg;
    dlg.resize(900, 600);
    dlg.show();
    expect(QTest::qWaitForWindowExposed(&dlg), "the settings browser window is exposed");

    QTreeWidget* tree = treeOf(dlg);
    QTableWidget* table = tableOf(dlg);
    expect(tree != nullptr && table != nullptr, "tree and table exist");
    if (tree == nullptr || table == nullptr) {
        return;
    }
    expect(selectScope(tree, QStringLiteral("AA:BB:CC:DD:EE:01")),
           "the seeded radio scope is selectable in the tree");
    QApplication::processEvents();

    const int docRow = rowForKey(table, QStringLiteral("CleanDoc"));
    expect(docRow >= 0, "the seeded document is listed in the radio scope");
    if (docRow < 0) {
        return;
    }

    // Structural assertion FIRST, before this test attaches its own probes:
    // the dialog must not listen to cellActivated at all. Both signals fire
    // from mouseDoubleClickEvent (measured below), and `activated` ALSO
    // fires on a single click under styles where ActivateItemOnSingleClick
    // is true — a modal on a stray click. Asserting the connection's absence
    // is falsifiable in a way that counting viewers is not: re-adding the
    // connection is masked at runtime by the post-exec repopulate
    // invalidating the held index (NF0T's finding — load-bearing by
    // accident), so only the wiring itself is a reliable guard.
    expect(cellActivatedIsUnconnected(table),
           "the table's cellActivated is NOT connected — double-click opens "
           "via cellDoubleClicked alone, and a single click never can");

    const int opened = viewersOpenedByDoubleClick(table, docRow, 2);
    std::printf("       (viewers opened by one double-click: %d)\n", opened);
    expect(opened == 1,
           "a double-click opens exactly one document viewer");
}

void testCorruptDocumentIsNotEditable()
{
    // The unparseable document (planted in main()) reads back through the
    // typed API as an empty object — indistinguishable from an absent row —
    // so the viewer must judge the stored BYTES.
    SettingsBrowserDialog dlg;
    dlg.resize(900, 600);
    dlg.show();
    expect(QTest::qWaitForWindowExposed(&dlg), "the settings browser window is exposed");
    QTreeWidget* tree = treeOf(dlg);
    QTableWidget* table = tableOf(dlg);
    if (tree == nullptr || table == nullptr
        || !selectScope(tree, QStringLiteral("AA:BB:CC:DD:EE:01"))) {
        expect(false, "radio scope reachable for the corrupt-document case");
        return;
    }
    QApplication::processEvents();

    const int docRow = rowForKey(table, QStringLiteral("CorruptDoc"));
    expect(docRow >= 0, "the unparseable document still LISTS (it exists)");
    if (docRow < 0) {
        return;
    }

    // Open the viewer and inspect it from inside its own nested loop.
    bool sawNotParse = false;
    bool editDisabled = false;
    auto* probe = new QTimer(table);
    probe->setInterval(10);
    QObject::connect(probe, &QTimer::timeout, table,
                     [&sawNotParse, &editDisabled] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr) {
            return;
        }
        const auto labels = modal->findChildren<QLabel*>();
        for (const QLabel* label : labels) {
            if (label->text().contains(QStringLiteral("DOES NOT PARSE"))) {
                sawNotParse = true;
            }
        }
        const auto buttons = modal->findChildren<QPushButton*>();
        for (const QPushButton* button : buttons) {
            if (button->text().startsWith(QStringLiteral("Edit Raw JSON"))) {
                editDisabled = !button->isEnabled();
            }
        }
        modal->close();
    });
    probe->start();

    sendDoubleClick(table, docRow, 2);
    settle();
    probe->stop();

    expect(sawNotParse,
           "an unparseable document is labelled as such, not shown as empty");
    expect(editDisabled,
           "Edit Raw JSON is disabled on an unparseable document — saving "
           "would overwrite recoverable bytes");
}

// A stored JSON ARRAY parses cleanly but is not an object, so
// radioFeatureExact() rejects it and returns {} at version 0 — a
// parse-only check would call the row healthy, enable Edit, and let Save
// write {} while DOWNGRADING schema_version to 0 (setRadioFeature has no
// version guard; the upsert takes excluded.schema_version). The viewer
// must mirror the API's own condition (PR #4631 review, NF0T).
void testArrayDocumentIsTreatedAsCorrupt()
{
    SettingsBrowserDialog dlg;
    dlg.resize(900, 600);
    dlg.show();
    expect(QTest::qWaitForWindowExposed(&dlg), "the settings browser window is exposed");
    QTreeWidget* tree = treeOf(dlg);
    QTableWidget* table = tableOf(dlg);
    if (tree == nullptr || table == nullptr
        || !selectScope(tree, QStringLiteral("AA:BB:CC:DD:EE:01"))) {
        expect(false, "radio scope reachable for the array-document case");
        return;
    }
    QApplication::processEvents();

    const int docRow = rowForKey(table, QStringLiteral("ArrayDoc"));
    expect(docRow >= 0, "the array document lists in the radio scope");
    if (docRow < 0) {
        return;
    }

    bool editDisabled = false;
    bool sawNotParse = false;
    auto* probe = new QTimer(table);
    probe->setInterval(10);
    QObject::connect(probe, &QTimer::timeout, table,
                     [&editDisabled, &sawNotParse] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr) {
            return;
        }
        for (const QLabel* label : modal->findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("DOES NOT PARSE"))) {
                sawNotParse = true;
            }
        }
        for (const QPushButton* button : modal->findChildren<QPushButton*>()) {
            if (button->text().startsWith(QStringLiteral("Edit Raw JSON"))) {
                editDisabled = !button->isEnabled();
            }
        }
        modal->close();
    });
    probe->start();
    sendDoubleClick(table, docRow, 2);
    settle();
    probe->stop();

    expect(sawNotParse && editDisabled,
           "a stored JSON ARRAY is treated as unreadable, not as an empty "
           "document at v0 — Edit stays disabled");

    // The row must still be intact afterwards: nothing wrote {} over it.
    QString stillThere;
    SettingsDatabase check;
    bool intact = false;
    if (check.open(SettingsPaths::databasePath())) {
        int version = 0;
        intact = check.readRadioFeature(QStringLiteral("hl2"),
                                        QStringLiteral("AA:BB:CC:DD:EE:01"),
                                        QStringLiteral("ArrayDoc"), version,
                                        stillThere)
                 && stillThere.startsWith(QLatin1Char('['))
                 && version == 4;
        check.close();
    }
    expect(intact,
           "the array document survives the visit with its bytes and schema "
           "version 4 intact");
}

// Editable rows must display exactly what is stored: redactedValue()
// re-serialises JSON (keys sort, numbers normalise), so routing a CLEAN
// row through it would leave the cell showing something the store does
// not contain — and committing would persist that (PR #4631, NF0T).
void testEditableRowShowsStoredBytes()
{
    const QString stored =
        AppSettings::instance().value(QStringLiteral("PlainCfg")).toString();
    SettingsBrowserDialog dlg;
    QTableWidget* table = tableOf(dlg);
    const int row = rowForKey(table, QStringLiteral("PlainCfg"));
    if (row < 0) {
        expect(false, "the clean JSON row is present");
        return;
    }
    const QTableWidgetItem* item = table->item(row, 1);
    expect(item->text() == stored,
           "an editable JSON row displays the stored bytes verbatim");
    expect(item->flags() & Qt::ItemIsEditable,
           "control: that row is the editable one (masked rows may diverge)");
}

void testAddKeyNestedDialogSurvivesPersistentParentClose()
{
    // Match MainWindow::showOrRaiseCanon(): the browser is a top-level canon
    // workspace that deletes itself on close, then opens Add Key.
    QPointer<SettingsBrowserDialog> parent(new SettingsBrowserDialog);
    parent->setAttribute(Qt::WA_DeleteOnClose);
    parent->show();

    QPushButton* add = nullptr;
    for (QPushButton* button : parent->findChildren<QPushButton*>()) {
        if (button->accessibleName() == QStringLiteral("Add settings key")) {
            add = button;
            break;
        }
    }
    expect(add != nullptr, "the persistent browser exposes Add Key");
    if (add == nullptr) {
        parent->close();
        settle();
        return;
    }

    QPointer<QWidget> child;
    bool closeIssued = false;
    bool timedOut = false;
    QTimer closeParent;
    closeParent.setSingleShot(true);
    QObject::connect(&closeParent, &QTimer::timeout, &closeParent, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr || !parent) {
            return;
        }
        child = modal;
        closeIssued = true;
        parent->close();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        timedOut = true;
        if (QWidget* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
        if (parent) {
            parent->close();
        }
    });
    closeParent.start(0);
    watchdog.start(1500);
    QTest::mouseClick(add, Qt::LeftButton);
    watchdog.stop();
    settle();

    expect(!timedOut, "Add Key parent-close scenario returns before its watchdog");
    expect(closeIssued, "Add Key was open when its persistent parent closed");
    expect(parent.isNull() && child.isNull(),
           "closing the WA_DeleteOnClose browser deletes both browser and Add Key child");
}

void testViewerNestedDialogSurvivesPersistentParentClose()
{
    QPointer<SettingsBrowserDialog> parent(new SettingsBrowserDialog);
    parent->setAttribute(Qt::WA_DeleteOnClose);
    parent->resize(900, 600);
    parent->show();

    QTreeWidget* tree = treeOf(*parent);
    QTableWidget* table = tableOf(*parent);
    if (tree == nullptr || table == nullptr
        || !selectScope(tree, QStringLiteral("AA:BB:CC:DD:EE:01"))) {
        expect(false, "persistent browser exposes the seeded viewer row");
        parent->close();
        settle();
        return;
    }
    QApplication::processEvents();
    const int row = rowForKey(table, QStringLiteral("CleanDoc"));
    if (row < 0) {
        expect(false, "persistent browser lists CleanDoc for viewer teardown");
        parent->close();
        settle();
        return;
    }

    QPointer<QWidget> child;
    bool closeIssued = false;
    bool timedOut = false;
    QTimer closeParent;
    QObject::connect(&closeParent, &QTimer::timeout, &closeParent, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr || !parent) {
            return;
        }
        child = modal;
        closeIssued = true;
        closeParent.stop();
        parent->close();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        timedOut = true;
        if (QWidget* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
        if (parent) {
            parent->close();
        }
    });
    closeParent.start(10);
    watchdog.start(1500);
    sendDoubleClick(table, row, 2);
    for (int i = 0; i < 160 && !closeIssued && !timedOut; ++i) {
        QTest::qWait(10);
    }
    closeParent.stop();
    watchdog.stop();
    settle();

    expect(!timedOut, "viewer parent-close scenario returns before its watchdog");
    expect(closeIssued, "document viewer was open when its persistent parent closed");
    expect(parent.isNull() && child.isNull(),
           "closing the WA_DeleteOnClose browser deletes both browser and viewer child");
}

void testStaticWarningSurvivesParentDeletion()
{
    QPointer<QWidget> parent(new QWidget);
    parent->setAttribute(Qt::WA_DeleteOnClose);
    parent->show();

    QPointer<QWidget> warning;
    bool closeIssued = false;
    bool timedOut = false;
    QTimer closeParent;
    closeParent.setSingleShot(true);
    QObject::connect(&closeParent, &QTimer::timeout, &closeParent, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr || !parent) {
            return;
        }
        warning = modal;
        closeIssued = true;
        parent->close();
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        timedOut = true;
        if (QWidget* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
        if (parent) {
            parent->close();
        }
    });
    closeParent.start(0);
    watchdog.start(1500);
    // Ask for a non-default button set with an explicit default, so a stale
    // "the operator pressed Yes" cannot masquerade as the cancellation the
    // callers rely on: SettingsBrowserDialog::addKey()/deleteSelected() only
    // write to the store when this returns Yes.
    const QMessageBox::StandardButton result = FramelessMessageBox::warning(
        parent, QStringLiteral("Lifetime test"),
        QStringLiteral("Parent will close during this modal warning."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    watchdog.stop();
    settle();

    expect(!timedOut, "static warning parent-close scenario returns before its watchdog");
    expect(closeIssued, "static warning was open when its parent closed");
    expect(parent.isNull() && warning.isNull(),
           "closing the warning parent deletes both parent and static warning child");
    expect(result == QMessageBox::NoButton,
           "a warning whose parent dies mid-loop reports cancellation, never a button");
}

// ── Canon (#7055) ────────────────────────────────────────────────────────

QPushButton* buttonByAccessibleName(QWidget* w, const QString& name)
{
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->accessibleName() == name) return b;
    return nullptr;
}

// Answers the first visible top-level of type T for which `match` holds, with
// `answer`, once it is up; reports its default button. A safety timer closes
// any modal left open so a miss fails a check rather than hanging.
template <class T>
struct Driver {
    std::function<bool(T*)> match;
    std::function<void(T*)> answer;
    QString seenDefault;
    bool seen{false};
    QTimer timer, safety;
    Driver(std::function<bool(T*)> m, std::function<void(T*)> a) : match(std::move(m)), answer(std::move(a))
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            if (seen) return;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                auto* t = qobject_cast<T*>(w);
                if (!t || !t->isVisible() || !match(t)) continue;
                seen = true;
                for (QPushButton* b : t->template findChildren<QPushButton*>())
                    if (b->isDefault()) seenDefault = b->text();
                answer(t);
                return;
            }
        });
        timer.start(20);
        safety.setSingleShot(true);
        QObject::connect(&safety, &QTimer::timeout, [] {
            while (QWidget* modal = QApplication::activeModalWidget()) modal->close();
        });
        safety.start(8000);
    }
};

template <class T>
std::function<void(T*)> clickText(const QString& text)
{
    return [text](T* w) {
        for (QPushButton* b : w->template findChildren<QPushButton*>())
            if (b->text() == text) { b->click(); return; }
        w->reject();
    };
}

template <class T>
std::function<void(T*)> pressKey(Qt::Key key)
{
    return [key](T* w) { QTest::keyClick(w->focusWidget() ? w->focusWidget() : w, key); };
}

void testCanonWindow()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        expect(tm.setActiveTheme(QLatin1String(theme)), theme);
        SettingsBrowserDialog d;
        expect(qobject_cast<CanonWindow*>(&d) != nullptr, "Settings Browser is a CanonWindow");
        expect(d.windowType() == Qt::Window && d.windowFlags().testFlag(Qt::FramelessWindowHint),
               "it is a frameless workspace window");
        expect(d.objectName() == "SettingsBrowserDialog", "it keeps the name screens.json waits for");
        expect(d.property("themeContainer").toString() == "canon/settingsBrowser",
               "it uses the canon/settingsBrowser scope");
        auto* header = d.findChild<QLabel*>("canonHeaderTitle");
        expect(header && header->text() == "Settings Browser", "the canon header carries the title");
        bool closeButton = false;
        for (QPushButton* b : d.findChildren<QPushButton*>())
            closeButton = closeButton || b->text() == "Close";
        expect(!closeButton, "the footer Close is gone");
        const QString sheet = d.bodyWidget()->styleSheet();
        expect(!sheet.contains(": ;") && sheet.contains("QMenu") && sheet.contains("selection-color"),
               "the sheet resolves, with menu and selection rules");
        bool literal = false;
        for (QWidget* w : d.findChildren<QWidget*>())
            for (const char* hex : {"#0a0a18", "#0070c0", "#9bd1ff", "#ff8f8f", "#ffc978", "#241c10"})
                literal = literal || w->styleSheet().contains(QLatin1String(hex), Qt::CaseInsensitive);
        expect(!literal, "the old hard-coded colours are gone");
    }
    tm.setActiveTheme(before);
}

// Delete asks a canon question with Cancel the default (QMessageBox's Yes / No
// defaulted to Yes, so Return deleted). Cancel, Return, Esc and closing it
// delete nothing; Delete deletes.
void testDeleteDefaultsToCancel()
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("CanonDeleteMe"), QStringLiteral("1"));
    s.save();
    SettingsBrowserDialog d;
    d.resize(900, 600);
    d.show();
    QTest::qWaitForWindowExposed(&d);
    QTreeWidget* tree = treeOf(d);
    QTableWidget* table = tableOf(d);
    QPushButton* del = buttonByAccessibleName(&d, QStringLiteral("Delete selected setting"));
    if (!tree || !table || !del || !selectScope(tree, QStringLiteral("App Settings"))) {
        expect(false, "the browser shows App Settings and Delete");
        return;
    }
    settle();
    const auto run = [&](std::function<void(CanonMessageBox*)> answer) {
        const int row = rowForKey(table, QStringLiteral("CanonDeleteMe"));
        if (row < 0) return QString();
        table->selectRow(row);
        Driver<CanonMessageBox> driver([](CanonMessageBox* b) { return b->windowTitle() == "Delete Setting?"; },
                                       answer);
        del->click();
        settle();
        return driver.seen ? driver.seenDefault : QStringLiteral("<no question>");
    };
    expect(run(clickText<CanonMessageBox>("Cancel")) == "Cancel", "Delete asks a canon question, Cancel the default");
    expect(s.contains(QStringLiteral("CanonDeleteMe")), "Cancel deletes nothing");
    expect(run(pressKey<CanonMessageBox>(Qt::Key_Return)) == "Cancel"
               && s.contains(QStringLiteral("CanonDeleteMe")),
           "Return on the question deletes nothing");
    expect(run(pressKey<CanonMessageBox>(Qt::Key_Escape)) == "Cancel"
               && s.contains(QStringLiteral("CanonDeleteMe")),
           "Esc on the question deletes nothing");
    expect(run([](CanonMessageBox* b) { b->close(); }) == "Cancel"
               && s.contains(QStringLiteral("CanonDeleteMe")),
           "closing the question deletes nothing");
    run(clickText<CanonMessageBox>("Delete"));
    expect(!s.contains(QStringLiteral("CanonDeleteMe")), "Delete deletes the setting");
    d.hide();
}

// Add Key is a canon dialog with OK the default; a new raw key then asks a
// canon question with Cancel the default, and only Create writes it.
void testAddKeyAndCreateRawKey()
{
    auto& s = AppSettings::instance();
    s.remove(QStringLiteral("CanonNewKey"));
    s.save();
    SettingsBrowserDialog d;
    d.resize(900, 600);
    d.show();
    QTest::qWaitForWindowExposed(&d);
    QTreeWidget* tree = treeOf(d);
    QPushButton* add = buttonByAccessibleName(&d, QStringLiteral("Add settings key"));
    if (!tree || !add || !selectScope(tree, QStringLiteral("App Settings"))) {
        expect(false, "the browser shows App Settings and Add Key");
        return;
    }
    settle();
    const auto run = [&](std::function<void(CanonMessageBox*)> answer) {
        bool canon = false;
        QString addDefault;
        Driver<CanonWindow> addKey([](CanonWindow* w) { return w->objectName() == "settingsAddKeyDialog"; },
            [&](CanonWindow* w) {
                canon = w->findChild<QLabel*>("canonHeaderTitle") != nullptr;
                for (QPushButton* b : w->findChildren<QPushButton*>()) if (b->isDefault()) addDefault = b->text();
                const auto edits = w->findChildren<QLineEdit*>();
                for (QLineEdit* e : edits) {
                    if (e->accessibleName() == "New key name") e->setText("CanonNewKey");
                    if (e->accessibleName() == "New key value") {
                        e->setText("7");
                        e->setFocus();
                        QTest::keyClick(e, Qt::Key_Return);   // OK is the default
                    }
                }
            });
        Driver<CanonMessageBox> question([](CanonMessageBox* b) { return b->windowTitle() == "Create Raw Key?"; },
                                         answer);
        add->click();
        settle();
        return std::make_tuple(canon, addDefault, question.seen, question.seenDefault);
    };
    auto [canon, addDefault, asked, questionDefault] = run(clickText<CanonMessageBox>("Cancel"));
    expect(canon && addDefault == "OK", "Add Key is a canon dialog with OK the default");
    expect(asked && questionDefault == "Cancel", "a new raw key asks first, with Cancel the default");
    expect(!s.contains(QStringLiteral("CanonNewKey")), "Cancel writes nothing");
    const bool askedOnReturn = std::get<2>(run(pressKey<CanonMessageBox>(Qt::Key_Return)));
    expect(askedOnReturn && !s.contains(QStringLiteral("CanonNewKey")),
           "Return on the question writes nothing");
    run(clickText<CanonMessageBox>("Create"));
    expect(s.value(QStringLiteral("CanonNewKey")).toString() == "7", "Create writes the key");
    s.remove(QStringLiteral("CanonNewKey"));
    s.save();
    d.hide();
}

// The browser deletes itself on close, and closing it while Delete Setting? or
// Create Raw Key? is up destroys the question with it. confirmCanon() then
// answers no without touching the dead box, so neither path writes the store.
void testQuestionsSurviveBrowserDeletion()
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("CanonDeleteMe"), QStringLiteral("1"));
    s.remove(QStringLiteral("CanonNewKey"));
    s.save();

    // Opens the browser on App Settings, starts `ask` and closes the browser
    // from inside the question titled `title`.
    const auto closeDuring = [&](const QString& title, const QString& buttonName,
                                 const std::function<void(SettingsBrowserDialog*)>& select,
                                 std::function<void(CanonWindow*)> addKeyAnswer) {
        QPointer<SettingsBrowserDialog> browser(new SettingsBrowserDialog);
        browser->setAttribute(Qt::WA_DeleteOnClose);
        browser->resize(900, 600);
        browser->show();
        QTest::qWaitForWindowExposed(browser);
        QTreeWidget* tree = treeOf(*browser);
        QPushButton* button = buttonByAccessibleName(browser, buttonName);
        if (!tree || !button || !selectScope(tree, QStringLiteral("App Settings"))) {
            browser->close();
            settle();
            return std::make_tuple(false, false);
        }
        settle();
        select(browser);
        QPointer<QWidget> question;
        Driver<CanonWindow> addKey([](CanonWindow* w) { return w->objectName() == "settingsAddKeyDialog"; },
                                   std::move(addKeyAnswer));
        Driver<CanonMessageBox> driver([title](CanonMessageBox* b) { return b->windowTitle() == title; },
                                       [&](CanonMessageBox* b) {
                                           question = b;
                                           if (browser) browser->close();
                                       });
        button->click();
        settle();
        return std::make_tuple(driver.seen, browser.isNull() && question.isNull());
    };

    const auto [deleteAsked, deleteGone] = closeDuring(
        QStringLiteral("Delete Setting?"), QStringLiteral("Delete selected setting"),
        [](SettingsBrowserDialog* b) {
            QTableWidget* table = tableOf(*b);
            const int row = rowForKey(table, QStringLiteral("CanonDeleteMe"));
            if (row >= 0) table->selectRow(row);
        },
        [](CanonWindow* w) { w->close(); });
    expect(deleteAsked && deleteGone,
           "closing the browser during Delete Setting? deletes the browser and the question");
    expect(s.contains(QStringLiteral("CanonDeleteMe")),
           "a Delete Setting? whose browser dies mid-question deletes nothing");

    const auto [createAsked, createGone] = closeDuring(
        QStringLiteral("Create Raw Key?"), QStringLiteral("Add settings key"),
        [](SettingsBrowserDialog*) {},
        [](CanonWindow* w) {
            for (QLineEdit* e : w->findChildren<QLineEdit*>()) {
                if (e->accessibleName() == "New key name") e->setText("CanonNewKey");
                if (e->accessibleName() == "New key value") {
                    e->setText("7");
                    e->setFocus();
                    QTest::keyClick(e, Qt::Key_Return);
                }
            }
        });
    expect(createAsked && createGone,
           "closing the browser during Create Raw Key? deletes the browser and the question");
    expect(!s.contains(QStringLiteral("CanonNewKey")),
           "a Create Raw Key? whose browser dies mid-question writes nothing");

    s.remove(QStringLiteral("CanonDeleteMe"));
    s.save();
}

void testClosePaths()
{
    for (int path = 0; path < 3; ++path) {
        QPointer<SettingsBrowserDialog> d = new SettingsBrowserDialog;
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
        const bool exposed = QTest::qWaitForWindowExposed(d);
        if (path == 0) QTest::keyClick(d, Qt::Key_Escape);
        if (path == 1) {
            d->activateWindow();
            QTest::qWaitForWindowActive(d);
            QTest::keySequence(d, QKeySequence::Close);
        }
        if (path == 2) {
            QToolButton* corner = nullptr;
            for (QToolButton* b : d->findChildren<QToolButton*>())
                if (b->accessibleName() == QLatin1String("Close")) corner = b;
            expect(corner != nullptr, "the corner close button exists");
            if (corner) QTest::mouseClick(corner, Qt::LeftButton);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        expect(exposed && d.isNull(),
               path == 0 ? "Esc destroys the window"
               : path == 1 ? "Ctrl+W / Cmd+W destroys the window"
                           : "the corner close button destroys the window");
        delete d.data();
    }
}

void testLegibleAtMinimum()
{
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        tm.setActiveTheme(QLatin1String(theme));
        SettingsBrowserDialog d;
        d.show();
        QTest::qWaitForWindowExposed(&d);
        d.resize(QSize(1, 1));
        QTest::qWait(50);
        bool legible = true;
        for (QWidget* w : d.bodyWidget()->findChildren<QWidget*>()) {
            if (!w->isVisible()) continue;
            bool scrolled = false;
            for (QWidget* p = w->parentWidget(); p && p != &d; p = p->parentWidget())
                scrolled = scrolled || qobject_cast<QAbstractScrollArea*>(p) != nullptr;
            if (scrolled || qobject_cast<QAbstractScrollArea*>(w)) continue;
            int need = w->minimumSizeHint().height();
            if (auto* l = qobject_cast<QLabel*>(w); l && l->wordWrap()) need = l->heightForWidth(l->width());
            if (w->height() < need) {
                legible = false;
                std::printf("  %s: %s h=%d need=%d\n", theme, w->metaObject()->className(), w->height(), need);
            }
        }
        std::printf("  %s: shown minimum %dx%d\n", theme, d.width(), d.height());
        expect(legible, "every control keeps its height at the smallest size");
        expect(d.height() <= 700, "the shown minimum height fits a 768 px screen");
        d.hide();
    }
    tm.setActiveTheme(before);
}

void testGeometryCarriesOver()
{
    static const QString kKey = QStringLiteral("SettingsBrowserDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    SettingsBrowserDialog restored;
    const QSize chosen(restored.minimumWidth() + 20, restored.minimumHeight() + 30);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    restored.show();
    QTest::qWaitForWindowExposed(&restored);
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    expect(restored.size() == chosen, "geometry saved by the old dialog restores under its key");
    restored.hide();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-settings-browser-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "[FAIL] could not create settings profile\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    seedStore();

    // The unparseable row cannot go in through the typed API (it only
    // accepts a QJsonObject), which is exactly why the viewer has to judge
    // the stored bytes: such a row can only ARRIVE from a damaged store, a
    // partial write, or another build. Plant it the same way — straight
    // into the file on a second connection (WAL allows it).
    {
        SettingsDatabase raw;
        if (raw.open(SettingsPaths::databasePath())) {
            raw.upsertRadioFeature(QStringLiteral("hl2"),
                                   QStringLiteral("AA:BB:CC:DD:EE:01"),
                                   QStringLiteral("CorruptDoc"), 1,
                                   QStringLiteral("{\"frequencyHz\":710000"));
            // Valid JSON, wrong shape: parses, but is not an object.
            raw.upsertRadioFeature(QStringLiteral("hl2"),
                                   QStringLiteral("AA:BB:CC:DD:EE:01"),
                                   QStringLiteral("ArrayDoc"), 4,
                                   QStringLiteral("[{\"slot\":1}]"));
            raw.close();
        }
    }

    testMaskedRowsAreReadOnly();
    testEditableRowShowsStoredBytes();
    testDoubleClickOpensOneViewer();
    testCorruptDocumentIsNotEditable();
    testArrayDocumentIsTreatedAsCorrupt();
    testAddKeyNestedDialogSurvivesPersistentParentClose();
    testViewerNestedDialogSurvivesPersistentParentClose();
    testStaticWarningSurvivesParentDeletion();
    testCanonWindow();
    testDeleteDefaultsToCancel();
    testAddKeyAndCreateRawKey();
    testQuestionsSurviveBrowserDeletion();
    testClosePaths();
    testLegibleAtMinimum();
    testGeometryCarriesOver();

    std::printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
