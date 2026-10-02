#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// TelePost LP-100A RF wattmeter serial protocol. Field order and semantics:
// "LP-100A Digital Vector RF Wattmeter Operations Manual" pp. 20-21
// (telepostinc.com/LP-100A-Op_Manual.pdf). Field widths and framing: a hardware
// capture (954 records), since the manual's example is wrong (see
// kRecordLength). Provenance: docs/architecture/lp-100a-wattmeter-design.md.
// Link: 115200 8N1, no handshake. Firmware < 1.2.0.0 (38400) and < 1.0.3 (19200,
// no dBm/SWR) are unsupported; their records are simply rejected.
namespace LpMeter {

// Commands (host -> meter): the entire set, one byte each. All but Poll are
// INCREMENTS with no absolute form and no ack, so report the next reading, never
// what was commanded. Single-byte commands can't interleave on a shared ser2net
// port, which is why PollGate can be advisory.
constexpr char kPollCommand      = 'P';  // request one reading
constexpr char kAlarmCommand     = 'A';  // increment SWR alarm set point
constexpr char kModeCommand      = 'M';  // increment mode / power range
constexpr char kPeakAvgCommand   = 'F';  // cycle Peak / Avg / Tune

// ---- Record framing ------------------------------------------------------

// ';' LEADS a record; it is not a terminator. The meter emits no CR, LF or
// any other terminator at all -- confirmed by capture, and consistent with
// both transports in the reference Node-RED flow framing by timing rather
// than by a delimiter.
constexpr char kRecordMarker = ';';

// Body length after the marker, measured: the manual's example
//     ;1457.00,49.3,005.0,2,N8LP  ,0,2,61.6,1.02
// is 41 chars because its Z field is 4 wide; the wire sends 5 ("046.3"), and all
// 954 captured records were 42. A SANITY CHECK only (see looksLikeRecord()): a
// 5-char dBm value (-99.9..-10.0, signed and unpadded) would make 43.
constexpr int kRecordLength = 42;

// Field indices within the comma-separated record.
enum Field {
    FieldPower      = 0,  // W,     7 wide, zero-padded
    FieldZ          = 1,  // ohms,  5 wide, zero-padded, MAGNITUDE only
    FieldPhase      = 2,  // deg,   5 wide, zero-padded, MAGNITUDE only (see below)
    FieldAlarm      = 3,  // enum,  1 wide
    FieldCallsign   = 4,  //        6 wide, space-padded
    FieldRange      = 5,  // enum,  1 wide
    FieldPeakHold   = 6,  // enum,  1 wide
    FieldDbm        = 7,  // dBm,   4 wide, SIGNED, not zero-padded
    FieldSwr        = 8,  //        4 wide
    FieldCount      = 9,
};

// ---- A decoded reading ---------------------------------------------------

struct Reading {
    double  powerW{0.0};
    double  zOhms{0.0};       // |Z| at the coupler LOAD port
    // |phase|, UNSIGNED: the protocol never transmits the reactance sign (manual
    // p.12: QSY ~100 kHz and watch the trend). NEVER render signed reactance or R+jX;
    // inferring the sign by QSY slope would be a separate feature.
    double  phaseDeg{0.0};
    double  dBm{0.0};
    double  swr{1.0};

    int     alarmSetPoint{0};
    int     powerRange{0};    // 0=High, 1=Mid, 2=Low -- see RangeTracker
    int     peakHoldMode{0};

    QString callsign;         // trailing pad stripped

    // False when the fields don't describe one moment: at key-up in Peak Hold the
    // meter holds power and dBm ~1.7 s while Z, phase and SWR revert to idle.
    // Detected from physics, not the mode field: |Z| and phase must reproduce the
    // reported SWR (within 0.0093 on coherent records vs up to 43 off), which also
    // covers Avg mode's hold.
    bool coherent{true};
};

// Enum names. Hardware (not the manual) shows six alarm values ('A' walks 0..5)
// and three peak-hold modes ('F' walks 0..2). Unknown indices return a visible
// placeholder, never an empty string.
QString alarmSetPointName(int value);   // Off / 1.5 / 2.0 / 2.5 / 3.0 / User
QString powerRangeName(int value);      // High / Mid / Low
QString peakHoldModeName(int value);    // Avg / Peak / Fast

// ---- Validation and decode ----------------------------------------------

// The whole integrity check (there is NO CHECKSUM; a flipped digit keeps the
// length). Cheapest first: field count, per-field parseability, physical
// plausibility, and separator positions only when the length is the known 42, so
// a wider dBm field still validates.
bool looksLikeRecord(const QByteArray& body);

// Decodes a record body (marker already stripped). Returns nullopt for
// anything looksLikeRecord() rejects.
std::optional<Reading> decodeReading(const QByteArray& body);

// Streaming parser. Feed it bytes from either transport -- the wire format is
// identical over a local serial port and a raw-TCP proxy. Resyncs on the
// record marker, so leading garbage self-heals: notably the ser2net connect
// banner, which is ~210 bytes of text containing no ';' and is therefore
// discarded without special handling.
//
// Deliberately has zero Qt-networking dependency so it is unit-testable
// against captured bytes with no hardware and no event loop.
class ResponseParser {
public:
    void setReadingCallback(std::function<void(const Reading&)> cb)
    {
        m_onReading = std::move(cb);
    }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

