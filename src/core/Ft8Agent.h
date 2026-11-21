#pragma once

#include "WsprAgent.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {

struct DxSpot;

// The FT8 receiver that runs on the radio (tools/ft8-agent): a legacy
// waveform that decodes FT8 from a slice of its own and serves its spots and
// settings as JSON on TCP 48997. Built from AetherWSPR (WsprAgent.h), which
// is the template for the shim around the decoder; what differs is the
// 15-second cycle, the decoder, and a much smaller settings set.
//
// The host side shares AetherWSPR's: the request, its safety properties and
// the polling are RadioAgentClient / RadioAgentSpotFeed (RadioAgentClient.h),
// and SpotHub builds both receivers' tabs and keeps both receivers' settings
// with the same code. What is FT8's own is this file: the JSON it speaks.
//
// RECEIVE ONLY. The agent has no transmit path: its slice is set tx=0 and the
// vendored decoder's transmit-side encoder is not built. There is nothing in
// these settings that can key a radio.
namespace ft8 {
// Band hopping is NOT supported by the agent. WSPR's schedule is WSJT-X's
// two-minute slot rotation and has no FT8 meaning; #7114 leaves the question
// open rather than inventing one. A fixed band is the whole band story here.
inline constexpr bool kSupportsHopping = false;
}  // namespace ft8

// The receiver's waveform name, as its package manifest declares it.
inline constexpr const char* kFt8WaveformName = "AetherFT8";
// The port it answers on: 48994 is the radio health agent, 48995 AetherWSPR.
inline constexpr quint16 kFt8Port = 48997;

// The theme color FT8 spots take until one is chosen. Not WSPR's violet: the
// two receivers can run side by side on one panadapter.
inline constexpr const char* kFt8DefaultColorToken = "color.canon.category.teal";
// SpotHub → FT8's own settings live under this AppSettings key. They are the
// same three WSPR has (WsprSpotHubSettings, key "WsprSpots"), read and
// written by the same code: start with the radio, color, lifetime.
inline constexpr const char* kFt8SpotHubKey = "Ft8Spots";

inline bool isFt8Waveform(const QString& name)
{
    return name == QLatin1String(kFt8WaveformName);
}

// One decode.
struct Ft8Spot {
    qint64 cycleStart{0};   // UTC seconds, start of the 15-second cycle
    QString band;
    // ⚠ The agent reports ft8_lib's candidate SCORE, a correlation strength,
    // NOT a dB signal-to-noise ratio. It is deliberately not called snr:
    // plotting it beside a WSPR spot's real dB figure would be meaningless.
    int score{0};
    double dt{0.0};         // s
    qint64 frequencyHz{0};  // the transmitter's frequency
    QString call;           // the TRANSMITTING station
    QString grid;           // empty unless the message carried one
    QString message;        // as sent, e.g. "CQ DX K1ABC FN42"
};

// One decoded cycle.
struct Ft8Cycle {
    qint64 start{0};
    QString band;
    qint64 dialHz{0};
    int spots{0};
    int decodeMs{0};        // against kDecodeBudgetMs
    int audioPct{0};
    int lostPackets{0};
    double audioDbfs{0.0};
};

// An FT8 cycle is 15 s with ~12.6 s of signal; the agent captures 13 s, so
// what is left to decode in is 2 s. Measured on a FLEX-6300 this is tight:
// median 1966 ms, max 2275 ms, with 6 of 13 cycles over. A UI that shows
// decodeMs should show it against this.
inline constexpr int kCycleSeconds = 15;
inline constexpr int kDecodeBudgetMs = 2000;

// GET /v1/status.
struct Ft8Status {
    QString version;
    QString state;          // starting, connecting, waiting_clock, starting_audio, waiting_cycle, capturing, decoding
    bool clockSynced{false};
    // Which clock the agent trusts: "kernel", "radio_ntp" or "none". A
    // FLEX-6000 runs a kernel that never clears STA_UNSYNC even when chronyd
    // has disciplined the clock, so radio_ntp is normal there and is not a
    // degraded state.
    QString clockSource;
    qint64 uptimeS{0};
    QString callsign;       // in effect: the setting, or the radio's
    QString grid;
    QString band;
    qint64 dialHz{0};
    bool slice{false};
    // The radio's index for the receiver's slice, -1 while it has none or
    // the receiver is older than 0.1.3. The radio reports that slice as the
    // connected GUI client's own, so this is how AetherSDR knows to leave it
    // alone (#7139).
    int sliceIndex{-1};
    bool audio{false};
    QList<Ft8Cycle> cycles;
    QList<Ft8Spot> spots;
};

