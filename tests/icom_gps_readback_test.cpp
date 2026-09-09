// Socket-free injected-transport coverage for the IC-705 GPS and NTP read-back
// decode in IcomCivBackend::onCivFrame. The retired fake-radio peer used to
// carry these assertions; feeding the frame handler directly is the layer
// AGENTS.md routes "what the client publishes for a given reply" to.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/aprs/AprsPacket.h"

#include <QCoreApplication>
#include <cmath>
#include <cstdio>
#include <vector>

using AetherSDR::GpsDelta;
using AetherSDR::icom::CivFrame;
using AetherSDR::icom::IcomCivBackend;
using AetherSDR::icom::modelForName;
namespace cmd = AetherSDR::icom::cmd;
namespace gps = AetherSDR::icom::gps;
namespace settingSub = AetherSDR::icom::settingSub;

namespace AetherSDR::icom {

struct IcomCivBackendTestAccess {
    static void prime(IcomCivBackend& backend, const char* model)
    {
        backend.m_model = modelForName(model);
        backend.m_connected = true;
    }

    static void inject(IcomCivBackend& backend, const CivFrame& frame)
    {
        backend.onCivFrame(frame, backend.m_sessionGeneration);
    }
};

}  // namespace AetherSDR::icom

using AetherSDR::icom::IcomCivBackendTestAccess;