    // Pending bytes not yet resolved into a record. Exposed so a test can
    // assert the buffer stays bounded on a stream that goes malformed after
    // delivering its first marker -- the one path the no-marker cap in feed()
    // does not cover.
    int bufferedBytes() const { return static_cast<int>(m_buf.size()); }

private:
    QByteArray m_buf;
    std::function<void(const Reading&)> m_onReading;
};

// Field bounds: the largest value each canonical width can express (7-char
// 0000.00 Power tops at 9999.99). With no checksum, an out-of-bound value can't
// come from a well-formed record and is rejected at the boundary (see
// hasCanonicalNumericForm).
constexpr double kMaxPowerW  = 9999.99;
constexpr double kMaxZOhms   = 9999.9;
constexpr double kMaxAbsDbm  = 999.9;
constexpr double kMaxSwr     = 99.99;

// ---- Derived values ------------------------------------------------------

// Return loss in dB: RL = -20*log10(|gamma|), gamma = (SWR-1)/(SWR+1). A perfect
// match (SWR 1.00, the idle reading) returns +infinity; 0 dB is total
// reflection. Callers must handle a non-finite result; see
// kMaxReportableReturnLossDb for presentation.
double returnLossDb(double swr);

// The largest return loss the SWR field can justify: "1.00" (two decimals) means
// [0.995, 1.005), i.e. RL >= ~52 dB, so the applet shows a lower bound instead of
// infinity. Derived from the field's quantisation. The meter's own display
// convention at a perfect match is unobserved; match it if one turns up.
constexpr double kSwrDisplayQuantum = 0.01;
double maxReportableReturnLossDb();

// SWR implied by |Z| and phase against a 50-ohm reference. The meter reports
// all three, so this is a redundancy -- and that redundancy is exactly what
// makes Reading::coherent detectable.
double swrFromImpedance(double zOhms, double phaseDeg);

// How far the implied and reported SWR may differ before a record is judged
// incoherent. Coherent captured records agreed to 0.0093; incoherent ones
// diverged by up to 43. Anywhere in between would do, so this is set well
// clear of measurement noise without being anywhere near the real signal.
constexpr double kCoherenceTolerance = 0.25;

// Reflected power from forward power and SWR. No caller yet (like
// kAlarmCommand/kModeCommand/kPeakAvgCommand); tested by lp100a_protocol_test.
// Callers must gate on Reading::coherent: at Peak Hold key-up this returns 0 W
// against a live forward reading (see LpMeterApplet::applyDimming()).
double reflectedWattsFromSwr(double forwardW, double swr);

// ---- Poll gating ---------------------------------------------------------

// The meter never pushes. On a shared transport (ser2net, Lantronix, Digi) the
// proxy mirrors one client's replies to all (a silent connection received 60
// records in 6 s), so ride along when someone else polls and poll when the wire
// is quiet. Gate on FOREIGN records only: gating on any record lets our own
// replies set the solo rate too (N=130 ms caps solo at 7.7 Hz; N=100 ms collides
// 48.5% of the time against a 100 ms foreign cadence). Pure and clock-injected
// for testing.
class PollGate {
public:
    // 10 Hz when we own the wire, which is also exactly the applet's label
    // throttle -- polling faster would produce readings the UI discards.
    static constexpr qint64 kSoloPollIntervalMs = 100;

    // Suppression threshold bounds, applied to 2x the observed foreign
    // cadence. Floor is above the measured worst-case foreign gap (121 ms) so
    // a fast foreign poller never leaks a spurious poll even before its
    // cadence has been established. Ceiling is a STATED DECISION, not an artifact: we ride along with
    // a foreign poller down to ~1.5 Hz, and past that we supplement rather
    // than let our own gauge fall below ~0.5 Hz. TelePost's own VCP offers up
    // to a 5 s interval, so a slow foreign client is a real configuration.
    static constexpr qint64 kMinQuietMs = 130;
    static constexpr qint64 kMaxQuietMs = 2000;

    void reset();

    // Call for every decoded record, before shouldPoll() for the same tick. One reply
    // per poll: the first record after an unanswered poll is ours regardless of
    // latency (works for slow remote/VPN paths). A foreign record arriving first is
    // miscounted once, and suppression still converges.
    void onRecord(qint64 nowMs);

