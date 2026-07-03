#pragma once

#include "CallsignInfo.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

namespace AetherSDR {

class QrzClient;

// Application-wide callsign-lookup facade: QRZ client + lazy on-disk cache
// + photo download.  Every consumer (CW decoder card, lookup dialog, and
// the future SSB voice-decoder card) goes through here so a busy net never
// hits QRZ twice for the same station.
//
//   lookup("KI6BCJ")  →  infoReady(info, fromCache)   [+ photoReady later]
//                     or lookupFailed(call, message)
//
// Cache: one JSON file in QStandardPaths::CacheLocation, entries expire
// after kCacheTtlSec (7 days).  A stale entry is refreshed from the
// network but still served as a fallback when the refresh fails (offline
// operation keeps working).  Station photos land next to it under
// qrz-photos/ and ride the same TTL.
//
// Credentials: username + enable flag in the AppSettings["QrzLookup"]
// blob (QrzLookupSettings, Principle V), password in the OS keychain
// (key "qrz_password") — never in the settings file.  Call
// reloadConfiguration() after the setup tab changes any of them.
class CallsignLookupService : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kCacheTtlSec = 7 * 24 * 3600;

    static CallsignLookupService& instance();

    bool enabled() const { return m_enabled; }
    bool hasCredentials() const;

    // Kick a lookup.  Fresh cache hit → infoReady fires (queued, so
    // connect-then-call ordering is safe).  Otherwise the network path
    // runs; concurrent requests for the same call coalesce.
    void lookup(const QString& call, bool forceRefresh = false);

    // Local path of a cached station photo, or empty when none exists
    // yet (photoReady announces late arrivals).
    QString photoPathFor(const QString& call) const;

    int  cacheEntryCount();
    void clearCache();

    // Cache probes (setup tab, CW-spot gating, automation bridge).
    bool hasCachedEntry(const QString& call);
    CallsignInfo cachedEntry(const QString& call);  // !isValid() when absent

    // Async credential test for the setup tab; result via loginTestFinished.
    void testLogin(const QString& username, const QString& password);

    // Persist the QRZ password to the OS keychain (empty deletes the
    // entry) and refresh the client's credentials when done.
    void savePassword(const QString& password);

    // Async keychain read for the setup tab's password field; the value
    // arrives via the callback on the main thread ({} when none stored).
    void readPassword(std::function<void(const QString&)> callback);

    // Re-read AppSettings + keychain after the QRZ setup tab saves.
    void reloadConfiguration();

signals:
    void infoReady(const AetherSDR::CallsignInfo& info, bool fromCache);
    void lookupFailed(const QString& call, const QString& message);
    void photoReady(const QString& call, const QString& imagePath);
    void loginTestFinished(bool ok, const QString& message);
    void configurationChanged();

private:
    explicit CallsignLookupService(QObject* parent = nullptr);
    ~CallsignLookupService() override;

    void ensureCacheLoaded();
    void scheduleCacheSave();
    void saveCacheNow();
    QString cacheFilePath() const;
    QString photoDirPath() const;
    void fetchPhoto(const CallsignInfo& info);
    void loadPasswordFromKeychain();

    QrzClient* m_client{nullptr};
    QNetworkAccessManager m_photoNam;
    bool m_enabled{false};
    bool m_cacheLoaded{false};
    bool m_cacheDirty{false};
    QHash<QString, CallsignInfo> m_cache;
    QSet<QString> m_inFlight;
    QSet<QString> m_photoInFlight;
    QTimer m_saveTimer;
};

} // namespace AetherSDR
