#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// VK3AMP RF amplifier protocol.
//
// Protocol authority: NOT a manufacturer spec (unlike ACOM's, see
// AcomProtocol.h) -- reverse-engineered from packet captures against real
// hardware by a companion, unaffiliated project (vkamp_client.py), whose
// own comments tag each field/command with its confirmation level. Treat
// "confirmed" claims here as confirmed against the specific unit(s) that
// project tested, not guaranteed identical across every VK3AMP/TCI_VKAMP-
// family amp in the field. See docs/architecture/vkamp-amplifier-design.md
// for the full design note, including why this codebase deliberately does
// NOT carry over that project's decompile-derived fault-name table
// (Constitution Principle IV) -- error codes surface as a bare number here.
//
// Three wire formats, not one (design doc Section 3):
//   - TCP control/status (port 5005): ASCII CSV, request/response -- the amp
//     does not broadcast spontaneously while idle.
//   - UDP telemetry (port 5010): ASCII CSV, TX-gated, needs a periodic "11"
//     retrigger to keep flowing.
//   - Serial (COM port): a different, undocumented-here 15-byte binary frame
//     -- out of scope for this v1 (TCP+UDP only); see design doc Section 3.3.
namespace Vkamp {

// VK3AMP ships in three rated-power classes. The wire protocol has no
// model/wattage field to auto-detect this from (design doc Section 5 --
// written before this variant table existed, back when only the 2000W unit
// had been tested), so the operator picks one in Peripherals settings
// (PeripheralSettings "Vkamp"/"Variant") and it's threaded down to
// VkampApplet::setVariant(). Defaults to W2000, the originally-confirmed
// unit.
enum class Variant { W600, W1000, W2000 };

// Rated (nameplate) output for the variant -- also where the forward-power
// gauge's red zone begins.
float ratedWatts(Variant v);
// Forward-power gauge full-scale: rated output plus ~25% headroom so a
// carrier briefly over the nameplate rating is still visible on-scale
// instead of pinning. Confirmed against real hardware only for W2000
// (2000W rated -> 2500W full-scale); W600/W1000 apply the same ratio
// pending their own confirmation.
float meterFullScaleWatts(Variant v);
// UI label, e.g. "2000 W".
QString variantLabel(Variant v);

// f1/band decode table (design doc Section 3.1) -- codes assigned in
// straight ascending numeric order to bands in straight descending-
// wavelength order. Two relay-group pairs (17-15, 12-10) share one code
// each; all 8 codes are confirmed, no gaps. Read-only from this protocol --
// there is no command that sets it (the amp senses it via CAT-follow or the
// physical front panel). Empty string for any other code.
QString bandName(int f1);

// ── Status (TCP, request/response) ──────────────────────────────────────

struct Status {
    int   temp_c{0};
    float volts{0.0f};
    int   band{0};              // f1 field -- see bandName() and its own doc comment
    int   antenna{0};           // 1-3, direct antenna-relay select
    int   error_code{0};        // raw numeric code only, 0 = no fault -- see this file's own doc comment
    bool  tx{false};
    bool  cooling_override{false};
    bool  bypass{false};

    // Inferred, not a protocol bit -- see design doc Section 3.1/Section 5.
    // Bypass produces a distinct ~6.3V standby reading that is neither rail
    // target; callers must gate on !bypass before trusting this value
    // (VkampConnection does, before ever surfacing it to the GUI).
    bool voltageLow() const;
};

// Parses one status broadcast in isolation -- StatusStreamParser below is
// what VkampConnection actually feeds a live byte stream through; this is
// the unit-test entry point. Returns nullopt if `text` doesn't match the
// CSV shape.
std::optional<Status> parseStatus(const QByteArray& text);

// Streaming parser mirroring the companion Python client's own hard-won
// buffer-trim behavior: every candidate match in the fed bytes fires the
// callback, then only the bytes AFTER THE LAST match are kept for the next
// feed() -- re-scanning a growing buffer from scratch on every feed() was an
// earlier, since-fixed bug in that project (see design doc Section 1's own
// provenance note on this project's methodology). If a feed() produces no
// match at all, the buffer is capped rather than left to grow unboundedly
// against a non-matching/garbage stream (same fallback the companion
// project's own serial-status parser uses).
class StatusStreamParser {
public:
    void setStatusCallback(std::function<void(const Status&)> cb) { m_onStatus = std::move(cb); }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

private:
    static constexpr int kMaxBufferBytes = 4096;

    QByteArray m_buf;
    std::function<void(const Status&)> m_onStatus;
};

// ── Telemetry (UDP, TX-gated) ─────────────────────────────────────────────

struct Telemetry {
    int output{0};
    int reflected{0};
    int current{0};
    int input_raw{0};

    // Calibrated values -- quadratic (output/reflected/input) or linear
    // (current) fits against external reference measurements, ported from
    // the companion project's own calibration (design doc Section 3.2).
    //
    // The three quadratics share one out-of-range rule: the fit is followed
    // only where it is monotonically increasing, and below its own vertex it
    // tapers linearly to 0W at 0 raw counts. calibratedPower() in the .cpp
    // carries the reasoning -- in short, a decreasing power curve is the fit
    // telling you it has left the data it was built from, and scaling a real
    // reading by a fraction of itself never made that reading truer. The
    // consequence that matters at call sites: 0 raw counts really does mean
    // 0W on every curve, so "no carrier" needs no special case anywhere.
    float outputWatts() const;
    float reflectedWatts() const;
    float currentAmps() const;
    float inputWatts() const;
    // Computed client-side, not a wire field -- rho = sqrt(reflected/output),
    // SWR = (1+rho)/(1-rho). Returns 1.0 when there is no calibrated forward
    // power to divide by, matching the amp's own idle display. A genuinely
    // matched load reaches 1.0 through the arithmetic rather than through a
    // guard, since reflectedWatts() reads a true 0 at 0 raw counts.
    float swr() const;
};

// `payload` is the raw UDP datagram bytes -- this strips at the first NUL
// (the companion project's own observed framing) and requires exactly 4
// comma-separated integer fields. Returns nullopt otherwise.
std::optional<Telemetry> parseTelemetry(const QByteArray& payload);

// ── Commands (host -> amp) ───────────────────────────────────────────────
// All bare 2-digit ASCII, no terminator (design doc Section 3.1).

QByteArray buildBypass(bool on);
QByteArray buildCooling(bool on);
// Two fixed rail setpoints, NOT a continuous dial -- see design doc
// Section 5. Callers must never expose this as a slider/numeric entry.
QByteArray buildVoltage(bool low);
// `port` must be 1-3 -- caller's responsibility to range-check (design doc
// Section 3.1: the amp silently no-ops for 4-8, this layer doesn't guard it
// since VkampConnection already does before calling here).
QByteArray buildSelectAntenna(int port);
// "11" -- doubles as the UDP telemetry trigger/keepalive AND the TCP idle
// keepalive ping (design doc Section 6): it is the only thing that produces
// a status reply during genuine idle silence.
QByteArray buildPoll();
// One "23" send -- the amp needs this repeated for ~9-12s to actually
// register (a hold-to-confirm pattern); VkampConnection owns the repeat
// timing, not this layer.
QByteArray buildResetHold();

}  // namespace Vkamp
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Vkamp::Status)
Q_DECLARE_METATYPE(AetherSDR::Vkamp::Telemetry)
