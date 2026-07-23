#pragma once

// AetherClock shared time-frame machinery: the per-second/per-frame result
// types both time-signal decoders emit, plus cross-frame confidence-weighted
// bit voting over a sliding window of decoded frames.
//
// Field maps are supplied by each decoder from the NIST time-code tables
// (WWV/WWVH per NIST SP 432; WWVB legacy AM per NIST SP 250-67). This unit is
// map-agnostic: it votes bits, scores minute increments across consecutive
// frames, and reports lock + aggregate confidence.
//
// Pure DSP/logic — no Qt, no GUI (engine-boundary EB1/EB2).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace AetherSDR {

// Classified per-second symbol of an AM time-code frame.
enum class ClockSymbol : int8_t {
    Unknown = -1,
    Zero    = 0,
    One     = 1,
    Marker  = 2,
};

enum class ClockLockState : int {
    NoSignal  = 0,
    Acquiring = 1,
    Locked    = 2,
};

enum class ClockStation : int {
    Unknown = 0,
    Wwv     = 1,
    Wwvh    = 2,
    Wwvb    = 3,
};

// Why locked() currently says no (WS-7 acquisition telemetry). A tag on the
// EXISTING lock gates — the verdict logic is unchanged, the tag just names
// which gate refused (PRD-C: refusal reasons are set exactly where the voter
// already branches, never new logic). None means locked, or simply not enough
// frames yet (still collecting — the funnel reports frameCount separately).
// uint8_t so it crosses the engine boundary as a plain byte.
enum class ClockLockRefusal : std::uint8_t {
    None         = 0,
    QualityFloor = 1,  // resolution quality below Config::minLockQuality
    Plausibility = 2,  // voted timestamp implausibly far from the reference
    Staleness    = 3,  // no range-valid frame recent enough (or none at all)
    Contested    = 4,  // window disagrees with itself: minute-increment chain
                       // short, or a static field carries zero winning margin
};

// One BCD map entry: which second-of-frame carries which weight. A field's
// value is the sum of weights whose seconds decoded as One.
struct ClockBitWeight {
    int second;   // 0..59
    int weight;
};
using ClockFieldMap = std::vector<ClockBitWeight>;

// Emitted once per classified second (drives the alignment display).
struct ClockSecondInfo {
    int64_t edgeSample = 0;      // sample index (total consumed) of the second edge
    ClockSymbol symbol = ClockSymbol::Unknown;
    float confidence = 0.0f;     // matched-filter margin (best - runner-up), >= 0
    int secondOfFrame = -1;      // 0..59 once frame-synced, else -1
    // 1 s alignment window at the decoder's series rate:
    std::vector<float> envelope; // received amplitude series (normalized 0..1-ish)
    std::vector<float> expected; // zero-mean matched template of `symbol`
    int seriesRateHz = 0;        // 200 for WWV/WWVH, 100 for WWVB
    // Where the received pulse actually sits relative to the template's nominal
    // position, in series samples (matched-filter shift minus the nominal chain
    // delay). ~0 on a drift-free stream; nonzero while the decoder is absorbing
    // sample-clock drift — the display should shift the expected overlay by
    // this so envelope and template stay honest to each other (WS-4.5).
    int windowShift = 0;
};

// Emitted once per completed frame decode (raw, pre-voting).
struct ClockFrameInfo {
    int minute = -1, hour = -1, doy = -1, year2 = -1; // per-frame BCD decode
    int dut1Tenths = 0;          // signed tenths of a second (e.g. -3 = -0.3 s)
    bool dst1 = false;           // WWV s2  (WWVB: DST-code bit s57)
    bool dst2 = false;           // WWV s55 (WWVB: DST-code bit s58)
    bool leapPending = false;    // leap-second warning bit
    bool leapYear = false;       // WWVB LYI s55 (always false for WWV)
    float frameConfidence = 0.0f;    // 0..1
    int64_t frameStartSample = 0;    // sample index of second 0 of this frame
    ClockStation station = ClockStation::Unknown;
};

// The voted broadcast time once locked.
struct ClockTimeInfo {
    int minute = -1, hour = -1, doy = -1, year2 = -1;
    float quality = 0.0f;            // voter lockConfidence, 0..1
    int64_t lastEdgeSample = 0;      // sample index of the most recent second edge
    int lastEdgeSecondOfFrame = -1;  // second-of-frame of that edge
    ClockStation station = ClockStation::Unknown;
};

