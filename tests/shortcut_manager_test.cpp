#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/SettingsDatabase.h"
#include "core/ShortcutManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

void registerCwActions(ShortcutManager& manager)
{
    manager.registerAction(QStringLiteral("cwkey"), QStringLiteral("Trigger straight key"),
                           QStringLiteral("CW"), QKeySequence(), {});
    manager.registerAction(QStringLiteral("cwdit"), QStringLiteral("Trigger CW Left Paddle"),
                           QStringLiteral("CW"), QKeySequence(), {});
    manager.registerAction(QStringLiteral("cwdah"), QStringLiteral("Trigger CW Right Paddle"),
                           QStringLiteral("CW"), QKeySequence(), {});
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-shortcut-manager-test"));
    if (!settingsProfile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);

    auto& settings = AppSettings::instance();
    settings.load();

    ShortcutManager manager;
    registerCwActions(manager);
    manager.loadBindings();
    manager.setBinding(QStringLiteral("cwkey"), QKeySequence(Qt::Key_F9));
    manager.setBinding(QStringLiteral("cwdit"), QKeySequence(Qt::Key_F10));
    manager.setBinding(QStringLiteral("cwdah"), QKeySequence(Qt::Key_F11));

    // Assert against the persisted database file (not the in-memory cache):
    // the sanitized ids must be the stored rows, and the dotted CW ids must
    // not exist — that sanitization now lives in ShortcutManager, since the
    // SQLite store no longer rejects exotic key characters the way the XML
    // element-name rule did.
    bool ok = expect(QFile::exists(settings.filePath()),
                     "shortcut settings store is written");
    const auto storedRow = [&settings](const char* key, QString& value) {
        return SettingsDatabase::readAppValueFromFile(
            settings.filePath(), QString::fromLatin1(key), value);
    };
    QString value;
    ok &= expect(storedRow("Shortcut_cwkey", value) && value == QStringLiteral("F9"),
                 "straight-key shortcut persists as Shortcut_cwkey");
    ok &= expect(storedRow("Shortcut_cwdit", value) && value == QStringLiteral("F10"),
                 "dit shortcut persists as Shortcut_cwdit");
    ok &= expect(storedRow("Shortcut_cwdah", value) && value == QStringLiteral("F11"),
                 "dah shortcut persists as Shortcut_cwdah");
    ok &= expect(!storedRow("Shortcut_cw.key", value),
                 "shortcut store does not use dotted CW IDs");
    ok &= expect(!storedRow("Shortcut_cw.dit", value),
                 "shortcut store does not use dotted CW dit ID");
    ok &= expect(!storedRow("Shortcut_cw.dah", value),
                 "shortcut store does not use dotted CW dah ID");

    settings.reset();
    settings.load();

    ShortcutManager restored;
    registerCwActions(restored);
    restored.loadBindings();

    const auto* straight = restored.action(QStringLiteral("cwkey"));
    const auto* dit = restored.action(QStringLiteral("cwdit"));
    const auto* dah = restored.action(QStringLiteral("cwdah"));

    ok &= expect(straight && straight->currentKey == QKeySequence(Qt::Key_F9),
                 "straight-key shortcut reloads");
    ok &= expect(dit && dit->currentKey == QKeySequence(Qt::Key_F10),
                 "dit shortcut reloads");
    ok &= expect(dah && dah->currentKey == QKeySequence(Qt::Key_F11),
                 "dah shortcut reloads");

    // Regression #3964: a binding cleared by the user must stay empty after
    // restart — not revert to the action's non-empty default key.
    // The old loadBindings() used !val.isNull(); QXmlStreamReader::readElementText()
    // returns a null QString for empty/self-closing elements, so the persisted ""
    // was treated as "never written" and the default was silently restored.
    {
        ShortcutManager mgr;
        // Register with a non-empty default so we can tell if it wrongly reverts.
        mgr.registerAction(QStringLiteral("mox_toggle"), QStringLiteral("MOX Toggle"),
                           QStringLiteral("TX"), QKeySequence(Qt::Key_Space), {});
        mgr.loadBindings();               // no entry in settings yet → keeps Space
        const auto* fresh = mgr.action(QStringLiteral("mox_toggle"));
        ok &= expect(fresh && fresh->currentKey == QKeySequence(Qt::Key_Space),
                     "absent key keeps non-empty default");
        mgr.clearBinding(QStringLiteral("mox_toggle"));   // saves Shortcut_mox_toggle = ""

        settings.reset();
        settings.load();

        ShortcutManager mgr2;
        mgr2.registerAction(QStringLiteral("mox_toggle"), QStringLiteral("MOX Toggle"),
                            QStringLiteral("TX"), QKeySequence(Qt::Key_Space), {});
        mgr2.loadBindings();

        const auto* mox = mgr2.action(QStringLiteral("mox_toggle"));
        ok &= expect(mox && mox->currentKey.isEmpty(),
                     "cleared binding stays empty after reload (not reverted to default Space)");
    }

    // Regression (#3983 review): saveBindings() must persist only customizations
    // so a future release's changed default is delivered. An action left at — or
    // reset to — its default is written absent; only user-changed keys stick.
    {
        settings.reset();
        settings.load();

        ShortcutManager mgr;
        mgr.registerAction(QStringLiteral("band_up"), QStringLiteral("Band Up"),
                           QStringLiteral("Band"), QKeySequence(), {});
        mgr.registerAction(QStringLiteral("rit_toggle"), QStringLiteral("RIT Toggle"),
                           QStringLiteral("RIT/XIT"), QKeySequence(Qt::Key_R), {});
        mgr.loadBindings();
        // Customize one action so a save runs; leave rit_toggle at its default.
        mgr.setBinding(QStringLiteral("band_up"), QKeySequence(Qt::Key_PageUp));

        settings.reset();
        settings.load();

        ok &= expect(!settings.contains(QStringLiteral("Shortcut_rit_toggle")),
                     "action left at its default is not persisted");

        // A later release changes rit_toggle's default — it must be delivered,
        // not pinned to this release's Key_R.
        ShortcutManager future;
        future.registerAction(QStringLiteral("band_up"), QStringLiteral("Band Up"),
                              QStringLiteral("Band"), QKeySequence(), {});
        future.registerAction(QStringLiteral("rit_toggle"), QStringLiteral("RIT Toggle"),
                              QStringLiteral("RIT/XIT"), QKeySequence(Qt::Key_G), {});
        future.loadBindings();
        const auto* rit = future.action(QStringLiteral("rit_toggle"));
        ok &= expect(rit && rit->currentKey == QKeySequence(Qt::Key_G),
                     "changed default delivered to an unpersisted action");
        const auto* bu = future.action(QStringLiteral("band_up"));
        ok &= expect(bu && bu->currentKey == QKeySequence(Qt::Key_PageUp),
                     "persisted customization survives reload");
    }

    // Regression (#3983 review, consequence 2): the load-time duplicate-key
    // normalization clears the losing action in memory but must NOT persist that
    // machine decision. Two actions share a non-empty default; once a future
    // release resolves the collision, the previously-cleared action must receive
    // its new (non-colliding) default rather than staying pinned to "".
    {
        settings.reset();
        settings.load();

        ShortcutManager mgr;
        mgr.registerAction(QStringLiteral("alpha"), QStringLiteral("Alpha"),
                           QStringLiteral("TX"), QKeySequence(Qt::Key_F5), {});
        mgr.registerAction(QStringLiteral("beta"), QStringLiteral("Beta"),
                           QStringLiteral("TX"), QKeySequence(Qt::Key_F5), {});
        mgr.loadBindings();   // normalization clears one of the two in memory
        // Force a save so any (wrongly) persisted clear would land on disk.
        mgr.setBinding(QStringLiteral("alpha"), QKeySequence(Qt::Key_F6));

        settings.reset();
        settings.load();

        ok &= expect(!settings.contains(QStringLiteral("Shortcut_beta")),
                     "normalization clear is not persisted");

        // Future release moves beta's default off the collision.
        ShortcutManager future;
        future.registerAction(QStringLiteral("alpha"), QStringLiteral("Alpha"),
                              QStringLiteral("TX"), QKeySequence(Qt::Key_F5), {});
        future.registerAction(QStringLiteral("beta"), QStringLiteral("Beta"),
                              QStringLiteral("TX"), QKeySequence(Qt::Key_F7), {});
        future.loadBindings();
        const auto* beta = future.action(QStringLiteral("beta"));
        ok &= expect(beta && beta->currentKey == QKeySequence(Qt::Key_F7),
                     "resolved-collision default delivered (normalization not pinned)");
    }

    // Portable CSV backup: explicit overrides round-trip, rows left at default
    // adopt the importing release's local default, and unknown future actions
    // are reported without blocking the known subset.
    {
        settings.reset();

        ShortcutManager source;
        source.registerAction(QStringLiteral("alpha"), QStringLiteral("Alpha Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F1), {});
        source.registerAction(QStringLiteral("beta"), QStringLiteral("Beta Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F2), {});
        source.registerAction(QStringLiteral("defaulted"), QStringLiteral("Default Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F4), {});
        source.registerAction(QStringLiteral("future_action"),
                              QStringLiteral("Future, \"Capability\""),
                              QStringLiteral("Display"), QKeySequence(Qt::Key_F8), {});
        source.loadBindings();
        source.setBinding(QStringLiteral("alpha"),
                          QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
        source.clearBinding(QStringLiteral("beta"));

        const QByteArray csv = source.exportBindingsCsv();
        ok &= expect(csv.startsWith("FORMAT_VERSION,ACTION_ID,ACTION_NAME,CATEGORY,SHORTCUT,CUSTOMIZED\r\n"),
                     "shortcut CSV has a named additive schema");
        ok &= expect(csv.contains("Ctrl+Alt+A"),
                     "shortcut CSV uses QKeySequence portable text");
        ok &= expect(csv.contains("\"Future, \"\"Capability\"\"\""),
                     "shortcut CSV quotes human-readable action names");

        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("alpha"), QStringLiteral("Alpha Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F3), {});
        target.registerAction(QStringLiteral("beta"), QStringLiteral("Beta Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F2), {});
        target.registerAction(QStringLiteral("defaulted"), QStringLiteral("Default Action"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F5), {});
        target.loadBindings();

        const ShortcutImportResult imported = target.importBindingsCsv(csv);
        ok &= expect(imported.ok(), "portable shortcut CSV imports successfully");
        ok &= expect(imported.importedCount == 3,
                     "known shortcut actions import as one batch");
        ok &= expect(imported.unknownActions.size() == 1
                         && imported.unknownActions.first().contains(QStringLiteral("future_action")),
                     "future action is reported and skipped on an older release");
        ok &= expect(target.action(QStringLiteral("alpha"))->currentKey
                         == QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A),
                     "customized shortcut is restored");
        ok &= expect(target.action(QStringLiteral("beta"))->currentKey.isEmpty(),
                     "explicitly cleared shortcut is restored");
        ok &= expect(target.action(QStringLiteral("defaulted"))->currentKey
                         == QKeySequence(Qt::Key_F5),
                     "non-customized row uses importing release default");

        const QKeySequence beforeBadImport = target.action(QStringLiteral("alpha"))->currentKey;
        QByteArray malformed = csv;
        malformed.replace("Alpha Action", "\"Alpha Action");
        const ShortcutImportResult rejected = target.importBindingsCsv(malformed);
        ok &= expect(!rejected.ok(), "malformed shortcut CSV is rejected");
        ok &= expect(target.action(QStringLiteral("alpha"))->currentKey == beforeBadImport,
                     "rejected import leaves every binding unchanged");
    }

    // A custom binding may intentionally take another action's default. The
    // CSV contains both rows, so batch import must normalize after applying all
    // rows rather than letting the later default row steal the key back.
    {
        settings.reset();
        ShortcutManager source;
        source.registerAction(QStringLiteral("custom_first"), QStringLiteral("Custom First"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        source.registerAction(QStringLiteral("default_second"), QStringLiteral("Default Second"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F5), {});
        source.setBinding(QStringLiteral("custom_first"), QKeySequence(Qt::Key_F5));

        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("custom_first"), QStringLiteral("Custom First"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        target.registerAction(QStringLiteral("default_second"), QStringLiteral("Default Second"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F5), {});
        const ShortcutImportResult imported =
            target.importBindingsCsv(source.exportBindingsCsv());
        ok &= expect(imported.ok()
                         && target.action(QStringLiteral("custom_first"))->currentKey
                                == QKeySequence(Qt::Key_F5),
                     "custom import wins over a later action's colliding default");
        ok &= expect(target.action(QStringLiteral("default_second"))->currentKey.isEmpty(),
                     "colliding default remains normalized after import");
    }

    // Exact action-name fallback keeps a backup usable if a stable internal id
    // is renamed; ambiguous names intentionally do not guess.
    {
        settings.reset();
        ShortcutManager oldRelease;
        oldRelease.registerAction(QStringLiteral("old_id"), QStringLiteral("Renamed Action"),
                                  QStringLiteral("Display"), QKeySequence(), {});
        oldRelease.setBinding(QStringLiteral("old_id"), QKeySequence(Qt::Key_F9));

        ShortcutManager newRelease;
        newRelease.registerAction(QStringLiteral("new_id"), QStringLiteral("Renamed Action"),
                                  QStringLiteral("Display"), QKeySequence(), {});
        const ShortcutImportResult renamed =
            newRelease.importBindingsCsv(oldRelease.exportBindingsCsv());
        ok &= expect(renamed.ok() && renamed.importedCount == 1,
                     "human-readable action name bridges an internal id rename");
        ok &= expect(newRelease.action(QStringLiteral("new_id"))->currentKey
                         == QKeySequence(Qt::Key_F9),
                     "name-fallback import applies the shortcut");
    }

    // File transfer uses QSaveFile (atomic replacement) and the same parser as
    // the visible Import/Export buttons and automation bridge.
    {
        settings.reset();
        ShortcutManager source;
        source.registerAction(QStringLiteral("file_action"), QStringLiteral("File Action"),
                              QStringLiteral("Display"), QKeySequence(Qt::Key_F6), {});
        source.setBinding(QStringLiteral("file_action"), QKeySequence(Qt::Key_F10));
        const QString path = QDir(settingsProfile.path()).filePath(QStringLiteral("shortcuts.csv"));
        const ShortcutExportResult exported = source.exportToFile(path);
        ok &= expect(exported.ok() && exported.exportedCount == 1,
                     "shortcut file export commits atomically");

        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("file_action"), QStringLiteral("File Action"),
                              QStringLiteral("Display"), QKeySequence(Qt::Key_F6), {});
        const ShortcutImportResult imported = target.importFromFile(path);
        ok &= expect(imported.ok() && target.action(QStringLiteral("file_action"))->currentKey
                                            == QKeySequence(Qt::Key_F10),
                     "shortcut file import uses production parser and applies binding");
    }

    // Regression: a customized import row that displaces a locally-persisted
    // customization on the same key must (a) clear persisted along with
    // currentKey — otherwise saveBindings writes "" and loadBindings pins the
    // displaced action to empty forever (#3964); and (b) surface the local
    // action in result.displacedActions so the UI can warn the user.
    {
        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("local_only"), QStringLiteral("Local Only"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        target.registerAction(QStringLiteral("imported"), QStringLiteral("Imported"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        target.loadBindings();
        target.setBinding(QStringLiteral("local_only"), QKeySequence(Qt::Key_F5));

        ShortcutManager source;
        source.registerAction(QStringLiteral("imported"), QStringLiteral("Imported"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        source.setBinding(QStringLiteral("imported"), QKeySequence(Qt::Key_F5));

        const ShortcutImportResult imported =
            target.importBindingsCsv(source.exportBindingsCsv());
        ok &= expect(imported.ok(), "cross-clear import succeeds");
        ok &= expect(imported.displacedActions.size() == 1
                         && imported.displacedActions.first().contains(QStringLiteral("local_only")),
                     "displaced local binding is surfaced to the caller");

        // Round-trip through save/load: the displaced action must NOT come
        // back as a pinned empty (#3964 failure mode).
        settings.reset();
        settings.load();
        ShortcutManager reloaded;
        reloaded.registerAction(QStringLiteral("local_only"), QStringLiteral("Local Only"),
                                QStringLiteral("DSP"), QKeySequence(Qt::Key_F9), {});
        reloaded.registerAction(QStringLiteral("imported"), QStringLiteral("Imported"),
                                QStringLiteral("DSP"), QKeySequence(), {});
        reloaded.loadBindings();
        ok &= expect(reloaded.action(QStringLiteral("local_only"))->currentKey
                         == QKeySequence(Qt::Key_F9),
                     "displaced local action gets its release default (not pinned empty)");
    }

    // Regression (#3964 via import): a CUSTOMIZED=False row from a machine
    // that never touched action A must not resurrect A's default when the
    // local user had explicitly cleared A. The explicit clear is preserved.
    {
        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("keep_cleared"), QStringLiteral("Keep Cleared"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F4), {});
        target.loadBindings();
        target.clearBinding(QStringLiteral("keep_cleared"));

        ShortcutManager source;
        source.registerAction(QStringLiteral("keep_cleared"), QStringLiteral("Keep Cleared"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F4), {});
        // source did not customize keep_cleared → CSV row is CUSTOMIZED=False.

        const ShortcutImportResult imported =
            target.importBindingsCsv(source.exportBindingsCsv());
        ok &= expect(imported.ok(),
                     "default-adopting row for a locally-cleared action imports without error");
        ok &= expect(target.action(QStringLiteral("keep_cleared"))->currentKey.isEmpty(),
                     "explicit local clear is preserved across a CUSTOMIZED=False import");
    }

    // Zero-import (older release reading a newer backup): every row is unknown,
    // so nothing changes — the tail must not churn AppSettings or emit
    // bindingsChanged for a pure no-op.
    {
        settings.reset();
        ShortcutManager source;
        source.registerAction(QStringLiteral("future_only"), QStringLiteral("Future Only"),
                              QStringLiteral("Display"), QKeySequence(Qt::Key_F1), {});
        source.setBinding(QStringLiteral("future_only"), QKeySequence(Qt::Key_F2));

        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("known"), QStringLiteral("Known"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F6), {});
        target.loadBindings();
        int changeSignals = 0;
        QObject::connect(&target, &ShortcutManager::bindingsChanged,
                         [&changeSignals] { ++changeSignals; });

        const ShortcutImportResult imported =
            target.importBindingsCsv(source.exportBindingsCsv());
        ok &= expect(imported.ok() && imported.importedCount == 0,
                     "no-op import succeeds with zero imported actions");
        ok &= expect(imported.unknownActions.size() == 1,
                     "unknown future action is reported");
        ok &= expect(changeSignals == 0,
                     "no-op import does not emit bindingsChanged");
    }

    // Parser rejects garbage-key names (e.g. XF86AudioNext) instead of
    // silently storing a Key_unknown that never fires as a QShortcut.
    {
        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("badkey"), QStringLiteral("Bad Key"),
                              QStringLiteral("DSP"), QKeySequence(), {});
        target.loadBindings();

        const QByteArray csv =
            "FORMAT_VERSION,ACTION_ID,ACTION_NAME,CATEGORY,SHORTCUT,CUSTOMIZED\r\n"
            "1,badkey,Bad Key,DSP,XF86AudioNext,True\r\n";
        const ShortcutImportResult rejected = target.importBindingsCsv(csv);
        ok &= expect(!rejected.ok(),
                     "unrecognized portable-key name is rejected (Key_unknown guard)");
    }

    // Parser surfaces every field error on a row instead of only the first
    // (independent-if validators, not an else-if chain).
    {
        settings.reset();
        ShortcutManager target;
        target.loadBindings();
        const QByteArray csv =
            "FORMAT_VERSION,ACTION_ID,ACTION_NAME,CATEGORY,SHORTCUT,CUSTOMIZED\r\n"
            "1,bad-id,,DSP,,maybe\r\n";
        const ShortcutImportResult rejected = target.importBindingsCsv(csv);
        ok &= expect(!rejected.ok(), "multi-field-bad row is rejected");
        int badId = 0, blankName = 0, badCustomized = 0;
        for (const QString& msg : rejected.errors) {
            if (msg.contains(QStringLiteral("invalid ACTION_ID"))) ++badId;
            if (msg.contains(QStringLiteral("ACTION_NAME is required"))) ++blankName;
            if (msg.contains(QStringLiteral("CUSTOMIZED"))) ++badCustomized;
        }
        ok &= expect(badId == 1 && blankName == 1 && badCustomized == 1,
                     "every field error on the row is surfaced in one pass");
    }

    // Duplicate-id check must not fire on rows whose id is itself invalid —
    // otherwise misaligned rows produce spurious 'duplicate ""' noise.
    {
        settings.reset();
        ShortcutManager target;
        target.loadBindings();
        const QByteArray csv =
            "FORMAT_VERSION,ACTION_ID,ACTION_NAME,CATEGORY,SHORTCUT,CUSTOMIZED\r\n"
            "1,,A,DSP,,False\r\n"
            "1,,B,DSP,,False\r\n";
        const ShortcutImportResult rejected = target.importBindingsCsv(csv);
        ok &= expect(!rejected.ok(), "invalid-id rows are rejected");
        for (const QString& msg : rejected.errors) {
            ok &= expect(!msg.contains(QStringLiteral("duplicate ACTION_ID")),
                         "duplicate-id check does not fire on invalid ids");
        }
    }

    // Rejected import must leave EVERY binding untouched, not just the first.
    {
        settings.reset();
        ShortcutManager target;
        target.registerAction(QStringLiteral("a1"), QStringLiteral("Alpha One"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F1), {});
        target.registerAction(QStringLiteral("a2"), QStringLiteral("Alpha Two"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F2), {});
        target.registerAction(QStringLiteral("a3"), QStringLiteral("Alpha Three"),
                              QStringLiteral("DSP"), QKeySequence(Qt::Key_F3), {});
        target.loadBindings();
        target.setBinding(QStringLiteral("a1"), QKeySequence(Qt::Key_F11));
        target.clearBinding(QStringLiteral("a2"));

        const QByteArray malformed =
            "FORMAT_VERSION,ACTION_ID,ACTION_NAME,CATEGORY,SHORTCUT,CUSTOMIZED\r\n"
            "1,a1,\"Broken quoted name,DSP,Ctrl+Q,True\r\n";
        const ShortcutImportResult rejected = target.importBindingsCsv(malformed);
        ok &= expect(!rejected.ok(), "malformed CSV is rejected");
        ok &= expect(target.action(QStringLiteral("a1"))->currentKey == QKeySequence(Qt::Key_F11),
                     "a1 unchanged after rejected import");
        ok &= expect(target.action(QStringLiteral("a2"))->currentKey.isEmpty(),
                     "a2 (explicitly cleared) unchanged after rejected import");
        ok &= expect(target.action(QStringLiteral("a3"))->currentKey == QKeySequence(Qt::Key_F3),
                     "a3 (default) unchanged after rejected import");
    }

    // #7354: left and right modifier keys bind separately (iambic paddles on a
    // keyboard-emulating Morse adapter).
    {
        using P = ShortcutNativePlatform;
        // Per-platform native side tables.
        ok &= expect(shortcutSidedKeyFor(P::Windows, Qt::Key_Control, 0x1d, 0x11)
                         == kShortcutKeyLeftCtrl,
                     "Windows scan 0x1D is Left Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::Windows, Qt::Key_Control, 0x11d, 0x11)
                         == kShortcutKeyRightCtrl,
                     "Windows extended scan 0x11D is Right Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::Windows, Qt::Key_Control, 0, 0xa3)
                         == kShortcutKeyRightCtrl,
                     "Windows VK_RCONTROL is Right Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::MacOS, Qt::Key_Meta, 0, 0x3b)
                         == kShortcutKeyLeftCtrl,
                     "macOS kVK_Control is Left Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::MacOS, Qt::Key_Meta, 0, 0x3e)
                         == kShortcutKeyRightCtrl,
                     "macOS kVK_RightControl is Right Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::Xkb, Qt::Key_Control, 37, 0xffe3)
                         == kShortcutKeyLeftCtrl,
                     "xkb Control_L is Left Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::Xkb, Qt::Key_Control, 105, 0xffe4)
                         == kShortcutKeyRightCtrl,
                     "xkb Control_R is Right Ctrl");
        ok &= expect(shortcutSidedKeyFor(P::Windows, Qt::Key_A, 0x1d, 0)
                         == Qt::Key_unknown,
                     "a non-modifier key never resolves to a side");
        ok &= expect(shortcutSidedKeyFor(P::Xkb, Qt::Key_Control, 0, 0) == Qt::Key_unknown,
                     "missing native data resolves no side");

        // A bare Ctrl press carries ControlModifier; it must still resolve bare.
        const QKeyEvent ctrlPress(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        ok &= expect(shortcutSequenceFromKeyEvent(&ctrlPress) == QKeySequence(Qt::Key_Control),
                     "bare Ctrl press resolves to Control, not Ctrl+Control");
        const QKeyEvent ctrlRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        ok &= expect(shortcutSequenceFromKeyEvent(&ctrlRelease) == QKeySequence(Qt::Key_Control),
                     "bare Ctrl release resolves to Control");
        const QKeyEvent squeezeAlt(QEvent::KeyPress, Qt::Key_Alt,
                                   Qt::ControlModifier | Qt::AltModifier);
        ok &= expect(shortcutSequenceFromKeyEvent(&squeezeAlt) == QKeySequence(Qt::Key_Alt),
                     "Alt pressed while Ctrl held resolves bare");
        const QKeyEvent ctrlA(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
        ok &= expect(shortcutSequenceFromKeyEvent(&ctrlA)
                         == QKeySequence(Qt::ControlModifier | Qt::Key_A),
                     "Ctrl+A is unchanged");
        // An injected sided key resolves to itself.
        const QKeyEvent injected(QEvent::KeyPress, kShortcutKeyRightCtrl, Qt::ControlModifier);
        ok &= expect(shortcutSequenceFromKeyEvent(&injected) == QKeySequence(kShortcutKeyRightCtrl),
                     "injected Right Ctrl resolves to Right Ctrl");

        // The live wrapper reads this host's native fields (each CI lane runs
        // its own branch).
#if defined(Q_OS_WIN)
        const QKeyEvent live(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier,
                             0x11d, 0x11, 0);
#elif defined(Q_OS_MACOS)
        const QKeyEvent live(QEvent::KeyPress, Qt::Key_Meta, Qt::MetaModifier, 0, 0x3e, 0);
#else
        const QKeyEvent live(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier,
                             105, 0xffe4, 0);
#endif
        ok &= expect(shortcutSequenceFromKeyEvent(&live) == QKeySequence(kShortcutKeyRightCtrl),
                     "a native right-Ctrl press resolves to Right Ctrl on this platform");

        // Text round-trip in both formats, with and without a modifier prefix.
        for (const auto& sk : kShortcutSidedKeys) {
            const QKeySequence bare(sk.key);
            const QKeySequence shifted(Qt::ShiftModifier | sk.key);
            ok &= expect(shortcutKeyText(bare, QKeySequence::PortableText)
                             == QLatin1String(sk.name)
                         && shortcutKeyFromText(QLatin1String(sk.name),
                                                QKeySequence::PortableText) == bare
                         && shortcutKeyFromText(shortcutKeyText(bare)) == bare
                         && shortcutKeyFromText(shortcutKeyText(shifted)) == shifted
                         && shortcutKeyFromText(
                                shortcutKeyText(shifted, QKeySequence::PortableText),
                                QKeySequence::PortableText) == shifted,
                         sk.name);
        }

        // Sided bindings persist, reload, and do not collide at load.
        settings.reset();
        settings.load();
        ShortcutManager mgr;
        registerCwActions(mgr);
        mgr.loadBindings();
        mgr.setBinding(QStringLiteral("cwdit"), QKeySequence(kShortcutKeyLeftCtrl));
        mgr.setBinding(QStringLiteral("cwdah"), QKeySequence(kShortcutKeyRightCtrl));
        // The platform's generic Ctrl (Qt::Key_Meta on macOS).
        mgr.setBinding(QStringLiteral("cwkey"),
                       QKeySequence(shortcutSidedKeyGeneric(kShortcutKeyLeftCtrl)));

        QString stored;
        ok &= expect(storedRow("Shortcut_cwdit", stored) && stored == QStringLiteral("Left Ctrl"),
                     "Left Ctrl binding persists as \"Left Ctrl\"");

        settings.reset();
        settings.load();
        ShortcutManager reloaded;
        registerCwActions(reloaded);
        reloaded.loadBindings();
        ok &= expect(reloaded.action(QStringLiteral("cwdit"))->currentKey
                         == QKeySequence(kShortcutKeyLeftCtrl),
                     "Left Ctrl binding reloads");
        ok &= expect(reloaded.action(QStringLiteral("cwdah"))->currentKey
                         == QKeySequence(kShortcutKeyRightCtrl),
                     "Right Ctrl binding reloads (not cleared as a duplicate)");

        // Sided binding wins; an unbound side falls back to the generic key.
        ok &= expect(reloaded.actionForKey(QKeySequence(kShortcutKeyLeftCtrl))
                         == reloaded.action(QStringLiteral("cwdit")),
                     "Left Ctrl resolves to the dit paddle");
        ok &= expect(reloaded.actionForKey(QKeySequence(kShortcutKeyRightCtrl))
                         == reloaded.action(QStringLiteral("cwdah")),
                     "Right Ctrl resolves to the dah paddle");
        reloaded.clearBinding(QStringLiteral("cwdah"));
        ok &= expect(reloaded.actionForKey(QKeySequence(kShortcutKeyRightCtrl))
                         == reloaded.action(QStringLiteral("cwkey")),
                     "unbound Right Ctrl falls back to the generic Control binding");
        ok &= expect(reloaded.conflictCheck(QKeySequence(kShortcutKeyRightCtrl)).isEmpty(),
                     "a generic Control binding is not a conflict for a sided key");

        // Press L, press R, release L, release R: each event names its paddle.
        reloaded.setBinding(QStringLiteral("cwdah"), QKeySequence(kShortcutKeyRightCtrl));
        const QKeyEvent pressL(QEvent::KeyPress, kShortcutKeyLeftCtrl, Qt::ControlModifier);
        const QKeyEvent pressR(QEvent::KeyPress, kShortcutKeyRightCtrl, Qt::ControlModifier);
        const QKeyEvent releaseL(QEvent::KeyRelease, kShortcutKeyLeftCtrl, Qt::ControlModifier);
        const QKeyEvent releaseR(QEvent::KeyRelease, kShortcutKeyRightCtrl, Qt::NoModifier);
        const auto* dit = reloaded.action(QStringLiteral("cwdit"));
        const auto* dah = reloaded.action(QStringLiteral("cwdah"));
        ok &= expect(reloaded.actionForKey(shortcutSequenceFromKeyEvent(&pressL)) == dit
                         && reloaded.actionForKey(shortcutSequenceFromKeyEvent(&pressR)) == dah
                         && reloaded.actionForKey(shortcutSequenceFromKeyEvent(&releaseL)) == dit
                         && reloaded.actionForKey(shortcutSequenceFromKeyEvent(&releaseR)) == dah,
                     "squeeze: press L, press R, release L, release R resolve per paddle");

        // CSV export/import carries the sided names.
        const QByteArray csv = reloaded.exportBindingsCsv();
        ok &= expect(csv.contains("Left Ctrl") && csv.contains("Right Ctrl"),
                     "CSV export names the sided keys");
        settings.reset();
        ShortcutManager imported;
        registerCwActions(imported);
        imported.loadBindings();
        const ShortcutImportResult result = imported.importBindingsCsv(csv);
        ok &= expect(result.ok(), "CSV with sided keys imports");
        ok &= expect(imported.action(QStringLiteral("cwdit"))->currentKey
                             == QKeySequence(kShortcutKeyLeftCtrl)
                         && imported.action(QStringLiteral("cwdah"))->currentKey
                             == QKeySequence(kShortcutKeyRightCtrl),
                     "CSV import restores both paddles");
    }

    return ok ? 0 : 1;
}
