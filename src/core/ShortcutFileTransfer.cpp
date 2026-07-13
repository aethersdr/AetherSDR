#include "ShortcutFileTransfer.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace AetherSDR {

ShortcutExportResult ShortcutFileTransfer::exportToFile(const ShortcutManager& manager,
                                                        const QString& path)
{
    ShortcutExportResult result;
    if (path.trimmed().isEmpty()) {
        result.error = QStringLiteral("No export path was provided.");
        return result;
    }

    const QByteArray csv = manager.exportBindingsCsv();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Couldn't open %1 for writing.")
                           .arg(QDir::toNativeSeparators(path));
        return result;
    }
    if (file.write(csv) != csv.size()) {
        file.cancelWriting();
        result.error = QStringLiteral("Couldn't write the shortcut backup to %1.")
                           .arg(QDir::toNativeSeparators(path));
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("Couldn't save %1.")
                           .arg(QDir::toNativeSeparators(path));
        return result;
    }
    result.exportedCount = manager.actions().size();
    return result;
}

ShortcutImportResult ShortcutFileTransfer::importFromFile(ShortcutManager& manager,
                                                          const QString& path)
{
    ShortcutImportResult result;
    if (path.trimmed().isEmpty()) {
        result.errors << QStringLiteral("No import path was provided.");
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors << QStringLiteral("Couldn't open %1 for reading.")
                             .arg(QDir::toNativeSeparators(path));
        return result;
    }
    return manager.importBindingsCsv(file.readAll());
}

} // namespace AetherSDR