// Read-only decoder acquisition snapshot (WS-7 / PRD-C): the pre-lock funnel's
// stages 1-3 and 5, assembled ON CALL from state the decoder already keeps —
// nothing here feeds back into decode behavior, and no field is display-derived
// (every value is a real measurement or a real gate verdict). Stage 4 (the
// classified-seconds ring) is engine-side. Pure std types — the decoders are
// Qt-free (EB1/EB2); the engine mirrors this into the Qt-facing
// ClockDiagnostics struct it emits at ~1 Hz.
struct ClockDecoderDiagnostics {
    // stage 1 — carrier
    float toneSnrDb = 0.0f;    // WWVB: last tone-search peak/median in dB;
                               // WWV/WWVH: folded tick-band peak-to-mean in dB
    float pwmContrast = 0.0f;  // WWVB: p90/p10 envelope contrast (0 when n/a)
    bool toneDetected = false; // WWVB tone-gate passed / WWV tick fold locked
    // stage 2 — timing
    bool phaseLocked = false;  // WWV tickLocked / WWVB phaseKnown
    float delayEstMs = 0.0f;   // WWV tracked matched-filter delay; NaN when n/a
    // stage 3 — frame
    bool anchored = false;
    int badFrameStreak = 0;    // WWV marker-skeleton health (WWVB: 0)
    // stage 5 — vote
    int framesInWindow = 0;    // TimeFrameVoter::frameCount()
    int windowSize = 0;        // Config::window
    float voteQuality = 0.0f;  // lockConfidence() raw 0..1
    std::uint8_t refusalReason = 0;  // ClockLockRefusal
};

// A complete broadcast timestamp decoded from a single frame. minute/hour are
// 0-based; doy is 1-based day-of-year; year2 is the two-digit year (century
// assumption: full year = 2000 + year2, i.e. the 20xx century).
struct TimeFields {
    int minute = -1;
    int hour   = -1;
    int doy    = -1;
    int year2  = -1;
    bool operator==(const TimeFields&) const = default;
};

// Advance a timestamp forward by `minutes` (>= 0) using calendar arithmetic:
// minute 59 -> 0 carries the hour, hour 23 -> 0 carries the day-of-year, and
// doy wraps at the year length (365, or 366 when 2000 + year2 is a Gregorian
// leap year) carrying year2 (mod 100). Pure/stateless so it is unit-testable in
// isolation. Inputs are assumed already range-valid.
TimeFields advanceMinutes(TimeFields t, int minutes);

// Cross-frame confidence-weighted bit voter (reference: wwv_decode_proto.py —
// weight = max(confidence, 0.01); production adds frame aging).
class TimeFrameVoter {
public:
    // Well-known indices into Config::fields.
    enum FieldIndex : size_t {
        FieldMinutes = 0,   // special: expected to increment +1 per frame
        FieldHours   = 1,
        FieldDoy     = 2,
        FieldYear    = 3,
        FieldCount   = 4,
    };

    struct Config {
        std::array<ClockFieldMap, FieldCount> fields;
        std::vector<int> markerSeconds;  // marker positions within the frame
        size_t window = 8;               // sliding window of most recent frames
        int minFramesForLock = 2;        // consistent frames required for lock
        float agingFactor = 0.9f;        // per-frame-age weight multiplier

        // WS-4.5 honesty gates (all default-off = pre-WS-4.5 behavior).
        //
        // Trust floor: a bit whose WINNING side has no single vote at/above
        // this margin contributes zero trust (its value still composes — only
        // the certification collapses). Rationale (2026-07-20 live false lock,
        // decoded 2006-01-01 at q100): a deep fade zero-biases the SAME bits in
        // EVERY window frame, so the misread is unanimous — margin 1.0, full
        // participation — and a disagreement-based quality metric certifies it.
        // Unanimity of noise-grade reads is not evidence; a lock-quality floor
        // then refuses the lock. (Deliberately NOT an eligibility gate on the
        // votes themselves: silencing noise-grade votes flips coherence
        // verdicts and vote topology on real corpora — measured 2026-07-20.)
        float minBitConfidence = 0.0f;
        // locked() additionally requires the resolution quality to reach this.
        float minLockQuality = 0.0f;
        // Absolute-plausibility leg: when armed (bound > 0 and referenceNow
        // set), locked() refuses any voted timestamp farther than this many
        // minutes from the reference clock. Systematic misreads (misaligned
        // windows, coherent zero-bias) are self-consistent by construction, so
        // an independent reference is the only evidence that can veto them. The
        // bound is deliberately generous — a host clock that is minutes or even
        // hours wrong is exactly what the feature measures; a decode DECADES
        // away can only be garbage.
        int plausibilityBoundMinutes = 0;
        std::function<TimeFields()> referenceNow;
        // Staleness bound: locked() requires a range-VALID frame within the
        // newest maxNewestValidAge window slots. Without it the voter
        // dead-reckons: range-invalid garbage frames used to vanish from the
        // normalized window entirely, so the surviving stale frames kept
        // extrapolating forward (+1 min per garbage frame) at undiminished
        // quality — the 2026-07-20 receiver emitted advancing timestamps at
        // q1.00 for minutes after the air turned to garbage.
        int maxNewestValidAge = 3;
    };