    // Call exactly once per poll tick. Returns true if a poll should be sent
    // NOW, and records that poll internally so the next record can be
    // classified against it.
    bool shouldPoll(qint64 nowMs);

    // True while another client's polling is suppressing ours. For the
    // applet's diagnostic tooltip -- an operator riding along at someone
    // else's slow cadence otherwise sees a sluggish gauge with no explanation.
    bool isRidingAlong() const { return m_ridingAlong; }

    // Observed foreign cadence, or -1 before two foreign records have been
    // seen. Also for the tooltip.
    qint64 foreignIntervalMs() const { return m_foreignIntervalMs; }

    qint64 quietThresholdMs() const;

private:
    bool   m_ownReplyPending{false};
    // One timestamp, two jobs: shouldPoll() asks "how long since a foreign
    // record" and onRecord() asks "how long between the last two". An earlier
    // draft kept m_prevForeignMs alongside this, assigned the same value on
    // the same line -- two names implying a last-vs-previous distinction the
    // code never made. Collapsed; onRecord() reads it before overwriting it.
    qint64 m_lastForeignMs{-1};
    qint64 m_foreignIntervalMs{-1};
    bool   m_ridingAlong{false};
};

// ---- Power-range scaling -------------------------------------------------

// The meter reports WHICH of three ranges is active (field 5); it never
// reports what that range's ceiling in watts is. The ceilings are configured
// on the meter itself and cannot be read over the wire -- the manual's VCP
// lists 25/250/2500 W while the reference station's unit is set to
// 700/125/25 W. So they are an operator setting here, with these defaults.
struct RangeCeilings {
    // 1500 W: the US legal limit, which never under-scales a legal station.
    double highW{1500.0};
    // 150 W: a decade below, covering the ubiquitous 100 W barefoot rig with
    // headroom. The manual's 250 and the reference unit's 125 bracket it.
    double midW{150.0};
    // 25 W: the one value the manual and the reference unit agree on.
    double lowW{25.0};

    double forRange(int rangeIndex) const;
};

// Follows the meter's reported range onto a gauge scale: EXPAND immediately
// (an under-scaled gauge hides overpower), CONTRACT only after holding a smaller
// range for kContractHoldMs. Contraction is timed on the wall clock (riding
// along a slow poller makes record counts unbounded), requires a STABLE
// candidate (no checksum, so single corrupt records happen), and has no
// power > 0.1 W gate (safe because expansion is immediate).
class RangeTracker {
public:
    static constexpr qint64 kContractHoldMs = 2000;

    // Consecutive over-ceiling records required before the ceiling is raised.
    // ACOM uses 2 for the same job (AcomConnection::maybeAutoRangeUp), but its
    // 2 was chosen against an 8-bit checksum where ~1/256 corrupt frames pass.
    // This protocol has none, so the constant does not transfer on ACOM's
    // authority -- it is defensible here only because looksLikeRecord() is
    // doing much more work than a length test. If that validation is ever
    // weakened, this must be revisited.
    static constexpr int kCeilingExpandRecords = 2;

    // Who is calling setCeilings(). The up-only guard stops a config re-read from
    // shrinking a ceiling observed power already expanded this session (as ACOM does
    // for a late SystemConfig), but must not apply to an operator edit from the
    // context menu, which would otherwise silently do nothing. No default, so every
    // call site states its intent.
    enum class CeilingSource {
        ConfigLoad,    // stored settings, a reconnect, a late authoritative value
        OperatorEdit,  // the context menu; authoritative for the edited range
    };

    // For OperatorEdit, editedRange identifies the one menu row the operator
    // changed. std::nullopt means all ranges (Reset to defaults). This keeps
    // an edit to a hidden range from discarding an auto-expanded displayed
    // range while still allowing a same-value edit/reset to clear expansion.
    void setCeilings(const RangeCeilings& ceilings, CeilingSource source,
                     std::optional<int> editedRange = std::nullopt);
    void reset();

    void onReading(int reportedRange, double watts, qint64 nowMs);

    int    displayedRange() const { return m_displayedRange; }
    double ceilingW() const { return m_ceilingW; }

    // True while the ceiling has been auto-expanded past its configured value.
    // Session-scoped and never persisted -- reset() restores the configured
    // ceiling, exactly as AcomConnection resets to its default tier on every
    // reconnect rather than carrying an auto-scaled one forward.
    bool ceilingAutoExpanded() const { return m_autoExpanded; }

private:
    void adoptRange(int range);

    RangeCeilings m_ceilings;
    int     m_displayedRange{0};
    double  m_ceilingW{0.0};
    bool    m_autoExpanded{false};

    int     m_candidateRange{-1};
    qint64  m_candidateSinceMs{-1};
    int     m_overCeilingRun{0};
};

}  // namespace LpMeter
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::LpMeter::Reading)
