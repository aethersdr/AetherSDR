#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {

struct DxSpot;

// The WSPR receiver that runs on the radio (tools/wspr-agent): a legacy
// waveform that decodes WSPR from a slice of its own and serves its spots
// and settings as JSON on TCP 48995. It ships inside AetherSDR as Qt
// resources, a package for each kind of radio, and is installed only if the
// bytes match the pin. `tools/wspr-agent/build.py` rebuilds both identical
// files with zig 0.17; copy them to resources/waveforms/ and bump all of
// this together.
struct WsprPackage {
    struct Pin {
        const char* resource;
        const char* fileName;
        const char* sha256;
        qint64 size;
    };
    static constexpr const char* kVersion = "0.1.7";
    // FLEX-6000: a legacy waveform, whose settings persist on /user.
    static constexpr Pin kLegacy{":/waveforms/AetherWSPR.ssdr_waveform", "AetherWSPR.ssdr_waveform",
                                 "b590bf70550ffca1d2bfa63b088bf2360003583f0db1857742a5f19f2e150ca9", 142256};
    // FLEX-8000 and Aurora: a container, because a legacy waveform has only
    // RAM there and the container's writable layer survives a reboot.
    static constexpr Pin kContainer{":/waveforms/AetherWSPR.tar.gz", "AetherWSPR.tar.gz",
                                    "3789ab5242ff6391ffa910c9012063d782e9f2a76676b45c34b4b52452fa71d1", 155772};
};

// The receiver's waveform name, in both packages.
inline constexpr const char* kWsprWaveformName = "AetherWSPR";
// The port it answers on: at the radio's LAN address, or at the tailnet
// shim's address when the shim publishes it.
inline constexpr quint16 kWsprPort = 48995;

inline bool isWsprWaveform(const QString& name)
{
    return name == QLatin1String(kWsprWaveformName);
}

// One decode.
struct WsprSpot {
    qint64 cycleStart{0};   // UTC seconds, start of the two-minute cycle
    QString band;
    int snr{0};             // dB in 2500 Hz
    double dt{0.0};         // s
    qint64 frequencyHz{0};  // the transmitter's frequency
    int drift{0};           // Hz per minute
    QString call;           // empty for an unresolved hashed call
    QString grid;
    int dbm{0};
};

// One decoded (or skipped) cycle.
struct WsprCycle {
    qint64 start{0};
    QString band;
    qint64 dialHz{0};
    int spots{0};
    int decodeMs{0};
    int audioPct{0};
    int lostPackets{0};
    double audioDbfs{0.0};
};

// GET /v1/status.
struct WsprStatus {
    QString version;
    QString state;          // starting, connecting, waiting_clock, starting_audio, waiting_cycle, capturing, decoding
    bool clockSynced{false};
    qint64 uptimeS{0};
    QString callsign;       // in effect: the setting, or the radio's
    QString grid;           // in effect: the setting, or the radio's GPS grid
    QString band;
    qint64 dialHz{0};
    QString hopPeriod;      // sunrise, day, sunset, night; empty when not hopping
    QString depth;
    bool slice{false};
    bool audio{false};
    struct Upload {
        bool enabled{false};
        qint64 sent{0};
        qint64 rejected{0};
        int queued{0};
        QString lastError;
    } upload;
    QList<WsprCycle> cycles;
    QList<WsprSpot> spots;
};

// GET and POST /v1/config. WSJT-X's band hopping: a band list for each
// time-of-day period.
struct WsprConfig {
    enum Period { Sunrise, Day, Sunset, Night, PeriodCount };
    QString callsign;       // empty: the radio's
    QString grid;           // empty: the radio's GPS grid
    QString band{QStringLiteral("20m")};
    bool hopping{false};
    int grayMinutes{60};
    QStringList hop[PeriodCount];
    QString depth{QStringLiteral("standard")};
    QString rxant;          // empty: the radio's default
    bool upload{false};
    int daxChannel{4};

    bool operator==(const WsprConfig& o) const;
    bool operator!=(const WsprConfig& o) const { return !(*this == o); }
};

// The theme color WSPR spots take until one is chosen.
inline constexpr const char* kWsprDefaultColorToken = "color.canon.category.violet";

// SpotHub → WSPR's own settings, one object under AppSettings key
// "WsprSpots" (Principle V): whether reading starts when a radio with the
// receiver connects, and how its spots look on the panadapter.
struct WsprSpotHubSettings {
    bool autoStart{false};
    QString color;          // empty: kWsprDefaultColorToken
    int lifetimeMinutes{30};

    bool operator==(const WsprSpotHubSettings& o) const
    {
        return autoStart == o.autoStart && color == o.color && lifetimeMinutes == o.lifetimeMinutes;
    }
};

namespace wspr {

// The bands the receiver knows, in band order, and WSJT-X's ten coordinated
// hopping bands.
const QStringList& bands();
const QStringList& coordinatedBands();
// The depth names the receiver takes, in menu order (standard first).
const QStringList& depths();
// The JSON key for a hopping period (hop_sunrise ...).
QString periodKey(WsprConfig::Period period);

// Parse the receiver's replies. Anything malformed or out of range is
// refused (nullopt) or, for one spot or cycle, dropped.
std::optional<WsprStatus> parseStatus(const QByteArray& json);
// `claimed` (when non-null) is whether the receiver already issued an
// admin token.
std::optional<WsprConfig> parseConfig(const QByteArray& json, bool* claimed = nullptr);
// The admin token in a POST /v1/config reply, when this was the first.
QString adminToken(const QByteArray& json);

// The body POST /v1/config takes.
QByteArray configJson(const WsprConfig& config);

// A spot for SpotHub and the panadapter: the station heard, its frequency,
// the receiving station as spotter, "WSPR -21 dB 37 dBm" as the comment.
DxSpot toDxSpot(const WsprSpot& spot, const QString& receiver);

// Great-circle distance in km between two grids; -1 when either is unknown.
int distanceKm(const QString& fromGrid, const QString& toGrid);

// The spots from cycle `since` on, and in `next` where the next read should
// start: after the newest cycle among them, or `since` if there are none. A
// cycle's spots arrive together, once it has decoded.
QList<WsprSpot> spotsSince(const QList<WsprSpot>& spots, qint64 since, qint64* next);

// SpotHub settings to and from their stored JSON; anything missing or out
// of range takes its default.
WsprSpotHubSettings parseSpotHubSettings(const QString& json);
QString serializeSpotHubSettings(const WsprSpotHubSettings& settings);

// The receiver answers LAN callers, and a tailnet shim publishes it: a
// private, link-local or Tailscale (100.64.0.0/10) IPv4 address.
bool reachableAddress(const QHostAddress& address);
// The same for the connected radio. Over SmartLink its address is left from
// the last LAN session, and may be another radio's.
bool reachableRadio(bool overWan, const QHostAddress& address);

// Writes a bundled package to `dir` for the installer, reusing a matching
// copy already there. Only bytes matching the pin are written: empty, with
// `error` set, otherwise.
QString writeBundledPackage(const WsprPackage::Pin& pin, const QString& dir, QString* error);

// Whether a receiver reporting `installed` is older than the one bundled
// (WsprPackage::kVersion). Unreadable versions are never older.
bool updateAvailable(const QString& installed);

}  // namespace wspr
}  // namespace AetherSDR
