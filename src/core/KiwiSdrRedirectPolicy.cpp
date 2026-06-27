#include "KiwiSdrRedirectPolicy.h"

#include <QUrl>

namespace AetherSDR::KiwiSdrRedirectPolicy {

QString canonicalHost(QString host)
{
    host = host.trimmed().toLower();
    while (host.endsWith(QLatin1Char('.'))) {
        host.chop(1);
    }
    return host;
}

QString proxyReceiverAlias(const QString& host)
{
    constexpr QLatin1StringView kProxySuffix{".proxy.kiwisdr.com"};
    constexpr QLatin1StringView kProxy2Suffix{".proxy2.kiwisdr.com"};
    if (host.endsWith(kProxySuffix) && host.size() > kProxySuffix.size()) {
        return host.left(host.size() - kProxySuffix.size());
    }
    if (host.endsWith(kProxy2Suffix) && host.size() > kProxy2Suffix.size()) {
        return host.left(host.size() - kProxy2Suffix.size());
    }
    return {};
}

bool isAllowedStatusRedirect(const QUrl& from, const QUrl& to, QString* detail)
{
    const QString scheme = to.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        if (detail) {
            *detail = QStringLiteral("unsupported scheme %1").arg(to.scheme());
        }
        return false;
    }

    const QString fromHost = canonicalHost(from.host());
    const QString toHost = canonicalHost(to.host());
    if (fromHost.isEmpty() || toHost.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("missing redirect host");
        }
        return false;
    }

    if (fromHost == toHost) {
        return true;
    }

    // Kiwi's public proxy can migrate one receiver alias from proxy to proxy2.
    // Keep that exception exact so a status page cannot move the WebSocket
    // target to an unrelated host after the policy preflight succeeds.
    const QString fromAlias = proxyReceiverAlias(fromHost);
    const QString toAlias = proxyReceiverAlias(toHost);
    if (!fromAlias.isEmpty() && fromAlias == toAlias) {
        return true;
    }

    if (detail) {
        *detail = QStringLiteral("cross-domain redirect from %1 to %2")
                      .arg(fromHost, toHost);
    }
    return false;
}

} // namespace AetherSDR::KiwiSdrRedirectPolicy
