#include "core/KiwiPublicDirectory.h"

#include <cstdio>

using AetherSDR::KiwiDirectoryParse;
using AetherSDR::KiwiPublicDirectory;
using AetherSDR::KiwiPublicReceiver;
using ApiPolicy = AetherSDR::KiwiPublicReceiver::ApiPolicy;

namespace {

int fail(const char* msg)
{
    std::fprintf(stderr, "kiwi_public_directory_test: %s\n", msg);
    return 1;
}

// A faithful slice of cdn.aethersdr.com/kiwi.json in the real published shape:
// one object with a schema, a fetched_at and a receivers array, each entry
// carrying typed gps/bands/snr pairs and a real JSON bool for offline.
//
// It covers all FOUR ext_api regimes.  The fourth — the key being ABSENT — is
// the one the origin's HTML never distinguished and the one this migration is
// most likely to break: QJsonObject::value("ext_api").toInt() returns 0 for a
// missing key, which would silently reclassify every policy-unknown operator as
// a policy-disabled one.
const QByteArray kSample = QByteArrayLiteral(R"JSON({
  "schema": 1,
  "source": "https://files.kiwisdr.com/public/",
  "fetched_at": "2026-09-05T14:00:01Z",
  "receiver_count": 4,
  "receivers": [
    {
      "id": "c4f312a0a6ef",
      "url": "http://g3sdr.com:8073",
      "name": "G3SDR, Weston-super-Mare",
      "loc": "Weston-super-Mare, United Kingdom",
      "antenna": "Parallel dipoles",
      "sdr_hw": "KiwiSDR 1 v1.842 Limits",
      "grid": "IO81",
      "gps": [51.3458, -2.9773],
      "bands": [0, 30000000],
      "snr": [30, 31],
      "users": 3,
      "users_max": 4,
      "ext_api": 0,
      "offline": false,
      "flagged": false
    },
    {
      "id": "0000000000aa",
      "url": "http://open.example.com:8073",
      "name": "Open RX",
      "loc": "Somewhere",
      "sdr_hw": "KiwiSDR 1 v1.842",
      "gps": [0, 0],
      "bands": [2000000, 30000000],
      "snr": [42, 43],
      "users": 1,
      "users_max": 4,
      "ext_api": 4,
      "offline": false,
      "flagged": false
    },
    {
      "id": "0000000000bb",
      "url": "http://limited.example.com:8074",
      "name": "Limited RX",
      "loc": "Elsewhere",
      "gps": [-34.2737, 138.771],
      "bands": [10000000, 15000000],
      "snr": [38, 39],
      "users": 2,
      "users_max": 8,
      "ext_api": 4,
      "offline": false,
      "flagged": true
    },
    {
      "id": "0000000000cc",
      "url": "http://silent.proxy.kiwisdr.com",
      "name": "Policy-unknown RX",
      "loc": "Unstated",
      "users": 0,
      "users_max": 3,
      "offline": false,
      "flagged": false
    }
  ]
})JSON");

} // namespace

