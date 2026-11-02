#include "ExternalFilePath.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace AetherSDR::ExternalFilePath {
namespace {

QString normalized(const QString& path)
{
    return QDir::cleanPath(QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')));
}

bool underRoot(const QString& path, const QString& root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0
        || path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

#ifdef Q_OS_WIN
QString knownFolder(REFKNOWNFOLDERID id, DWORD flags)
{
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, flags | KF_FLAG_DONT_VERIFY, nullptr, &raw);
    const QString path = SUCCEEDED(result) && raw ? QString::fromWCharArray(raw) : QString();
    CoTaskMemFree(raw);
    return path;
}
#endif

} // namespace

QString resolve(const QString& path, const QList<AppDataMapping>& mappings)
{
    if (path.isEmpty()) {
        return path;
    }
    const QString cleanPath = normalized(path);
    // LocalCache is normally below LocalAppData. Never redirect it twice.
    for (const AppDataMapping& mapping : mappings) {
        if (!mapping.redirectedRoot.isEmpty()
            && underRoot(cleanPath, normalized(mapping.redirectedRoot))) {
            return path;
        }
    }
    for (const AppDataMapping& mapping : mappings) {
        if (mapping.originalRoot.isEmpty() || mapping.redirectedRoot.isEmpty()) {
            continue;
        }
        const QString original = normalized(mapping.originalRoot);
        const QString redirected = normalized(mapping.redirectedRoot);
        if (underRoot(cleanPath, original)) {
            const QString candidate = redirected + cleanPath.mid(original.size());
            // Reads of the original path can succeed through MSIX's merged
            // view. Only existence at the physical candidate proves it usable
            // by Explorer or a mail client outside the package.
            return QFileInfo::exists(candidate) ? candidate : path;
        }
    }
    return path;
}

QString resolve(const QString& path)
{
#ifdef Q_OS_WIN
    // RETURN_FILTER_REDIRECTION_TARGET reports the physical AppData location
    // for packagedClassicApp; unpackaged processes get the original folder.
    QList<AppDataMapping> mappings;
    for (const KNOWNFOLDERID& id : {FOLDERID_LocalAppData, FOLDERID_RoamingAppData}) {
        mappings.append({knownFolder(id, KF_FLAG_NO_PACKAGE_REDIRECTION),
                         knownFolder(id, KF_FLAG_RETURN_FILTER_REDIRECTION_TARGET)});
    }
    return resolve(path, mappings);
#else
    return path;
#endif
}

} // namespace AetherSDR::ExternalFilePath