    explicit TimeFrameVoter(Config cfg);

    // Arm (or replace) the absolute-plausibility reference at runtime — the
    // engine plumbs the host clock through the owning decoder after
    // construction. boundMinutes <= 0 or a null function disarms the gate.
    void setPlausibility(std::function<TimeFields()> referenceNow,
                         int boundMinutes);

    // Add one complete 60 s frame of classified symbols + confidences. Frames
    // MUST be consecutive broadcast minutes — the caller resets on gaps or
    // re-anchoring. Markers are excluded from bit votes automatically.
    void addFrame(const std::array<ClockSymbol, 60>& symbols,
                  const std::array<float, 60>& confidence);

    void reset();

    int frameCount() const;   // frames currently in the window
    int windowSize() const { return static_cast<int>(m_cfg.window); }

    // Lock = at least minFramesForLock frames in the window, AND at least
    // (minFramesForLock - 1) adjacent frame pairs whose per-frame minutes
    // decode increments by exactly +1 (mod 60), AND voted static fields
    // self-consistent (each static field carries a strictly positive winning
    // margin in the same normalized space votedField/lockConfidence use — an
    // all-Unknown window yields zero margin everywhere and must not lock).
    // Increment support is COUNTED across the window, not required of every pair
    // — live per-frame decodes are routinely imperfect and one corrupted frame
    // must not permanently prevent or drop the lock (the reference scores
    // increments: marker_score + 4 * increment_count).
    bool locked() const;

    // WS-7: which gate locked() is currently refusing on (None when locked, or
    // when merely short of minFramesForLock — the funnel reports frameCount()
    // separately). locked() and lockRefusal() both read one lockVerdict() pass,
    // so the two can never disagree.
    ClockLockRefusal lockRefusal() const;

    // Voted value of a timestamp field (minute/hour/doy/year), reported "as of
    // the newest frame". The contract is NORMALIZE, THEN PER-BIT:
    //   1. Each frame's whole timestamp is decoded from its own bits and
    //      extrapolated forward by its age in minutes (calendar arithmetic), so
    //      every in-window frame is expressed at the SAME (newest) epoch.
    //   2. Bits are then voted per-field across that normalized space. For a
    //      field the extrapolation left unchanged (the common case away from a
    //      boundary) each frame votes its ORIGINAL symbols with their ORIGINAL
    //      per-second confidences — a faded bit carries a low matched-filter
    //      margin and loses, which is what corrects the single-bit fades that
    //      corrupt noisy corpora frame-by-frame. For a field the extrapolation
    //      moved across a carry (minutes almost always; hour/doy/year only when
    //      the window straddles a boundary) the frame re-encodes the normalized
    //      value and weights every bit by its field-MIN original confidence — a
    //      BCD value is only as reliable as its weakest bit.
    //   3. Per-bit weights are max(confidence, 0.01) * agingFactor^age.
    //   4. Composition is COHERENCE-GATED. Per-bit statistics alone cannot certify
    //      a multi-bit BCD value: sibling bits can be won by DIFFERENT frame camps
    //      (a faded bit plus a confident sibling misread), assembling a value no
    //      frame held. A bit is coherent only when its confidence-weighted winner
    //      agrees with its aging-only count majority. If every bit of a field is
    //      coherent the field is that per-bit compose (the fade-rescue / clean
    //      path); if any bit is incoherent the field falls back to the top HELD
    //      value — one a frame actually carried, voted per-value with the same
    //      aged x field-min weighting.
    // This is rollover-safe BECAUSE the extrapolation removes the epoch skew: a
    // stale hour can never outvote the current one, since after normalization
    // every frame is voting on the current hour. Cross-frame bit blending is
    // confined to normalized space AND to coherent bits, so blending there is
    // correction, not synthesis across epochs or across frame camps. Should the
    // compose ever produce an out-of-range BCD value, the vote falls back to the
    // single highest-aged-weight frame's extrapolated tuple (a guarded fallback,
    // never the primary path). Returns -1 if no frame decodes to a range-valid
    // timestamp.
    int votedField(FieldIndex field) const;

