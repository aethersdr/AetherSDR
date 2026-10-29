#pragma once

#include <QList>
#include <QString>

namespace AetherSDR::ExternalFilePath {

// Resolve an existing AppData file for display or an external application.
// Internal readers/writers keep their original path and Windows' merged view.
QString resolve(const QString& path);

struct AppDataMapping {
    QString originalRoot;
    QString redirectedRoot;
};

// The same resolver with explicit known-folder results, for socket-free tests.
QString resolve(const QString& path, const QList<AppDataMapping>& mappings);

} // namespace AetherSDR::ExternalFilePath