int main()
{
    const KiwiDirectoryParse parsed = KiwiPublicDirectory::parse(kSample);
    if (!parsed.ok()) return fail("valid kiwi.json must parse");
    if (parsed.schema != 1) return fail("schema must be read back as 1");
    if (!parsed.fetchedAt.isValid()) return fail("fetched_at must parse as ISO-8601");
    if (parsed.fetchedAt.toString(Qt::ISODate) != QStringLiteral("2026-09-05T14:00:01Z"))
        return fail("fetched_at must round-trip as UTC");

    const auto& rxs = parsed.receivers;
    if (rxs.size() != 4) return fail("expected 4 receivers");

    // ---- ext_api == 0 -> Disabled -------------------------------------
    const KiwiPublicReceiver& web = rxs[0];
    if (web.id != QStringLiteral("c4f312a0a6ef")) return fail("id parse");
    if (web.url != QStringLiteral("http://g3sdr.com:8073")) return fail("url parse");
    if (web.name != QStringLiteral("G3SDR, Weston-super-Mare")) return fail("name parse");
    if (web.location != QStringLiteral("Weston-super-Mare, United Kingdom")) return fail("loc parse");
    if (web.grid != QStringLiteral("IO81")) return fail("grid parse");
    if (web.users != 3 || web.usersMax != 4) return fail("users/users_max parse");
    if (web.offline) return fail("offline JSON false must parse as not offline");
    if (web.extApi != 0) return fail("ext_api parse");
    if (web.apiPolicy() != ApiPolicy::Disabled) return fail("ext_api=0 must be Disabled");
    if (!web.advertisesConnectionLimit()) return fail("limits marker parse");
    if (web.connectionLimitBadge() != QStringLiteral("Limits")) return fail("limits badge");
    // THE honor guarantee: a web-only receiver is never API-connectable.
    if (web.mayConnectViaApi()) return fail("ext_api=0 must NOT allow API connect");
    // Typed fields.
    if (!web.hasGps) return fail("gps pair must parse");
    if (web.gpsLat < 51.34 || web.gpsLat > 51.35) return fail("gps lat parse");
    if (web.gpsLon > -2.97 || web.gpsLon < -2.98) return fail("gps lon parse");
    if (!web.hasBands || web.bandLowHz != 0 || web.bandHighHz != 30000000)
        return fail("bands pair must parse");
    if (web.snrAll != 30 || web.snrHf != 31) return fail("snr pair parse");

    // ---- ext_api >= users_max -> Open ---------------------------------
    const KiwiPublicReceiver& open = rxs[1];
    if (open.extApi != 4 || open.usersMax != 4) return fail("open parse");
    if (open.apiPolicy() != ApiPolicy::Open) return fail("ext_api==users_max must be Open");
    if (open.advertisesConnectionLimit()) return fail("no limits marker should be false");
    if (!open.connectionLimitBadge().isEmpty()) return fail("no limits badge");
    if (!open.mayConnectViaApi()) return fail("open receiver must allow API connect");
    // A [0, 0] fix is the origin's "no position", not a position on null island.
    if (open.hasGps) return fail("gps [0,0] must not read as a fix");
    if (open.bandRangeLabel() != QStringLiteral("2.0 – 30.0 MHz"))
        return fail("band range label");

    // Token match, not substring: a free-form hardware descriptor that merely
    // contains the letters "limits" must not false-positive, but the real marker
    // (its own token, any case) must match.
    KiwiPublicReceiver substr;
    substr.sdrHw = QStringLiteral("KiwiSDR v1.900 NoLimitsBeta");
    if (substr.advertisesConnectionLimit())
        return fail("substring must not match the Limits token");
    KiwiPublicReceiver tok;
    tok.sdrHw = QStringLiteral("KiwiSDR v1.900 limits");
    if (!tok.advertisesConnectionLimit())
        return fail("case-insensitive Limits token must match");

    // ---- 0 < ext_api < users_max -> Limited ---------------------------
    const KiwiPublicReceiver& limited = rxs[2];
    if (limited.extApi != 4 || limited.usersMax != 8) return fail("limited parse");
    if (limited.apiPolicy() != ApiPolicy::Limited) return fail("0<ext_api<max must be Limited");
    if (!limited.mayConnectViaApi()) return fail("limited receiver must allow API connect");
    if (!limited.flagged) return fail("flagged JSON true must parse");

    // ---- ext_api key ABSENT -> Unknown --------------------------------
    // The regression test for this whole migration. Assert on extApi == -1
    // specifically: mayConnectViaApi() alone would still pass with the
    // toInt()-returns-0 bug in place, because Disabled also refuses to connect.
    const KiwiPublicReceiver& silent = rxs[3];
    if (silent.extApi != -1)
        return fail("an absent ext_api key must stay -1, NOT become 0");
    if (silent.apiPolicy() != ApiPolicy::Unknown)
        return fail("an absent ext_api key must be Unknown, not Disabled");
    if (silent.mayConnectViaApi())
        return fail("policy-unknown receiver must NOT allow API connect");
    if (!silent.apiBadge().contains(QStringLiteral("unknown")))
        return fail("policy-unknown receiver must badge as unknown");
    if (silent.hasGps || silent.hasBands) return fail("absent gps/bands must stay unset");
    if (silent.snrAll != -1 || silent.snrHf != -1) return fail("absent snr must stay -1");

    // The offer policy. This exercises KiwiPublicReceiver::offerDecision() —
    // the same function KiwiPublicReceiverPicker::onReady() switches on — so a
    // regression in the picker's filtering shows up here. (A test that
    // re-implemented the loop would keep passing after the loop broke.)
    using Offer = KiwiPublicReceiver::Offer;
    if (web.offerDecision() != Offer::HiddenWebOnly)
        return fail("ext_api=0 must be hidden as web-only");
    if (open.offerDecision() != Offer::Yes)
        return fail("an API-open receiver must be offered");
    if (limited.offerDecision() != Offer::HiddenFlagged)
        return fail("a flagged receiver must be hidden, even when API-permitted");
    if (silent.offerDecision() != Offer::HiddenPolicyUnknown)
        return fail("a policy-unknown receiver must be hidden as policy-unknown");

    // Offline outranks every other reason, so the picker's status line never
    // reports an unreachable receiver as an operator policy decision.
    KiwiPublicReceiver down = open;
    down.offline = true;
    if (down.offerDecision() != Offer::HiddenOffline)
        return fail("an offline receiver must be hidden as offline");

    int shown = 0, hiddenWebOnly = 0, hiddenUnknown = 0, hiddenFlagged = 0;
    for (const auto& r : rxs) {
        switch (r.offerDecision()) {
            case Offer::Yes:                 ++shown; break;
            case Offer::HiddenWebOnly:       ++hiddenWebOnly; break;
            case Offer::HiddenPolicyUnknown: ++hiddenUnknown; break;
            case Offer::HiddenFlagged:       ++hiddenFlagged; break;
            case Offer::HiddenOffline:       break;
        }
    }
    if (shown != 1) return fail("exactly 1 receiver should survive the picker filter");
    if (hiddenWebOnly != 1) return fail("exactly 1 web-only hidden");
    if (hiddenUnknown != 1) return fail("exactly 1 policy-unknown hidden");
    if (hiddenFlagged != 1) return fail("exactly 1 flagged hidden");

    // ---- fail closed at the boundary (Principle VII) ------------------
    if (KiwiPublicDirectory::parse(QByteArray()).ok())
        return fail("an empty body must fail, not yield an empty list");
    if (KiwiPublicDirectory::parse(QByteArrayLiteral("{\"schema\": 1, \"receivers\": [")).ok())
        return fail("a truncated body must fail cleanly, not yield a partial list");
    if (KiwiPublicDirectory::parse(QByteArrayLiteral("not json at all")).ok())
        return fail("a non-JSON body must fail");
    if (KiwiPublicDirectory::parse(QByteArrayLiteral("[]")).ok())
        return fail("a top-level array must fail (the document is an object)");
    if (KiwiPublicDirectory::parse(QByteArrayLiteral("{\"receivers\": []}")).ok())
        return fail("a body with no schema must fail");

    const KiwiDirectoryParse future = KiwiPublicDirectory::parse(
        QByteArrayLiteral("{\"schema\": 2, \"receivers\": [{\"url\": \"http://x:8073\"}]}"));
    if (future.ok())
        return fail("an unsupported schema must be rejected, not parsed hopefully");
    if (!future.receivers.isEmpty())
        return fail("a rejected schema must yield no receivers");
    if (!future.error.contains(QStringLiteral("update AetherSDR")))
        return fail("a schema mismatch must tell the user to update AetherSDR");

    // The receiver-count cap is a boundary, so exercise it rather than trusting
    // the constant: one entry past it must be refused outright, not truncated
    // to the cap and served as if it were the whole directory.
    QByteArray flood = QByteArrayLiteral("{\"schema\": 1, \"receivers\": [");
    for (int i = 0; i <= KiwiPublicDirectory::kMaxReceivers; ++i) {
        if (i) flood += ',';
        flood += QByteArrayLiteral("{\"url\":\"http://x:8073\",\"ext_api\":1,\"users_max\":1}");
    }
    flood += QByteArrayLiteral("]}");
    const KiwiDirectoryParse flooded = KiwiPublicDirectory::parse(flood);
    if (flooded.ok())
        return fail("a receiver list past kMaxReceivers must be refused");
    if (!flooded.receivers.isEmpty())
        return fail("a refused receiver list must yield no receivers, not a truncated one");

    // An empty list is empty, not malformed — the message must not claim the
    // payload was unparseable.
    const KiwiDirectoryParse empty =
        KiwiPublicDirectory::parse(QByteArrayLiteral("{\"schema\": 1, \"receivers\": []}"));
    if (empty.ok()) return fail("an empty receiver list must not report success");
    if (!empty.error.contains(QStringLiteral("empty")))
        return fail("an empty receiver list must say so, not blame the payload");

    std::printf("kiwi_public_directory_test: OK (4 parsed; web-only, "
                "policy-unknown and flagged all filtered out separately)\n");
    return 0;
}
