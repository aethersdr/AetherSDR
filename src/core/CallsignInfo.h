#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// One station's contact data as returned by a callsign-database lookup
// (QRZ.com XML API today; the struct is provider-neutral so a future
// HamQTH/hamdb backend can fill the same fields).  Instances round-trip
// through JSON for the on-disk lookup cache.
struct CallsignInfo {
    QString call;          // canonical uppercase callsign
    QString firstName;     // QRZ <fname>
    QString lastName;      // QRZ <name> (last name)
    QString nameFmt;       // QRZ <name_fmt> — provider-formatted full name
    QString nickname;      // QRZ <nickname>
    QString city;          // QRZ <addr2>
    QString state;         // QRZ <state>
    QString country;       // QRZ <country>
    QString county;        // QRZ <county>
    QString grid;          // Maidenhead locator, QRZ <grid>
    QString licenseClass;  // QRZ <class>
    QString email;         // QRZ <email>
    QString url;           // QRZ <url> — station web page
    QString imageUrl;      // QRZ <image> — station photo URL
    double  latitude{0.0};
    double  longitude{0.0};
    bool    hasLatLon{false};
    bool    lotw{false};   // accepts QSL via Logbook of The World
    bool    eqsl{false};   // accepts QSL via eQSL
    bool    mailQsl{false};// returns paper QSL, QRZ <mqsl>
    qint64  fetchedUtc{0}; // epoch seconds when fetched from the provider

    // ── Transient (never serialized to the cache) ───────────────────────
    // prefixOnly: built from cty.dat prefix data because QRZ was
    // unreachable/slow/unconfigured — country-level accuracy only.
    bool    prefixOnly{false};
    QString continent;         // cty.dat continent ("NA") for prefix cards
    int     cqZone{0};         // cty.dat CQ zone for prefix cards
    // Distance/bearing from the operator's own position, stamped by
    // CallsignLookupService at emit time (own position changes; the cache
    // entry must not bake it in).  distanceKm < 0 → unknown.
    double  distanceKm{-1.0};
    double  bearingDeg{-1.0};
    bool    distanceApprox{false};  // position came from the DXCC country center

    bool isValid() const { return !call.isEmpty(); }

    // Age-based staleness against the lookup cache TTL.
    bool isOlderThan(qint64 ttlSec, qint64 nowUtc = QDateTime::currentSecsSinceEpoch()) const
    {
        return fetchedUtc <= 0 || (nowUtc - fetchedUtc) > ttlSec;
    }

    // "Patrick Jensen" — name_fmt when the provider gives one, else
    // first + last, else the bare callsign.
    QString displayName() const;

    // "San Jose, CA, United States" — skips empty parts.
    QString displayLocation() const;

    QJsonObject toJson() const;
    static CallsignInfo fromJson(const QJsonObject& obj);
};

} // namespace AetherSDR
