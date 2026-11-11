#pragma once

#include <QObject>
#include <QString>
#include <QList>

#include <functional>
#include <optional>

namespace AetherSDR {
// Connection-form model values. The GUI stages inputs here; backend construction
// consumes them through the neutral request without exposing vendor transports.
struct RadioConnectRequest;
struct NetworkRadioVariant {
    QString id;
    QString label;
};
struct NetworkRadioProfile {
    QString family;
    QString label;
    QString model;
    QString settingsKey;
    QString credentialKey;
    QString parameterPrefix;
    quint16 controlPort;
    quint16 audioPort;
    QList<NetworkRadioVariant> variants;
};
struct NetworkRadioLogin {
    QString host;
    QString username;
    QString password;
    quint16 controlPort = 60000;
    quint16 audioPort = 60001;
    bool administrator = false;
    bool savePassword = false;
    QString variant;
};
struct NetworkRadioCredentialStore {
    std::function<void(const QString&, QObject*, std::function<void(QString)>)> read;
    std::function<void(const QString&, const QString&, QObject*)> write;
};
// Session-reset seam for credential-store tests; never touches persistent data.
struct NetworkRadioLoginTestAccess { static void clearSession(); };
QString networkRadioCredentialKey(const NetworkRadioProfile& profile, const NetworkRadioLogin& login);
QList<NetworkRadioProfile> networkRadioProfiles();
std::optional<NetworkRadioProfile> networkRadioProfile(const QString& family);
void loadNetworkRadioLogin(const QString& family, QObject* context,
                          std::function<void(NetworkRadioLogin)> callback,
                          const NetworkRadioCredentialStore* store = nullptr);
void invalidateNetworkRadioLoginRevision(const QString& family);
bool networkRadioPasswordStoreAvailable();
void stageNetworkRadioLogin(const QString& family, const NetworkRadioLogin& login);
void populateNetworkRadioParams(RadioConnectRequest& request, const QString& family);
void commitNetworkRadioLogin(const QString& family, const QString& host,
                             quint64 revision, QObject* context,
                             const NetworkRadioCredentialStore* store = nullptr);
} // namespace AetherSDR
