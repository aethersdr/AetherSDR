#pragma once

#include <QString>
#include <QUrl>

#include <optional>

namespace AetherSDR {

// Where a link clicked in the What's New release notes may open. A relative
// link resolves against the repository on GitHub; only http and https leave
// the app. Any other scheme (file:, a custom app handler, javascript:) is
// refused, because the notes are fetched text the operator did not write.
inline std::optional<QUrl> whatsNewLinkTarget(const QUrl& url)
{
    const QUrl target = url.isRelative()
        ? QUrl(QStringLiteral("https://github.com/aethersdr/AetherSDR/")).resolved(url)
        : url;
    const QString scheme = target.scheme().toLower();
    if (!target.isValid() || target.host().isEmpty()
        || (scheme != QLatin1String("https") && scheme != QLatin1String("http"))) {
        return std::nullopt;
    }
    return target;
}

}  // namespace AetherSDR
