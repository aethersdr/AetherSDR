#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Connection metadata and client preferences only. Radio operating state and
// credentials belong to their existing owners, never to this document.
class ConnectionWorkspace {
public:
    static constexpr int kVersion = 1;
    static QJsonObject load();
    static bool save(const QJsonObject& document, QString* error = nullptr);
    static QString newId();
    static QJsonObject find(const QJsonArray& records, const QString& id);
    static bool uniqueName(const QJsonArray& records, const QString& name,
                           const QString& exceptId = {});
    static QJsonObject captureOperatorPreferences();
    static QJsonObject defaultOperatorPreferences();
    static QString accountId(const QString& email);
    static QString credentialKey(const QString& accountId);
    static QString connectionCredentialId(const QString& host, const QString& username);
};

} // namespace AetherSDR
