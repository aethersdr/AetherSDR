#pragma once

// Small theme-coloured images for stylesheets (`image: url("...")`), painted
// once per colour set and cached on disk: a stylesheet can only name a file.
// Shared by the combo-box arrow (ComboStyle.h) and the canon check boxes and
// radio buttons (CanonIndicators.h).
//
// The cache is the user's own cache directory, never the shared temp
// directory, where another local user could plant a file under the predictable
// name. Each file is written atomically, so a reader never sees half of one,
// even with two processes building the same set. And it fails closed: a path
// is returned only once every file was written, so a caller can drop its rule
// and keep Qt's own drawing rather than name a file that is not there.

#include <QDir>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QtDebug>

#include <functional>

namespace AetherSDR {

// Draws one image on its logical canvas.
using ThemedImagePainter = std::function<void(QPainter&)>;

// Paints (or reuses) `name` under `subdir`, keyed by `key` (every colour the
// painter uses, so two themes never share a file), at 1x up to `maxScale`x
// ("name.png", "name@2x.png", ...; Qt picks the sharp one for the screen).
// Returns the 1x path, or an empty string if any file could not be written.
inline QString themedImagePath(const QString& subdir, const QString& name,
                               const QString& key, QSize size, int maxScale,
                               const ThemedImagePainter& paint)
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
             + QStringLiteral("/AetherSDR");
    }
    const QString dir = root + QLatin1Char('/') + subdir;
    if (!QDir().mkpath(dir)) {
        qWarning() << "themed image cache: cannot create" << dir;
        return {};
    }
    const QString base = dir + QStringLiteral("/%1_%2").arg(name, key);
    const auto scaled = [&base](int scale) {
        return scale == 1 ? base + QStringLiteral(".png")
                          : base + QStringLiteral("@%1x.png").arg(scale);
    };
    bool cached = true;
    for (int scale = 1; scale <= maxScale && cached; ++scale) {
        cached = QFile::exists(scaled(scale));
    }
    if (cached) {
        return scaled(1);
    }
    for (int scale = 1; scale <= maxScale; ++scale) {
        QPixmap pm(size * scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        paint(p);
        p.end();
        QSaveFile file(scaled(scale));
        if (!file.open(QIODevice::WriteOnly) || !pm.save(&file, "PNG") || !file.commit()) {
            qWarning() << "themed image cache: cannot write" << scaled(scale);
            return {};
        }
    }
    return scaled(1);
}

} // namespace AetherSDR
