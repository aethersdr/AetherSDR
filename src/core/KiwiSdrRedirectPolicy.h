#pragma once

#include <QString>

class QUrl;

namespace AetherSDR::KiwiSdrRedirectPolicy {

QString canonicalHost(QString host);
QString proxyReceiverAlias(const QString& host);
bool isAllowedStatusRedirect(const QUrl& from, const QUrl& to,
                             QString* detail = nullptr);

} // namespace AetherSDR::KiwiSdrRedirectPolicy
