#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

namespace AetherSDR {

// One receiver from the public KiwiSDR directory, as republished by the
// AetherSDR mirror (cdn.aethersdr.com/kiwi.json).  Every field is
// sysop-published at the origin; in particular ext_api is the operator's
// external-API policy, which AetherSDR honors BEFORE ever attempting a
// connection.
struct KiwiPublicReceiver {
    QString id;         // origin's stable receiver id
    QString url;        // http://host:port — the receiver endpoint
    QString name;       // sysop description
    QString location;   // human-readable location ("loc")
    QString antenna;
    QString sdrHw;      // "KiwiSDR 1 v1.842 …"
    QString grid;       // Maidenhead

    // Position, published as a [lat, lon] pair.  hasGps is false when the
    // receiver publishes no usable fix, so 0,0 is never mistaken for one.
    double gpsLat{0.0};
    double gpsLon{0.0};
    bool   hasGps{false};

    // Tuning range, published as a [low, high] Hz pair.
    qint64 bandLowHz{0};
    qint64 bandHighHz{0};
    bool   hasBands{false};

    // Signal-to-noise, published as an [all-band, HF] pair in dB.
    // -1 = not published.
    int snrAll{-1};
    int snrHf{-1};

    int  users{0};
    int  usersMax{0};
    int  extApi{-1};    // sysop's max external-API connections.
                        // 0 = API disabled (web-only). -1 = not published.
                        // In kiwi.json a receiver that does not publish a
                        // policy has NO ext_api key at all — read it with an
                        // explicit -1 default, never toInt(), or every Unknown
                        // silently becomes a Disabled.
    bool offline{false};
    bool flagged{false};  // the origin itself marks this entry as bad

    enum class ApiPolicy {
        Unknown,    // ext_api not published
        Disabled,   // ext_api == 0  → sysop wants web-only
        Limited,    // 0 < ext_api < usersMax → some channels reserved for web
        Open,       // ext_api >= usersMax → all channels open to API
    };
    ApiPolicy apiPolicy() const {
        if (extApi < 0) return ApiPolicy::Unknown;
        if (extApi == 0) return ApiPolicy::Disabled;
        if (usersMax > 0 && extApi < usersMax) return ApiPolicy::Limited;
        return ApiPolicy::Open;
    }

    // THE honor decision.  When the sysop has disabled the external API
    // (ext_api == 0) AetherSDR must NOT open a native API/WebSocket connection
    // — that receiver is used via its web client instead.  Unknown policy is
    // treated conservatively as "do not assume API is allowed".
    bool mayConnectViaApi() const { return extApi > 0; }

    // Why this receiver is, or is not, offered in the picker.  The policy
    // lives here rather than inside the picker's loop so the GUI and the test
    // cannot drift apart — a test that re-implements the loop would keep
    // passing after the loop regressed.
    enum class Offer {
        Yes,
        HiddenOffline,
        HiddenFlagged,        // the origin itself marks the entry as bad
        HiddenWebOnly,        // ext_api == 0: operator disabled the API
        HiddenPolicyUnknown,  // no ext_api published: we can't confirm it's OK
    };
    Offer offerDecision() const
    {
        if (offline) return Offer::HiddenOffline;
        if (flagged) return Offer::HiddenFlagged;
        if (!mayConnectViaApi()) {
            return apiPolicy() == ApiPolicy::Disabled ? Offer::HiddenWebOnly
                                                      : Offer::HiddenPolicyUnknown;
        }
        return Offer::Yes;
    }

    // Short human-readable badge for the receiver picker.
    QString apiBadge() const;
    bool advertisesConnectionLimit() const;
    QString connectionLimitBadge() const;

    // "2.0 – 30.0 MHz", or empty when the receiver publishes no range.
    QString bandRangeLabel() const;
};

// Outcome of parsing one kiwi.json body.  Parsing either yields a whole list
// or fails with a reason — never a partial list built from a truncated or
// wrong-schema document (Principle VII: fail closed at the boundary).
struct KiwiDirectoryParse {
    QVector<KiwiPublicReceiver> receivers;
    QDateTime fetchedAt;      // when the mirror last pulled the origin (UTC)
    int       schema{0};
    QString   error;          // empty iff the parse succeeded

