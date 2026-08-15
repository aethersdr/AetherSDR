#pragma once

#include "TxLinearityTypes.h"

#include <cmath>
#include <functional>
#include <span>
#include <string>

namespace AetherSDR::txlin {

// The seam between "where the observed TX signal comes from" and "what is
// measured on it".
//
// This is the most consequential design decision in the feature, because the
// analyzer is the measurement half of a digital-predistortion loop and the
// correction half arrives later on a different backend. Everything above this
// interface is arithmetic on complex samples and knows nothing about
// FlexRadio, VITA-49, DAX, Metis or Qt. Everything below it is one radio's
// plumbing. Get the seam right and the HL2 DPD work is additive; get it wrong
// and the DSP gets rewritten, which is the one part that must not be.
//
// hasReferenceChannel() is the capability that gates the whole downstream
// pipeline:
//
//   false — FlexRadio. DAX IQ is unidirectional: it streams IQ from radio to
//           host and there is no API to send IQ back. The only inbound path is
//           the TX Audio stream, which carries audio, and an SSB modulator
//           derives phase from amplitude via Hilbert transform, so an arbitrary
//           complex correction cannot be expressed in it at all. Phase 1 is
//           therefore reference-free: spectrum, IMD products, shoulders, ACPR.
//           Phase 2 recovers an envelope by SYNTHESISING the reference from a
//           known two-tone stimulus (valid for that stimulus only).
//
//   true  — HPSDR / Hermes-Lite 2. The radio supplies both the DAC reference
//           stream and the RF feedback stream, which enables true AM-AM/AM-PM
//           and, eventually, correction.
//
// Consumers must degrade — not crash, and above all not fabricate — when
// CaptureFrame::reference is disengaged.
class ITxCaptureSource {
public:
    using FrameCallback = std::function<void(const CaptureFrame&)>;
    // Terminal failure of the capture path. The controller treats any call to
    // this as an immediate abort, because a keyed radio whose analysis thread
    // has stopped receiving is the exact situation §8 exists to prevent.
    using ErrorCallback = std::function<void(const std::string& reason)>;

    virtual ~ITxCaptureSource() = default;

    virtual bool start(const CaptureConfig& config) = 0;
    virtual void stop() = 0;

    [[nodiscard]] virtual bool hasReferenceChannel() const = 0;

    // Human-readable identity for export stamps ("FLEX-8600 DAX IQ ch 2").
    [[nodiscard]] virtual std::string describe() const = 0;

    // Frames are delivered on the capture thread. Implementations must set both
    // callbacks before start() and must not invoke either after stop() returns.
    virtual void setFrameCallback(FrameCallback callback) = 0;
    virtual void setErrorCallback(ErrorCallback callback) = 0;

    // Capture gate: only frames assembled while open reach the frame callback,
    // and a partial frame straddling either edge is discarded rather than
    // spliced. The controller opens it once the post-key settling guard has
    // elapsed and closes it before key-down ends, so PA thermal and ALC
    // settling never average into a steady-state distortion measurement.
    //
    // Defaulted to a no-op because a source that only ever produces samples
    // during transmit — a future HPSDR feedback stream, or a file replay in a
    // test — has nothing to gate.
    virtual void setGateOpen(bool open) { (void)open; }
};

// ── Overload detection ──────────────────────────────────────────────────────

// Fraction of samples whose magnitude sits at or above `threshold` (relative to
// full scale 1.0).
//
// Clipping is judged by POPULATION, not by any single sample. On a signal whose
// peaks are the entire point, one sample touching full scale in 16384 is a
// statistical certainty and means nothing; a population piled against the rail
// is the condition that flattens the envelope and manufactures the very
// distortion the instrument is trying to measure. The caller compares this
// against a fraction, so the verdict is "how much of the frame is saturated",
// which is the question that matters.
inline double saturatedFraction(std::span<const std::complex<float>> samples,
                                double threshold = 0.999)
{
    if (samples.empty()) {
        return 0.0;
    }
    // Compare squared magnitudes so the hot loop carries no sqrt.
    const double t2 = threshold * threshold;
    std::size_t hits = 0;
    for (const std::complex<float>& s : samples) {
        const double re = double(s.real());
        const double im = double(s.imag());
        if (re * re + im * im >= t2) {
            ++hits;
        }
    }
    return double(hits) / double(samples.size());
}

// Default fraction above which a frame is declared clipped. 0.1% of a 16384
// sample frame is ~16 samples — comfortably above the handful a clean
// peak-riding envelope produces, comfortably below the thousands a genuinely
// overdriven path produces.
inline constexpr double kClippedFractionThreshold = 0.001;

}  // namespace AetherSDR::txlin
