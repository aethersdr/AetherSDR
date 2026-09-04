#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>

namespace AetherSDR {

// TelePost LP-100A digital vector RF wattmeter serial protocol.
//
// Protocol authority: TelePost's own "LP-100A Digital Vector RF Wattmeter
// Operations Manual", pp. 20-21 (telepostinc.com/LP-100A-Op_Manual.pdf). The
// field ORDER and semantics come from there. The field WIDTHS and the record
// framing come from a live capture off real hardware (2026-08-29, 954 records
// across idle and three transmit cycles) because the manual's printed example
// is wrong in one field -- see kRecordLength below. See
// docs/architecture/lp-100a-wattmeter-design.md for the full provenance
// record, including which of the manual's claims did not survive measurement.
//
// Link: 115200 8N1, no handshake. Firmware < 1.2.0.0 used 38400 and < 1.0.3
// used 19200 without dBm or SWR; neither is supported and neither is detected
// -- an older unit simply produces records this parser rejects.
namespace LpMeter {

// ---- Commands (host -> meter) -------------------------------------------
//
// The ENTIRE host command set. Every one is a single byte, and every one
// except Poll is an INCREMENT with no absolute-set form and no acknowledgement:
// the meter's resulting state is only observable in the next poll response.
// Anything built on these must therefore report what the next reading says,
// never what was commanded.
//
// One useful consequence of single-byte commands: two clients sharing a
// ser2net port cannot interleave *within* a command, so no malformed command
// can reach the meter. That is what lets PollGate below be advisory rather
// than a hard lock.
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

// Body length after the marker. Measured, not taken from the manual: the
// manual's printed example
//
//     ;1457.00,49.3,005.0,2,N8LP  ,0,2,61.6,1.02
//
// is 41 characters because its Z field is 4 wide, where the wire sends 5
// (zero-padded, e.g. "046.3"). Every one of 954 captured records was 42.
//
// This is a SANITY CHECK, not the validation rule -- see looksLikeRecord().
// A firmware whose dBm field needs 5 characters (any value in -99.9..-10.0;
// dBm is the one field that is signed AND not zero-padded) would emit 43,
// and rejecting purely on length would then reject every record on that unit.
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
    // |phase|, UNSIGNED. The sign of the reactance is never transmitted --
    // this is a protocol limitation, not a decode gap. The manual (p.12)
    // has the operator recover it by hand: QSY ~100 kHz and watch which way
    // the reactance moves. LP-Plot automates that only "since it can control
    // your transmitter's frequency", sweeping to read the slope. 954 captured
    // records across three frequencies contained no sign character.
    //
    // So NEVER render this as signed reactance or an R+jX form. AetherSDR
    // does drive the VFO and could therefore implement the QSY-slope
    // inference LP-Plot uses, but that is a separate feature, not a decode.
    double  phaseDeg{0.0};
    double  dBm{0.0};
    double  swr{1.0};

    int     alarmSetPoint{0};
    int     powerRange{0};    // 0=High, 1=Mid, 2=Low -- see RangeTracker
    int     peakHoldMode{0};

    QString callsign;         // trailing pad stripped