    bool ok() const { return error.isEmpty(); }
};

// Fetches and parses the public KiwiSDR receiver directory from AetherSDR's
// mirror, exposing each receiver's external-API policy so AetherSDR can honor
// "web-only" operators up front.
//
// Good-citizen contract (this class is the proof we can show an operator):
//   • The mirror exists AT THE KIWISDR MAINTAINER'S REQUEST.  AetherSDR
//     clients' individual directory fetches were putting real load on his
//     server; he asked us to pull once and redistribute instead.  A
//     Cloudflare Worker pulls kiwisdr.com/public hourly under a shared secret
//     he provided and republishes it as JSON.  His server now sees one hourly
//     request from us rather than one per user, per browse.
//   • Clients read AetherSDR's copy and NEVER contact the kiwisdr.com
//     directory origin (a receiver the user then picks may itself be a
//     *.proxy.kiwisdr.com host — that connection is unchanged).  There is
//     deliberately no origin fallback: a CDN outage must not turn every
//     AetherSDR install in the world into a thundering herd aimed at the
//     server we were asked to relieve.  When the mirror is unreachable we
//     serve the last list we have, or say so plainly.
//   • Honest identity — a fixed "AetherSDR/<ver>" User-Agent, unchanged.  No
//     browser spoofing, no gate token to replay, no HTML to scrape.
//   • ext_api is still honored client-side: receivers whose operator set
//     ext_api == 0 are never offered for a native API connection, and a
//     receiver that publishes no policy at all is not assumed to permit one.
//   • Refresh respects the mirror's own 30-minute cache lifetime; we do not
//     poll faster than the data can change, and a manual refresh stays
//     available for users who want one.
//
// See docs/kiwisdr-public-directory.md.
class KiwiPublicDirectory : public QObject {
    Q_OBJECT
public:
    explicit KiwiPublicDirectory(QObject* parent = nullptr);

    // Fetch the mirror and emit ready() or failed().  One GET, no gate dance.
    void fetch();

    // Pure parser (no network).  Exposed so it can run offline and under test.
    static KiwiDirectoryParse parse(const QByteArray& json);

    // The honest User-Agent AetherSDR identifies with.
    static QByteArray userAgent();

    static constexpr const char* kDirectoryUrl = "https://cdn.aethersdr.com/kiwi.json";

    // The only kiwi.json schema this build understands.  A different value is
    // a hard failure telling the user to update, not something to parse on
    // hopefully — the fields we honor could have moved under our feet.
    // The producer lives outside this repo, so docs/kiwi-json-schema.md is the
    // in-tree contract this pins to; change one and change the other.
    static constexpr int kSupportedSchema = 1;

    // The mirror publishes cache-control: max-age=1800; refreshing faster than
    // that only re-reads bytes the CDN is still serving from cache.
    static constexpr int kMinRefreshSeconds = 1800;

    // The mirror's own client-facing staleness threshold.  Past this the
    // picker tells the user how old the list is.
    static constexpr int kStaleAfterMinutes = 360;

    // Boundary caps (Principle VII).  kMaxBodyBytes is enforced twice: on the
    // wire, where fetch() aborts the reply as soon as the advertised or
    // received length passes it (so an oversized body is never buffered whole),
    // and again in parse(), which is also reachable with bytes we did not
    // download — a saved payload handed to the POC.
    static constexpr int kMaxBodyBytes = 32 * 1024 * 1024;
    static constexpr int kMaxReceivers = 20000;

signals:
    void ready(const QVector<AetherSDR::KiwiPublicReceiver>& receivers,
               const QDateTime& fetchedAt);
    void failed(const QString& error);

private:
    QNetworkAccessManager* m_net{nullptr};
};

} // namespace AetherSDR
