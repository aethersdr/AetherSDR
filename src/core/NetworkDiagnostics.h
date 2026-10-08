#pragma once

#include <QString>

class QNetworkReply;

namespace AetherSDR::NetworkDiagnostics {

// Puts a failed external request into the support log (aether.network). Call
// right after creating the reply. A failure is logged once per host and error
// per session, with its TLS errors, so a map's tile storm costs one line; the
// first failure also records the TLS backend in use. Cancelled replies are
// not failures and are not logged.
void watch(QNetworkReply* reply, const char* what);

// The failure line watch() writes, without the dedup or the backend line.
QString describeFailure(const QNetworkReply* reply, const char* what);

} // namespace AetherSDR::NetworkDiagnostics
