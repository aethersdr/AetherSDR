#pragma once

#include "ShortcutManager.h"

#include <QString>

namespace AetherSDR {

struct ShortcutExportResult {
    int exportedCount{0};
    QString error;

    bool ok() const { return error.isEmpty(); }
};

class ShortcutFileTransfer
{
public:
    static ShortcutExportResult exportToFile(const ShortcutManager& manager,
                                             const QString& path);
    static ShortcutImportResult importFromFile(ShortcutManager& manager,
                                               const QString& path);
};

} // namespace AetherSDR