    // False when this record's fields do not describe one moment in time.
    //
    // MEASURED: at key-up in Peak Hold the meter holds power and dBm for
    // ~1.7 s while Z, phase and SWR have already reverted to idle -- 54 of
    // 894 captured records reported real power alongside SWR 1.00. A record
    // is not a snapshot, and anything that combines fields across that
    // boundary is wrong.
    //
    // Detected from physics rather than from the mode field: |Z| and phase
    // must reproduce the SWR the meter itself reports, and they do so to
    // within 0.0093 across every coherent record captured while diverging by
    // up to 43 across the incoherent ones. That separation is enormous, and
    // being mechanism-independent it also covers Avg mode's own averaging
    // hold -- which gating on peakHoldMode would silently miss.
    bool coherent{true};
};

// ---- Enum names ----------------------------------------------------------
//
// The manual documents five alarm values and two peak-hold modes. Both counts
// are wrong, CONFIRMED on hardware 2026-08-29 by cycling the meter through
// full rotations: 'A' walks 0,1,2,3,4,5 -- six alarm values -- and 'F' walks
// 0,1,2 -- three peak-hold modes. The reference Node-RED flow's fuller tables
// were right and the manual is short, which also settles that its author had
// observed these rather than adding the cases defensively.
//
// Every lookup still returns a readable placeholder for an unknown index
// rather than an empty string, so a firmware with a value neither source
// knows degrades to a visible "unknown" instead of a blank readout.
QString alarmSetPointName(int value);   // Off / 1.5 / 2.0 / 2.5 / 3.0 / User
QString powerRangeName(int value);      // High / Mid / Low
QString peakHoldModeName(int value);    // Avg / Peak / Fast

// ---- Validation and decode ----------------------------------------------

// The whole integrity mechanism for this protocol. There is NO CHECKSUM, so
// this function is the only thing standing between a corrupted record and a
// gauge -- which is why it checks considerably more than the reference flow's
// single `str.length == 42`. A flipped digit keeps the length.
//
// Checks, cheapest first: field count, per-field parseability, physical
// plausibility, and -- only when the length matches the known-good layout --
// that every separator sits where it must. The last check is skipped for
// other lengths so a wider dBm field degrades to "still valid" rather than
// "every record rejected".
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

// ---- Field bounds --------------------------------------------------------
//
// The largest value each field's canonical width can express. Read off the
// measured wire format rather than invented -- a 7-character Power field in
// 0000.00 form cannot say more than 9999.99 -- so a value above these did not
// come from a well-formed record whatever else about it looked plausible.
//
// They matter because this protocol has NO CHECKSUM: looksLikeRecord() is the
// entire integrity mechanism, and without an upper bound a corrupted field
// reached the gauges as a float infinity (see hasCanonicalNumericForm in the
// .cpp for the exact path). Constitution VII -- validate at the boundary.
constexpr double kMaxPowerW  = 9999.99;
constexpr double kMaxZOhms   = 9999.9;
constexpr double kMaxAbsDbm  = 999.9;
constexpr double kMaxSwr     = 99.99;

// ---- Derived values ------------------------------------------------------

// Return loss in dB from SWR, as the positive quantity the term denotes:
// RL = -20*log10(|gamma|), so a PERFECT match returns +infinity (nothing comes
// back) and 0 dB means total reflection.
//
// An earlier version had this exactly backwards -- it special-cased SWR <= 1.0
// to return 0.0 dB, i.e. it reported a matched load and a dead short
// identically, and did so on screen throughout receive because the meter idles
// at SWR 1.00. Its comment called that "the division-by-zero edge every naive
// implementation gets wrong", which was wrong twice over: gamma at SWR 1 is
// (1-1)/(1+1) = 0, so nothing is divided by zero; the divergence is log10(0).
// A test pinned the wrong value, so the suite protected the bug. Caught by
// @rfoust in review of #5320.
//
// Callers must expect a non-finite result. Formatting it is a presentation
// decision -- see kMaxReportableReturnLossDb.
double returnLossDb(double swr);

// The largest return loss the SWR field can actually justify.
//
// SWR arrives as 4 characters with two decimals, so a reported "1.00" means
// anything in [0.995, 1.005) and the true return loss is merely >= ~52 dB.
// Rendering infinity would claim a precision the wire does not carry, so the
// applet shows this as a lower bound instead. Derived from the field's own
// quantisation, not chosen.
//
// UNVERIFIED: what the LP-100A's own display shows at a perfect match is not
// documented in the manual and we have not observed it. If it turns out to
// have a house convention, match the instrument and update this.
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

// Reflected power from forward power and SWR.
//
// NO v1 CALLER, deliberately -- the applet shows no reflected figure, for the
// reason below. Declared here alongside kAlarmCommand/kModeCommand/
// kPeakAvgCommand, which are also unused in v1, so the control PR adds no
// protocol surface. Exercised by lp100a_protocol_test.
//
// CAUTION -- the fields this consumes are not always mutually coherent. At
// key-up in Peak Hold the meter holds power and dBm for ~1.7 s while Z, phase
// and SWR have already reverted to idle, so this returns 0 W reflected against
// a live-looking forward reading. A caller must therefore gate the result on
// Reading::coherent, which decodeReading() computes by checking |Z| and phase
// against the reported SWR; LpMeterApplet::applyDimming() is the worked
// example. (An earlier draft of this comment pointed at "per-field liveness
// tracking in LpMeterConnection" -- no such thing was ever built; the
// coherence flag on the reading replaced it.) Provided as a pure function so
// that decision stays with the caller.
double reflectedWattsFromSwr(double forwardW, double swr);

// ---- Poll gating ---------------------------------------------------------

// The meter never pushes; it answers 'P' and nothing else. On a shared
// transport (ser2net, Lantronix, Digi) other clients are commonly already
// polling it -- measured on the reference station: a second connection that
// sent NOTHING received 60 complete records in 6 s, so the proxy mirrors one
// client's replies to every other.
//
// Polling blindly on top of that doubles the meter's work, and for any client
// that reads blind after a fixed delay it makes that client attribute our
// replies to its own polls. So: ride along when someone else is already
// polling, poll when the wire is quiet.
//
// The subtlety worth preserving. Gating on "no record in the last N ms" does
// NOT work, because our own replies reset the same timestamp and N then sets
// the solo poll rate as well as the suppression threshold. Measured against
// the reference station's 100 ms foreign cadence: N=130 ms suppresses cleanly
// but caps solo polling at 7.7 Hz, while N=100 ms keeps 10 Hz and polls over
// the other client 48.5% of the time. Gating on FOREIGN records only breaks
// that coupling -- solo stays at a full 10 Hz and the threshold is free to be
// whatever suppression needs.
//
// Pure and clock-injected: no QTimer, no QDateTime, no socket. That is what
// makes the coupling fix testable rather than merely asserted.
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