    // Raw per-frame minutes decode of the newest frame (no voting).
    int lastFrameMinute() const;

    // Aggregate lock quality 0..1: the MINIMUM winning-vote margin across the
    // voted bits of the timestamp, measured in the SAME normalized
    // (age-extrapolated) space as votedField, saturating with frame count. The
    // min — not the mean — is the honest aggregate: a timestamp is only as
    // trustworthy as its least-certain bit, so quality collapses to the single
    // most-contested bit's margin. Averaging let dozens of clean bits mask the
    // one bit that decided a field, reporting near-1.0 while the value was wrong.
    // A clean rollover still reads ~1.0 (every bit unanimous once normalized), so
    // value and quality can no longer decouple. A bit with zero participating
    // votes across the window scores margin 0. ~0 with < minFramesForLock frames.
    float lockConfidence() const;

private:
    struct Frame {
        std::array<ClockSymbol, 60> symbols;
        std::array<float, 60> confidence;
    };
    Config m_cfg;
    std::vector<Frame> m_frames;   // newest last

    // Per-frame BCD decode of one field: sum the field-map weights whose second
    // classified as One (Marker/Unknown contribute nothing).
    int decodeField(const Frame& f, FieldIndex field) const;
    int decodeMinutes(const Frame& f) const;  // == decodeField(f, FieldMinutes)

    // One in-window frame reduced to its NORMALIZED (age-extrapolated) per-field
    // bit set — the single representation votedTimestamp / lockConfidence /
    // locked all consume, so their view of the window is guaranteed identical.
    struct NormalizedFrame {
        int age = 0;                 // 0 = newest frame
        // False for a frame whose raw decode fell outside valid broadcast
        // ranges: it VOTES nothing and HOLDS nothing, but it still occupies its
        // window slot and its real per-second confidences still count against
        // participation — confident air that fails to form a valid timestamp
        // is evidence AGAINST the surviving stale frames, not silence.
        bool valid = true;
        double meanConfidence = 0.0; // frame mean over the four field maps
        TimeFields ext;              // this frame's timestamp at the newest epoch
        // Per bit of a field map: the normalized symbol and the confidence that
        // weights it. symbol == Unknown means "no vote" (an original Marker /
        // Unknown second that survived into the unchanged-field path).
        struct Bit {
            ClockSymbol symbol = ClockSymbol::Unknown;
            float confidence = 0.0f;
        };
        std::array<std::vector<Bit>, FieldCount> bits; // parallel to Config::fields
    };

    // Reduce every range-valid in-window frame to its NormalizedFrame (newest ->
    // age 0). Frames whose raw decode falls outside valid broadcast ranges are
    // dropped, in the same spirit as excluding Marker/Unknown symbols from a bit
    // vote. Empty when no frame qualifies.
    std::vector<NormalizedFrame> buildNormalizedFrames() const;

    // The whole timestamp resolved from the normalized window, PLUS the quality
    // of that resolution — computed together so value and quality can never
    // disagree. Per field: if every bit is COHERENT (its confidence-weighted
    // winner agrees with its aging-only count majority) the field is the per-bit
    // compose, as before; if any bit is incoherent (a confident sibling misread
    // would let per-bit voting assemble a value no frame held) the field falls
    // back to the top HELD value (one a frame actually carried). Quality is the
    // MIN across fields of the field's contribution: for a coherent field the min
    // per-bit trust (margin x participation), for an incoherent field the min of
    // the held-value margin and the coherent bits' trust. An out-of-range compose
    // falls back to the highest-aged-weight frame's tuple at quality 0.
    struct Resolution {
        TimeFields value;    // all -1 when no frame qualifies
        double quality = 0.0;  // pre-saturation; 0 on the range fallback
    };
    Resolution computeResolution() const;

    // Thin wrapper: the resolved timestamp value (the source of truth for
    // votedField's four fields). Returns all-(-1) TimeFields when none qualifies.
    TimeFields votedTimestamp() const;

    // The lock gates, evaluated in their original order, with each refusal
    // tagged (WS-7). locked() == lockVerdict().locked by construction.
    struct LockVerdict {
        bool locked = false;
        ClockLockRefusal reason = ClockLockRefusal::None;
    };
    LockVerdict lockVerdict() const;
};

} // namespace AetherSDR