// The receiver's package, shipped inside AetherSDR so the Waveforms window
// can install it without the operator finding a file. Same shape as
// WsprPackage: the bytes are pinned by size and SHA-256 and checked before
// anything is sent to the radio, so a damaged resource installs nothing.
//
// ⚠ Rebuild with `tools/ft8-agent/build.py --armv7-only` ON LINUX, copy the
// result to resources/waveforms/, and bump the version, hash and size here
// together. A package built on Windows installs and then silently never
// runs -- only the zip container differs, and nothing reports the problem.
//
// There is no container pin yet, so FLEX-8000 and Aurora are not served by
// this installer and its menu entry is dimmed on them.
struct Ft8Package {
    struct Pin {
        const char* resource;
        const char* fileName;
        const char* sha256;
        qint64 size;
    };
    static constexpr const char* kVersion = "0.1.5";
    static constexpr Pin kLegacy{":/waveforms/AetherFT8.ssdr_waveform", "AetherFT8.ssdr_waveform",
                                 "9eb6f5a319f8646cabe8398052fe41abae69e9d14dc95a81b2877d67b2334b21", 115745};
};

// GET and POST /v1/config. A strict subset of WsprConfig: no hopping, no
// gray-line width, no decode depth, and no upload -- the agent has no
// uploader at all, because an outbound connection from the radio is its own
// new public surface (#7112).
struct Ft8Config {
    QString callsign;       // empty: the radio's
    QString grid;           // empty: the radio's GPS grid
    QString band{QStringLiteral("20m")};
    QString rxant;          // empty: the radio's default
    // Not AetherWSPR's 4: a DAX channel feeds one slice, so two receivers
    // on one default would take each other's audio.
    int daxChannel{3};

    bool operator==(const Ft8Config& o) const
    {
        return callsign == o.callsign && grid == o.grid && band == o.band
            && rxant == o.rxant && daxChannel == o.daxChannel;
    }
    bool operator!=(const Ft8Config& o) const { return !(*this == o); }
};

namespace ft8 {

// The bands the receiver knows, in band order. ⚠ These are the FT8 dial
// frequencies, not WSPR's: 20 m is 14.074, where WSPR's 20 m is 14.0956.
const QStringList& bands();

// Parse the receiver's replies. Anything malformed or out of range is
// refused (nullopt) or, for one spot or cycle, dropped.
std::optional<Ft8Status> parseStatus(const QByteArray& json);
// `claimed` (when non-null) is whether the receiver already issued an admin
// token. A fresh install has not, and the first POST claims it.
std::optional<Ft8Config> parseConfig(const QByteArray& json, bool* claimed = nullptr);
// The admin token in a POST /v1/config reply, when this was the first.
QString adminToken(const QByteArray& json);

// The body POST /v1/config takes.
QByteArray configJson(const Ft8Config& config);

// The receiver answers LAN callers only: a private, link-local or Tailscale
// (100.64.0.0/10) IPv4 address.
bool reachableAddress(const QHostAddress& address);
// The same for the connected radio. Over SmartLink its address is left from
// the last LAN session, and may be another radio's.
bool reachableRadio(bool overWan, const QHostAddress& address);

// Write the bundled package to `dir` and return its path, or empty with
// `error` set when the shipped bytes do not match the pin. Mirrors
// wspr::writeBundledPackage.
QString writeBundledPackage(const Ft8Package::Pin& pin, const QString& dir, QString* error);

// One decode as a panadapter spot.
//
// ⚠ hasSnr is FALSE, deliberately. Ft8Spot::score is ft8_lib's candidate
// correlation strength, not a dB signal-to-noise ratio, and DxSpot::snr is
// read as dB everywhere else (cluster, RBN, WSJT-X). Setting it would put a
// number on the panadapter that looks like an SNR and is not one, beside
// WSPR spots whose dB figures are real. The score goes in the comment, named.
DxSpot toDxSpot(const Ft8Spot& spot, const QString& receiver);

// The same spot as SpotHub's FT8 settings say it should look: their color
// (empty means the theme's kFt8DefaultColorToken) and their lifetime.
DxSpot styled(DxSpot spot, const WsprSpotHubSettings& settings);

// Whether the feed starts by itself when a radio connects. Reading the
// receiver is opt-in: off until Auto-Start is turned on in SpotHub, and
// never for a radio that cannot run waveforms or is reached over SmartLink.
bool feedStartsWithRadio(const WsprSpotHubSettings& settings, bool radioRunsWaveforms,
                         bool overWan, const QHostAddress& address);

// The decodes with a cycle at or after `since`, newest cycle written back to
// `next` so the caller advances. Same shape as wspr::spotsSince.
QList<Ft8Spot> spotsSince(const QList<Ft8Spot>& spots, qint64 since, qint64* next);

}  // namespace ft8
}  // namespace AetherSDR
