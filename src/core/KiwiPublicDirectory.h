#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

namespace AetherSDR {

// One receiver from the public KiwiSDR directory (kiwisdr.com/public).
// Every field is sysop-published in the directory itself; in particular
// ext_api is the operator's external-API policy, which AetherSDR honors
// BEFORE ever attempting a connection.
struct KiwiPublicReceiver {
    QString url;        // http://host:port — the receiver endpoint
    QString name;       // sysop description
    QString location;   // human-readable location ("loc")
    QString antenna;
    QString sdrHw;      // "KiwiSDR 1 v1.842 …"
    QString grid;       // Maidenhead
    QString gps;        // "(lat, lon)"
    QString bands;      // e.g. "0-30000000"
    QString snr;
    int  users{0};
    int  usersMax{0};
    int  extApi{-1};    // sysop's max external-API connections.
                        // 0 = API disabled (web-only). -1 = not published.
    bool offline{false};

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

    // Short human-readable badge for the receiver picker.
    QString apiBadge() const;
    bool advertisesConnectionLimit() const;
    QString connectionLimitBadge() const;
};

// Fetches and parses the public KiwiSDR receiver directory, exposing each
// receiver's external-API policy so AetherSDR can honor "web-only" operators
// up front.
//
// Good-citizen contract (this class is the proof we can show an operator):
//   • Honest identity — a fixed "AetherSDR/<ver>" User-Agent; it NEVER spoofs
//     a browser to get past the directory's interactive gate.
//   • Strictly manual — fetch() is only ever called from an explicit user
//     action.  No background polling, no timed refresh, no caching-and-
//     redistribution, no enumeration.  One human click == one fetch.
//   • Reads only the server-published directory (HTML comments) and the
//     per-receiver /status — never the KiwiSDR source.
class KiwiPublicDirectory : public QObject {
    Q_OBJECT
public:
    explicit KiwiPublicDirectory(QObject* parent = nullptr);

    // Honest interactive fetch: load the gate page (to read its one-time
    // token), replay the documented unlock request, then GET the list — all
    // with the AetherSDR User-Agent.  Emits ready() or failed().
    void fetch();

    // Pure parser (no network) — extracts receivers from directory HTML.
    // Exposed so it can run offline and under test.
    static QVector<KiwiPublicReceiver> parse(const QByteArray& directoryHtml);

    // The honest User-Agent AetherSDR identifies with.
    static QByteArray userAgent();

    static constexpr const char* kDirectoryUrl = "http://kiwisdr.com/public/";

signals:
    void ready(const QVector<AetherSDR::KiwiPublicReceiver>& receivers);
    void failed(const QString& error);

private:
    QNetworkAccessManager* m_net{nullptr};
};

} // namespace AetherSDR
