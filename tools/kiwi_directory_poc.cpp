// Proof-of-concept: AetherSDR's honest, API-policy-aware read of the public
// KiwiSDR directory.
//
// The list comes from AetherSDR's own mirror (cdn.aethersdr.com/kiwi.json),
// which exists at the KiwiSDR maintainer's request: a Cloudflare Worker pulls
// kiwisdr.com/public once an hour under a shared secret he provided, and every
// AetherSDR client reads our copy.  His server sees one hourly request instead
// of one per user, per browse — which is the load he asked us to remove.  The
// client never contacts kiwisdr.com, and there is deliberately no fallback that
// would send it there when our CDN has a bad day.
//
// This tool demonstrates how AetherSDR honors each operator's external-API
// policy (ext_api) BEFORE attempting any connection:
//   • ext_api == 0   → "web only": AetherSDR will NOT open a native API
//                      connection; it routes the user to the web client.
//   • ext_api  > 0   → API permitted (up to that many channels).
//   • ext_api absent → policy not published; treated as "do not assume API is
//                      allowed".  In kiwi.json this is a MISSING KEY, not a
//                      zero, and the two must never be conflated.
//
// It identifies honestly as "AetherSDR/<ver>" — never a spoofed browser.
//
// Usage:
//   kiwi_directory_poc            # fetch from the AetherSDR mirror
//   kiwi_directory_poc <file>     # parse a previously-saved kiwi.json

#include "core/KiwiPublicDirectory.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>

using AetherSDR::KiwiDirectoryParse;
using AetherSDR::KiwiPublicDirectory;
using AetherSDR::KiwiPublicReceiver;

static void report(const KiwiDirectoryParse& parsed)
{
    QTextStream out(stdout);
    const auto& rxs = parsed.receivers;
    int disabled = 0, limited = 0, open = 0, unknown = 0, flagged = 0, offline = 0;
    for (const auto& r : rxs) {
        if (r.flagged) ++flagged;
        if (r.offline) ++offline;
        switch (r.apiPolicy()) {
            case KiwiPublicReceiver::ApiPolicy::Disabled: ++disabled; break;
            case KiwiPublicReceiver::ApiPolicy::Limited:  ++limited;  break;
            case KiwiPublicReceiver::ApiPolicy::Open:     ++open;     break;
            case KiwiPublicReceiver::ApiPolicy::Unknown:  ++unknown;  break;
        }
    }

    out << "\n================ AetherSDR — KiwiSDR public directory ================\n";
    out << "Source          : " << KiwiPublicDirectory::kDirectoryUrl << "\n";
    out << "                  (AetherSDR's mirror of kiwisdr.com/public, at the\n";
    out << "                   maintainer's request — clients never hit his server)\n";
    out << "User-Agent sent : " << KiwiPublicDirectory::userAgent() << "\n";
    out << "Schema          : " << parsed.schema << "\n";
    out << "Mirror fetched  : "
        << (parsed.fetchedAt.isValid() ? parsed.fetchedAt.toString(Qt::ISODate)
                                       : QStringLiteral("(not published)")) << "\n";
    out << "Receivers parsed: " << rxs.size()
        << "   (" << offline << " offline, " << flagged << " flagged by the origin)\n\n";
    out << "Per-operator external-API policy (read from the directory):\n";
    out << "  web-only (ext_api=0) : " << disabled
        << "   <- AetherSDR will NOT connect via API; uses web client\n";
    out << "  API limited          : " << limited  << "   (some channels reserved for web)\n";
    out << "  API open             : " << open     << "\n";
    out << "  policy not published : " << unknown
        << "   <- no ext_api key at all; NOT the same as 0\n\n";

    out << "--- honoring 'web only' operators (first 8 of " << disabled << ") ---\n";
    int shown = 0;
    for (const auto& r : rxs) {
        if (r.apiPolicy() != KiwiPublicReceiver::ApiPolicy::Disabled) continue;
        out << "  [WEB ONLY] " << r.url << "\n"
            << "             " << r.location << "  |  " << r.apiBadge() << "\n"
            << "             mayConnectViaApi() = "
            << (r.mayConnectViaApi() ? "true  (!!)" : "false  -> route to web client") << "\n";
        if (++shown >= 8) break;
    }

    out << "\n--- policy not published (first 8 of " << unknown << ") ---\n";
    shown = 0;
    for (const auto& r : rxs) {
        if (r.apiPolicy() != KiwiPublicReceiver::ApiPolicy::Unknown) continue;
        out << "  [UNKNOWN]  " << r.url << "  |  extApi = " << r.extApi
            << "  |  mayConnectViaApi() = "
            << (r.mayConnectViaApi() ? "true  (!!)" : "false") << "\n";
        if (++shown >= 8) break;
    }

    out << "\n--- a few API-permitted operators (AetherSDR streams natively) ---\n";
    shown = 0;
    for (const auto& r : rxs) {
        if (r.apiPolicy() == KiwiPublicReceiver::ApiPolicy::Disabled
            || r.apiPolicy() == KiwiPublicReceiver::ApiPolicy::Unknown) continue;
        out << "  [API OK]   " << r.url << "  |  users " << r.users << "/" << r.usersMax
            << "  |  " << r.apiBadge();
        if (r.hasBands) out << "  |  " << r.bandRangeLabel();
        out << "\n";
        if (++shown >= 6) break;
    }
    out << "=====================================================================\n";
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    if (argc > 1) {
        // Offline parse of a saved kiwi.json.
        QFile f(QString::fromLocal8Bit(argv[1]));
        if (!f.open(QIODevice::ReadOnly)) {
            QTextStream(stderr) << "cannot open " << argv[1] << "\n";
            return 1;
        }
        const KiwiDirectoryParse parsed = KiwiPublicDirectory::parse(f.readAll());
        if (!parsed.ok()) {
            QTextStream(stderr) << "parse failed: " << parsed.error << "\n";
            return 1;
        }
        report(parsed);
        return 0;
    }

    // Live fetch from the mirror.
    KiwiPublicDirectory dir;
    QObject::connect(&dir, &KiwiPublicDirectory::ready,
                     [](const QVector<KiwiPublicReceiver>& rxs, const QDateTime& fetchedAt) {
        KiwiDirectoryParse parsed;
        parsed.receivers = rxs;
        parsed.fetchedAt = fetchedAt;
        parsed.schema = KiwiPublicDirectory::kSupportedSchema;
        report(parsed);
        QCoreApplication::quit();
    });
    QObject::connect(&dir, &KiwiPublicDirectory::failed, [](const QString& err) {
        QTextStream(stderr) << "fetch failed: " << err << "\n";
        QCoreApplication::exit(1);
    });
    QTimer::singleShot(0, &dir, &KiwiPublicDirectory::fetch);
    QTimer::singleShot(30000, []() {
        QTextStream(stderr) << "timeout\n"; QCoreApplication::exit(1);
    });
    return app.exec();
}
