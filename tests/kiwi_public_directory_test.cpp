#include "core/KiwiPublicDirectory.h"

#include <cstdio>

using AetherSDR::KiwiPublicDirectory;
using AetherSDR::KiwiPublicReceiver;
using ApiPolicy = AetherSDR::KiwiPublicReceiver::ApiPolicy;

namespace {

int fail(const char* msg)
{
    std::fprintf(stderr, "kiwi_public_directory_test: %s\n", msg);
    return 1;
}

// A faithful slice of kiwisdr.com/public in the real on-the-wire format:
// each receiver is an <a href='host'> anchor followed by <!-- key=value -->
// comments, up to the next anchor. Includes a non-receiver nav anchor that
// must be ignored, and the three ext_api regimes.
const QByteArray kSample = QByteArrayLiteral(
    "<a href='/about'>nav link (no receiver fields) should be skipped</a>\n"
    // ext_api=0 -> web-only (operator disabled the external API)
    "<a href='http://g3sdr.com:8073' target='_blank'> <img src='a.png'> </a>\n"
    "  <!-- id=c4f312a0a6ef -->\n"
    "  <!-- status=active -->\n"
    "  <!-- offline=no -->\n"
    "  <!-- name=G3SDR, Weston-super-Mare -->\n"
    "  <!-- sdr_hw=KiwiSDR 1 v1.842 Limits -->\n"
    "  <!-- bands=0-30000000 -->\n"
    "  <!-- users=3 -->\n"
    "  <!-- users_max=4 -->\n"
    "  <!-- ext_api=0 -->\n"
    "  <!-- loc=Weston-super-Mare, United Kingdom -->\n"
    "  <!-- antenna=Parallel dipoles -->\n"
    // ext_api == users_max -> fully open to API
    "<a href='http://open.example.com:8073' target='_blank'> </a>\n"
    "  <!-- name=Open RX -->\n"
    "  <!-- sdr_hw=KiwiSDR 1 v1.842 -->\n"
    "  <!-- users=1 -->\n"
    "  <!-- users_max=4 -->\n"
    "  <!-- ext_api=4 -->\n"
    "  <!-- loc=Somewhere -->\n"
    // 0 < ext_api < users_max -> limited (channels reserved for web)
    "<a href='http://limited.example.com:8074' target='_blank'> </a>\n"
    "  <!-- name=Limited RX -->\n"
    "  <!-- users=2 -->\n"
    "  <!-- users_max=8 -->\n"
    "  <!-- ext_api=4 -->\n"
    "  <!-- loc=Elsewhere -->\n");

} // namespace

int main()
{
    const auto rxs = KiwiPublicDirectory::parse(kSample);

    // The nav anchor (no name/users_max) is skipped; 3 real receivers remain.
    if (rxs.size() != 3)
        return fail("expected 3 receivers (nav anchor must be skipped)");

    const KiwiPublicReceiver& web = rxs[0];
    if (web.url != QStringLiteral("http://g3sdr.com:8073")) return fail("url parse");
    if (web.name != QStringLiteral("G3SDR, Weston-super-Mare")) return fail("name parse");
    if (web.location != QStringLiteral("Weston-super-Mare, United Kingdom")) return fail("loc parse");
    if (web.users != 3 || web.usersMax != 4) return fail("users/users_max parse");
    if (web.extApi != 0) return fail("ext_api parse");
    if (web.apiPolicy() != ApiPolicy::Disabled) return fail("ext_api=0 must be Disabled");
    if (!web.advertisesConnectionLimit()) return fail("limits marker parse");
    if (web.connectionLimitBadge() != QStringLiteral("Limits")) return fail("limits badge");
    // THE honor guarantee: a web-only receiver is never API-connectable.
    if (web.mayConnectViaApi()) return fail("ext_api=0 must NOT allow API connect");

    const KiwiPublicReceiver& open = rxs[1];
    if (open.extApi != 4 || open.usersMax != 4) return fail("open parse");
    if (open.apiPolicy() != ApiPolicy::Open) return fail("ext_api==users_max must be Open");
    if (open.advertisesConnectionLimit()) return fail("no limits marker should be false");
    if (!open.connectionLimitBadge().isEmpty()) return fail("no limits badge");
    if (!open.mayConnectViaApi()) return fail("open receiver must allow API connect");

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

    const KiwiPublicReceiver& limited = rxs[2];
    if (limited.extApi != 4 || limited.usersMax != 8) return fail("limited parse");
    if (limited.apiPolicy() != ApiPolicy::Limited) return fail("0<ext_api<max must be Limited");
    if (!limited.mayConnectViaApi()) return fail("limited receiver must allow API connect");

    // The picker filter: only API-permitted receivers are shown.
    int shown = 0;
    for (const auto& r : rxs)
        if (r.mayConnectViaApi()) ++shown;
    if (shown != 2) return fail("exactly 2 of 3 should pass the API-only filter");

    std::printf("kiwi_public_directory_test: OK (3 parsed, 1 web-only filtered out)\n");
    return 0;
}
