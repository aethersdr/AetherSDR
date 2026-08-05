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
#include "gui/SettingsBrowserDialog.h"

#include <QApplication>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QPlainTextEdit>
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
    QTest::qWaitForWindowExposed(&dlg);

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
    QTest::qWaitForWindowExposed(&dlg);
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
    QTest::qWaitForWindowExposed(&dlg);
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

    std::printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
