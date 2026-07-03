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
class CtyDatParser;

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

    static constexpr int kPrefixFallbackMs = 5000;

    bool enabled() const { return m_enabled; }
    bool hasCredentials() const;

    // Loaded cty.dat parser (owned elsewhere — MainWindow's DxccColorProvider).
    // Enables country-level prefix-fallback cards when QRZ can't answer.
    void setCtyParser(const CtyDatParser* parser) { m_ctyParser = parser; }

    // Operator's own position (radio GPS fix, or grid-square center) used to
    // stamp distance/bearing onto every emitted CallsignInfo.  GPS-sourced
    // positions take priority over the own-callsign fallback below.
    void setOwnLocation(double lat, double lon);
    void clearOwnLocation() { m_hasOwnLocation = false; }
    bool hasOwnLocation() const { return m_hasOwnLocation; }

    // Zero-config own-position fallback for radios without GPS: the
    // operator's own QRZ record (or its cache entry) carries their grid, so
    // a lookup of the radio's callsign supplies the missing home position.
    // A GPS fix, when one exists, always overrides this.
    void setOwnCallsign(const QString& call);

    // Can lookup(call) produce *something* for this call — QRZ credentials,
    // a cache entry, or at least cty.dat prefix data?  The CW screen-pop
    // gates on this so a pending card can never sit unfillable.
    bool canResolve(const QString& call);

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

    // cty.dat country-level stand-in for `call`; !isValid() when the prefix
    // doesn't resolve (or no parser was provided).
    CallsignInfo prefixInfo(const QString& call) const;
    // Emit the prefix stand-in for a call (at most once per lookup attempt).
    // Returns false when no prefix data exists for the call.
    bool emitPrefixFallback(const QString& call);
    // Stamp distance/bearing from the operator's position onto an outgoing
    // info (exact lat/lon > grid center > DXCC country center).
    void stampGeo(CallsignInfo& info) const;
    // Adopt the operator's own QRZ record as home position (unless GPS won).
    void maybeAdoptOwnLocation(const CallsignInfo& info);

    QrzClient* m_client{nullptr};
    QNetworkAccessManager m_photoNam;
    const CtyDatParser* m_ctyParser{nullptr};
    bool m_enabled{false};
    bool m_cacheLoaded{false};
    bool m_cacheDirty{false};
    bool m_hasOwnLocation{false};
    bool m_ownLocationFromGps{false};
    double m_ownLat{0.0};
    double m_ownLon{0.0};
    QString m_ownCallsign;
    QHash<QString, CallsignInfo> m_cache;
    QSet<QString> m_inFlight;
    QSet<QString> m_fallbackShown;   // calls already given a prefix card this attempt
    QSet<QString> m_photoInFlight;
    QTimer m_saveTimer;
};

} // namespace AetherSDR