namespace {
int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

CivFrame frame(std::uint8_t command, std::uint8_t sub, std::vector<std::uint8_t> data)
{
    CivFrame f;
    f.to = 0xE0;
    f.from = 0xA4;
    f.cmd = command;
    f.hasSub = true;
    f.sub = sub;
    f.data = std::move(data);
    return f;
}

// 1A 05 <item> <value…> with the item in two BCD bytes.
CivFrame menuFrame(int item, std::vector<std::uint8_t> value)
{
    std::vector<std::uint8_t> data{
        static_cast<std::uint8_t>(((item / 1000) << 4) | ((item / 100) % 10)),
        static_cast<std::uint8_t>((((item / 10) % 10) << 4) | (item % 10))};
    data.insert(data.end(), value.begin(), value.end());
    return frame(cmd::kSetting, settingSub::kMenu, std::move(data));
}

// Official IC-705 CI-V guide layout: N 34 13.464, W 118 03.534, 1742.5 m,
// 275 deg, 42.7 km/h, 2026-08-21 12:34:56 UTC.
const std::vector<std::uint8_t> kFix{
    0x34, 0x13, 0x46, 0x40, 0x01,
    0x01, 0x18, 0x03, 0x53, 0x40, 0x00,
    0x01, 0x74, 0x25, 0x00,
    0x02, 0x75,
    0x00, 0x04, 0x27,
    0x20, 0x26, 0x08, 0x21, 0x12, 0x34, 0x56,
};

struct Merged {
    GpsDelta state;
    int emissions = 0;
    void apply(const GpsDelta& d)
    {
        ++emissions;
        if (d.status) state.status = d.status;
        if (d.positionValid) state.positionValid = d.positionValid;
        if (d.source) state.source = d.source;
        if (d.grid) state.grid = d.grid;
        if (d.altitude) state.altitude = d.altitude;
        if (d.lat) state.lat = d.lat;
        if (d.lon) state.lon = d.lon;
        if (d.time) state.time = d.time;
        if (d.date) state.date = d.date;
        if (d.speed) state.speed = d.speed;
        if (d.track) state.track = d.track;
        if (d.ntpEnabled) state.ntpEnabled = d.ntpEnabled;
        if (d.ntpServer) state.ntpServer = d.ntpServer;
        if (d.gpsTimeCorrectionEnabled) {
            state.gpsTimeCorrectionEnabled = d.gpsTimeCorrectionEnabled;
        }
        if (d.ntpSyncStatus) state.ntpSyncStatus = d.ntpSyncStatus;
    }
};
}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        IcomCivBackend backend;
        Merged m;
        QObject::connect(&backend, &IcomCivBackend::gpsChanged,
                         [&m](const GpsDelta& d) { m.apply(d); });
        IcomCivBackendTestAccess::prime(backend, "IC-705");

        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kSource, {0x01}));
        check(m.state.source.value_or(QString{}) == QStringLiteral("Internal GPS")
                  && m.state.status.value_or(QString{}) == QStringLiteral("Waiting for position"),
              "23 01 = 01 publishes the internal receiver as source, no position yet");

        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kPosition, kFix));
        check(m.state.positionValid.value_or(false)
                  && m.state.status.value_or(QString{}) == QStringLiteral("Position reported"),
              "a decodable 23 00 reply publishes a usable position without inventing a lock");
        check(m.state.lat.value_or(QString{}) == QStringLiteral("N 34 13.464")
                  && m.state.lon.value_or(QString{}) == QStringLiteral("W 118 3.534"),
              "coordinates are published in the radio-format text the GUI parsers accept");
        double lat = 0.0;
        double lon = 0.0;
        check(AetherSDR::aprs::parseGpsCoordinate(m.state.lat.value_or(QString{}), lat)
                  && AetherSDR::aprs::parseGpsCoordinate(m.state.lon.value_or(QString{}), lon)
                  && std::abs(lat - 34.2244) < 0.0001 && std::abs(lon + 118.0589) < 0.0001,
              "the published text round-trips through aprs::parseGpsCoordinate");
        check(m.state.grid.value_or(QString{})
                  == AetherSDR::aprs::gridSquare(34.2244, -118.0589),
              "the Maidenhead grid is derived locally from the decoded fix");
        check(m.state.altitude.value_or(QString{}) == QStringLiteral("1742.5 m")
                  && m.state.speed.value_or(QString{}) == QStringLiteral("42.7 km/h")
                  && m.state.track.value_or(QString{}) == QStringLiteral("275 deg"),
              "altitude, speed and course carry their units");
        check(m.state.date.value_or(QString{}) == QStringLiteral("2026-08-21")
                  && m.state.time.value_or(QString{}) == QStringLiteral("12:34:56Z"),
              "the complete radio UTC is split into ISO date and Z-suffixed time");

        const int beforeSourceReread = m.emissions;
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kSource, {0x01}));
        check(m.emissions == beforeSourceReread + 1
                  && m.state.status.value_or(QString{}) == QStringLiteral("Position reported"),
              "the periodic 23 01 re-read does not overwrite a live position status");

        std::vector<std::uint8_t> noFix(27, 0xFF);
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kPosition, noFix));
        check(!m.state.positionValid.value_or(true)
                  && m.state.grid.value_or(QStringLiteral("x")).isEmpty()
                  && m.state.lat.value_or(QStringLiteral("x")).isEmpty()
                  && m.state.date.value_or(QStringLiteral("x")).isEmpty()
                  && m.state.status.value_or(QString{}) == QStringLiteral("No position data"),
              "an all-FF reply clears every readout rather than leaving the last fix");

        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kPosition, kFix));
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kSource, {0x00}));
        check(!m.state.positionValid.value_or(true)
                  && m.state.source.value_or(QString{}) == QStringLiteral("Off")
                  && m.state.status.value_or(QString{}) == QStringLiteral("GPS off")
                  && m.state.lat.value_or(QStringLiteral("x")).isEmpty(),
              "switching the source off clears the position the radio no longer owns");

        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kSource, {0x02}));
        check(m.state.source.value_or(QString{}) == QStringLiteral("Off"),
              "an undocumented source value is ignored rather than published");

        // NTP / clock SET items, 0168 as the fixed-width NUL-padded field the
        // real radio returns.
        std::vector<std::uint8_t> server(64, 0x00);
        const char* host = "time.nist.gov";
        for (std::size_t i = 0; host[i] != '\0'; ++i) {
            server[i] = static_cast<std::uint8_t>(host[i]);
        }
        const int before = m.emissions;
        IcomCivBackendTestAccess::inject(backend, menuFrame(168, server));
        check(m.state.ntpServer.value_or(QString{}) == QStringLiteral("time.nist.gov")
                  && m.emissions == before + 1,
              "the NUL-padded 0168 field is published as the bare hostname");

        std::vector<std::uint8_t> hostPort{'h', 'o', 's', 't', ':', '1', '2', '3', 0x20, 0x20};
        IcomCivBackendTestAccess::inject(backend, menuFrame(168, hostPort));
        check(m.state.ntpServer.value_or(QString{}) == QStringLiteral("host:123"),
              "a radio-stored value outside our write alphabet is still published, padding stripped");
        const int beforeBad = m.emissions;
        std::vector<std::uint8_t> unprintable{'h', 0x01, 's', 't'};
        IcomCivBackendTestAccess::inject(backend, menuFrame(168, unprintable));
        check(m.state.ntpServer.value_or(QString{}) == QStringLiteral("host:123")
                  && m.emissions == beforeBad,
              "an unprintable read-back is dropped, not published");

        IcomCivBackendTestAccess::inject(backend, menuFrame(167, {0x01}));
        IcomCivBackendTestAccess::inject(backend, menuFrame(169, {0x00}));
        check(m.state.ntpEnabled.value_or(false)
                  && m.state.gpsTimeCorrectionEnabled.has_value()
                  && !*m.state.gpsTimeCorrectionEnabled,
              "0167 and 0169 read-backs publish the radio's NTP and Time Correct state");
        IcomCivBackendTestAccess::inject(backend, menuFrame(167, {0x05}));
        check(m.state.ntpEnabled.value_or(false),
              "an out-of-range 0167 value does not flip the published state");

        IcomCivBackendTestAccess::inject(backend, frame(cmd::kSetting, settingSub::kNtpResult, {0x01}));
        check(m.state.ntpSyncStatus.value_or(QString{}) == QStringLiteral("Succeeded"),
              "1A 08 = 01 publishes Succeeded");
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kSetting, settingSub::kNtpResult, {0x02}));
        check(m.state.ntpSyncStatus.value_or(QString{}) == QStringLiteral("Failed"),
              "1A 08 = 02 publishes Failed");
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kSetting, settingSub::kNtpResult, {0x00}));
        check(m.state.ntpSyncStatus.value_or(QString{}) == QStringLiteral("Not accessed"),
              "1A 08 = 00 with no access in flight publishes Not accessed");
        const int beforeResult = m.emissions;
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kSetting, settingSub::kNtpResult, {0x07}));
        check(m.emissions == beforeResult, "an undocumented 1A 08 value is ignored");
    }

    {
        // Another Icom must not inherit the IC-705 command truth by position.
        IcomCivBackend backend;
        Merged m;
        QObject::connect(&backend, &IcomCivBackend::gpsChanged,
                         [&m](const GpsDelta& d) { m.apply(d); });
        IcomCivBackendTestAccess::prime(backend, "IC-9700");
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kPosition, kFix));
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kGps, gps::kSource, {0x01}));
        IcomCivBackendTestAccess::inject(backend, frame(cmd::kSetting, settingSub::kNtpResult, {0x01}));
        check(m.emissions == 0, "the IC-9700 profile publishes no GPS or NTP state from these frames");
    }

    return g_failures == 0 ? 0 : 1;
}
