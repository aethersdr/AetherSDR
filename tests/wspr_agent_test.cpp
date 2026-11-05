// The WSPR receiver on the radio (tools/wspr-agent): its status and settings
// JSON parse; malformed replies are refused and out-of-range spots and
// cycles dropped; the settings body round-trips through the receiver's own
// keys; a spot becomes a SpotHub DxSpot; and only LAN or tailnet addresses
// are tried. Socket-free.

#include "core/DxSpot.h"
#include "core/WsprAgent.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

// A status reply as the receiver writes it (agent.c status_json).
const char* kStatus = R"({"agent":"AetherWSPR","version":"0.1.0","state":"capturing","clock_synced":true,
"uptime_s":1386,"callsign":"KK7GWY","grid":"DN18pg","band":"20m","dial_hz":14095600,"slice":true,
"audio":true,"hop_period":"day","depth":"standard","upload":{"enabled":true,"sent":6,"rejected":0,
"queued":0,"last_error":""},
"cycles":[{"t":1793846520,"band":"20m","dial_hz":14095600,"spots":5,"decode_ms":9810,"audio_pct":100,
"lost_packets":0,"audio_dbfs":-30.2},
{"t":1793846640,"band":"20m","dial_hz":14095600,"spots":2,"decode_ms":-5,"audio_pct":100}],
"spots":[{"t":1793846520,"band":"20m","snr":-27,"dt":0.3,"hz":14097019,"drift":0,"call":"W0ICH",
"grid":"DN70kd","dbm":37},
{"t":1793846520,"band":"20m","snr":1,"dt":-0.1,"hz":14097147,"drift":-1,"call":"","grid":"EM32re","dbm":23},
{"t":1793846520,"band":"20m","snr":400,"dt":0.3,"hz":14097019,"drift":0,"call":"X","grid":"","dbm":37},
{"t":1793846520,"band":"20m","snr":-10,"dt":0.3,"hz":14097019,"drift":0,"call":"<b>x</b>","grid":"","dbm":37}]})";

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── status ────────────────────────────────────────────────────────────
    const auto st = wspr::parseStatus(kStatus);
    check(st.has_value(), "a receiver status parses");
    if (st) {
        check(st->callsign == "KK7GWY" && st->grid == "DN18pg" && st->band == "20m"
                  && st->dialHz == 14095600 && st->hopPeriod == "day" && st->clockSynced,
              "station, band and clock come through");
        check(st->upload.enabled && st->upload.sent == 6 && st->upload.queued == 0, "upload counters come through");
        check(st->cycles.size() == 1 && st->cycles[0].spots == 5 && st->cycles[0].audioDbfs < -30,
              "a cycle with a negative decode time is dropped");
        check(st->spots.size() == 2, "spots with an impossible SNR or a callsign that isn't one are dropped");
        check(st->spots.size() == 2 && st->spots[1].call.isEmpty() && st->spots[1].grid == "EM32re",
              "a hashed call (empty) is kept with its grid");
    }
    check(!wspr::parseStatus("not json"), "non-JSON is refused");
    check(!wspr::parseStatus(R"({"agent":"AetherHealth"})"), "another agent's JSON is refused");
    check(!wspr::parseStatus(R"({"agent":"AetherWSPR","callsign":"bad call","grid":"","band":"20m"})"),
          "a status with a malformed callsign is refused");
    check(!wspr::parseStatus(R"({"agent":"AetherWSPR","callsign":"","grid":"","band":"11m"})"),
          "a status on a band the receiver doesn't have is refused");
    {
        QByteArray many = R"({"agent":"AetherWSPR","callsign":"","grid":"","band":"","spots":[)";
        for (int i = 0; i < 5001; ++i) {
            many += i ? "," : "";
            many += R"({"t":1,"band":"","snr":0,"dt":0,"hz":14097000,"drift":0,"call":"","grid":"","dbm":30})";
        }
        many += "]}";
        check(!wspr::parseStatus(many), "more spots than the receiver ever keeps is refused");
    }

    // ── settings ──────────────────────────────────────────────────────────
    WsprConfig c;
    c.callsign = "KK7GWY";
    c.grid = "DN18";
    c.band = "30m";
    c.hopping = true;
    c.grayMinutes = 45;
    c.hop[WsprConfig::Day] = QStringList{"6m", "20m"};
    c.hop[WsprConfig::Night] = QStringList{"80m"};
    c.depth = "deep";
    c.upload = true;
    c.daxChannel = 3;
    const QByteArray body = wspr::configJson(c);
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    check(o.value("hop_day").toArray().at(0).toString() == "20m", "hop bands go out in band order");
    check(o.value("hop_sunrise").isArray() && o.value("hop_sunrise").toArray().isEmpty(),
          "an empty period is sent as an empty list");
    check(o.keys().size() == 13, "the body has exactly the receiver's thirteen keys");
    {
        QJsonObject reply;
        reply.insert("config", o);
        reply.insert("claimed", true);
        bool claimed = false;
        const auto back = wspr::parseConfig(QJsonDocument(reply).toJson(), &claimed);
        WsprConfig expect = c;
        expect.hop[WsprConfig::Day] = QStringList{"20m", "6m"};
        check(back && *back == expect && claimed, "settings round-trip through the receiver's keys");
    }
    check(!wspr::parseConfig(R"({"config":{"band":"20m","depth":"max","gray_minutes":60,"dax_channel":4}})"),
          "an unknown depth is refused");
    check(!wspr::parseConfig(R"({"config":{"band":"20m","depth":"deep","gray_minutes":0,"dax_channel":4}})"),
          "a gray time outside 1-120 is refused");
    check(!wspr::parseConfig(R"({"config":{"band":"20m","depth":"deep","gray_minutes":60,"dax_channel":4,
          "hop_day":["11m"]}})"),
          "a hop band the receiver doesn't have is refused");
    check(wspr::adminToken(R"({"admin_token":"0123456789abcdef0123456789abcdef"})")
              == "0123456789abcdef0123456789abcdef",
          "the first save's admin token is read");
    check(wspr::adminToken(R"({"admin_token":"short"})").isEmpty(), "a malformed token is ignored");

    // ── SpotHub spot ──────────────────────────────────────────────────────
    if (st && !st->spots.isEmpty()) {
        const DxSpot d = wspr::toDxSpot(st->spots[0], st->callsign);
        check(d.dxCall == "W0ICH" && d.spotterCall == "KK7GWY" && d.source == "WSPR"
                  && qAbs(d.freqMhz - 14.097019) < 1e-9 && d.hasSnr && d.snr == -27 && d.dxGrid == "DN70"
                  && d.comment == "WSPR -27 dB 37 dBm" && d.utcTime.toString("HHmm") == "0242",
              "a spot becomes a SpotHub DxSpot");
    }
    check(wspr::distanceKm("DN18pg", "DN70kd") > 1200 && wspr::distanceKm("DN18pg", "DN70kd") < 1350,
          "distance from the station's grid");
    check(wspr::distanceKm("DN18pg", "") == -1, "no distance without a grid");

    // ── addresses ─────────────────────────────────────────────────────────
    check(wspr::reachableAddress(QHostAddress("192.168.50.100")), "a LAN radio is reachable");
    check(wspr::reachableAddress(QHostAddress("100.114.103.82")), "a tailnet shim address is reachable");
    check(!wspr::reachableAddress(QHostAddress("8.8.8.8")), "a public address is not tried");
    check(!wspr::reachableAddress(QHostAddress()), "no address is not tried");

    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
