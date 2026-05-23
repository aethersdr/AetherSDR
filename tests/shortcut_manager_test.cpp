#include "core/AppSettings.h"
#include "core/ShortcutManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

using namespace MasterSDR;

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
    QTemporaryDir fakeHome(QDir::tempPath() + "/mastersdr-shortcut-manager-test-XXXXXX");
    if (!fakeHome.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    qputenv("HOME", fakeHome.path().toUtf8());
    qputenv("CFFIXED_USER_HOME", fakeHome.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);

    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QDir(configRoot + "/MasterSDR").removeRecursively();

    auto& settings = AppSettings::instance();
    settings.reset();

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

    QDir(configRoot + "/MasterSDR").removeRecursively();
    return ok ? 0 : 1;
}
