#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
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

    QFile saved(settings.filePath());
    bool ok = expect(saved.open(QIODevice::ReadOnly | QIODevice::Text),
                     "shortcut settings file is written");
    const QString xml = ok ? QString::fromUtf8(saved.readAll()) : QString();
    saved.close();

    ok &= expect(xml.contains(QStringLiteral("<Shortcut_cwkey>F9</Shortcut_cwkey>")),
                 "straight-key shortcut persists as Shortcut_cwkey");
    ok &= expect(xml.contains(QStringLiteral("<Shortcut_cwdit>F10</Shortcut_cwdit>")),
                 "dit shortcut persists as Shortcut_cwdit");
    ok &= expect(xml.contains(QStringLiteral("<Shortcut_cwdah>F11</Shortcut_cwdah>")),
                 "dah shortcut persists as Shortcut_cwdah");
    ok &= expect(!xml.contains(QStringLiteral("Shortcut_cw.key")),
                 "shortcut XML does not use dotted CW IDs");
    ok &= expect(!xml.contains(QStringLiteral("Shortcut_cw.dit")),
                 "shortcut XML does not use dotted CW dit ID");
    ok &= expect(!xml.contains(QStringLiteral("Shortcut_cw.dah")),
                 "shortcut XML does not use dotted CW dah ID");

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

    return ok ? 0 : 1;
}