    // Call for every decoded record, before shouldPoll() for the same tick.
    //
    // Classification is "exactly one reply per poll": the first record after
    // an unanswered poll is ours, regardless of latency. This remains valid
    // for remote/VPN ser2net paths where a reply can arrive much later than
    // the <=15 ms measured on the local reference station. If a foreign
    // record happens to arrive first, it consumes the pending slot and our
    // later reply is counted as foreign; the next foreign record still
    // establishes the shared cadence, so suppression converges without a
    // latency heuristic.
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

// Follows the meter's reported range onto a gauge scale.
//
// The asymmetry is deliberate and is the one piece of the reference flow's
// logic worth keeping: EXPAND the scale immediately, CONTRACT it only after
// the meter has held the smaller range for a while. An under-scaled gauge
// pins the needle and hides an overpower condition; an over-scaled one costs
// nothing but a moment of a generous axis.
//
// Three things here differ from that flow deliberately:
//
//   1. Contraction is timed on the WALL CLOCK, not counted in records. The
//      flow counts 20 records and its own comment concedes "2 seconds
//      assuming 100 msec polling rate" -- but PollGate above can legitimately
//      ride along behind a slow foreign poller, where 20 records is minutes.
//   2. Contraction requires a STABLE candidate. The flow's counter advances
//      whenever the reported range merely disagrees, then adopts whatever a
//      single record says when the count expires; a meter hunting between two
//      ranges therefore latches on one arbitrary sample. This protocol has no
//      checksum, so a lone corrupt-but-plausible record is likelier here than
//      in any of the checksummed peers.
//   3. No "only while power > 0.1 W" gate. In the flow that gate stalls the
//      timer between SSB syllables, making "2 seconds" an unpredictable
//      multiple of itself. Dropping it is safe precisely BECAUSE expansion is
//      immediate: contracting while idle is invisible, and keying up widens
//      the scale on that same record.
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

    // Why setCeilings() needs to know WHO is calling.
    //
    // The up-only guard below exists so a re-read of the stored configuration
    // cannot shrink a ceiling that observed power already expanded within this
    // session -- ACOM applies the same rule to a late SystemConfig reply that
    // would otherwise snap the tier below what auto-ranging established.
    //
    // But the identical entry point also serves a deliberate edit from the
    // applet's context menu, and there the guard is simply wrong: an operator
    // who lowers High to match their meter gets no change AND no feedback,
    // because the ceiling does not move so gaugeCeilingChanged() never fires.
    // An operator action that silently does nothing is worse than the stale
    // ceiling the guard was written to prevent.
    //
    // So the caller states its intent. No default -- the whole point is that
    // the two paths stop being indistinguishable at the call site.
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
