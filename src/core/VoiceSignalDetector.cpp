#include "VoiceSignalDetector.h"
#include "LogManager.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
    // Minimum width for a detected region to be considered a voice signal.
    constexpr double kVoiceMinHz = 1800.0;

    // Upper bound for voice signals.  Regions wider than this cannot be voice —
    // they are wideband interference (OTH radar, broadcast splatter, etc.) and
    // are emitted as a single QRM entry at the region centre rather than being
    // split into multiple 2.7 kHz voice chunks.
    constexpr double kVoiceMaxHz = 8000.0;

    // Region boundary: bins at least this far above the EMA floor are included
    // in the contiguous-region scan.  6 dB keeps the region edges accurate.
    constexpr float kThresholdDb = 6.0f;

    // Minimum peak above the EMA floor for a region to be REPORTED.
    // A region can be found at 6 dB but only shown when its peak is ≥ 8 dB —
    // this filters broad noise bumps that barely exceed the boundary threshold
    // while retaining weak-but-genuine voice signals that have a clear peak.
    constexpr float kMinPeakAboveFloorDb = 10.0f;

    // Percentile used for noise floor.  10th is more robust on crowded bands.
    constexpr int kNoisePctile = 10;

    // Gap-fill window: max of this many Hz either side of each bin.
    // Bridges the natural gaps in SSB speech without merging narrow digital signals
    // (CW ~50 Hz, FT8 ~50 Hz) whose expanded footprint still falls under kVoiceMinHz.
    constexpr double kFillHz = 400.0;
}

bool isVoiceSegmentLabel(const QString& label)
{
    return label.contains(QStringLiteral("PHONE"))  ||
           label.contains(QStringLiteral("SSB"))    ||
           label.contains(QStringLiteral("USB"))    ||
           label.contains(QStringLiteral("AM"))     ||
           label.contains(QStringLiteral("FM/RPT"));
}

QVector<DetectedVoiceSignal> detectVoiceSignals(
    const QVector<float>& binsDbm,
    double centerMhz,
    double bandwidthMhz,
    const QVector<QPair<double, double>>& voiceRangesMhz,
    float rollingNoiseFloorDbm,
    const QString& sliceMode,
    float softEdgeDb)
{
    QVector<DetectedVoiceSignal> results;
    const int N = binsDbm.size();
    if (N < 20 || bandwidthMhz <= 0.0) { return results; }

    const double hzPerBin = bandwidthMhz * 1.0e6 / N;
    const double startMhz = centerMhz - bandwidthMhz / 2.0;

    // ── Noise floor ───────────────────────────────────────────────────────
    // Prefer caller-supplied rolling average (EMA across ~10 frames) over the
    // per-frame 10th-percentile estimate.  The rolling average is less inflated
    // by transient QRM so the +3 dB threshold reliably rejects noise bumps.
    float noiseFloor;
    if (rollingNoiseFloorDbm > -500.0f) {
        noiseFloor = rollingNoiseFloorDbm;
    } else {
        QVector<float> sorted = binsDbm;
        std::sort(sorted.begin(), sorted.end());
        noiseFloor = sorted[N * kNoisePctile / 100];
    }
    const float threshold = noiseFloor + kThresholdDb;

    qCDebug(lcSHistory,
        "S-History: center=%.3f MHz  bw=%.3f MHz  bins=%d  hz/bin=%.1f  "
        "noise=%.1f dBm  thr=%.1f dBm  ranges=%d",
        centerMhz, bandwidthMhz, N, hzPerBin,
        static_cast<double>(noiseFloor), static_cast<double>(threshold),
        static_cast<int>(voiceRangesMhz.size()));

    // ── Voice-range mask ──────────────────────────────────────────────────
    QVector<bool> inVoice(N, voiceRangesMhz.isEmpty());
    if (!voiceRangesMhz.isEmpty()) {
        for (int j = 0; j < N; ++j) {
            const double f = startMhz + (j + 0.5) / N * bandwidthMhz;
            for (const auto& r : voiceRangesMhz) {
                if (f >= r.first && f <= r.second) { inVoice[j] = true; break; }
            }
        }
    }

    // ── Sliding-max fill ──────────────────────────────────────────────────
    // SSB speech has natural spectral gaps (unvoiced consonants, pauses).
    // A sliding max across ±kFillHz bridges those gaps so the contiguous-
    // region detector sees the full 2-3 kHz footprint instead of fragments.
    // Narrow digital signals (CW, FT8) expand by ≤ 2×kFillHz, keeping
    // their total below kVoiceMinHz when kFillHz < (kVoiceMinHz / 2).
    const int fillBins = std::max(1, static_cast<int>(kFillHz / hzPerBin));
    QVector<float> filled(N);
    for (int j = 0; j < N; ++j) {
        float mx = binsDbm[j];
        for (int k = 1; k <= fillBins; ++k) {
            if (j - k >= 0) { mx = std::max(mx, binsDbm[j - k]); }
            if (j + k <  N) { mx = std::max(mx, binsDbm[j + k]); }
        }
        filled[j] = mx;
    }

    // ── Contiguous-region detection on the filled spectrum ────────────────
    // The inVoice mask is applied at REGION level for voice-width signals only.
    // Narrow carriers (QRM tones) are scanned across the full visible spectrum
    // because interference does not respect band-plan boundaries.
    int i = 0;
    while (i < N) {
        if (filled[i] < threshold) { ++i; continue; }

        const int start = i;
        while (i < N && filled[i] >= threshold) { ++i; }
        const int end = i - 1;

        const double widthHz = (end - start + 1) * hzPerBin;
        const bool   isNarrow = (widthHz < kVoiceMinHz);

        // Voice-width signals: honour the band-plan voice allocation filter.
        // Narrow carrier QRM bypasses it — it can sit anywhere in the pan.
        if (!isNarrow) {
            bool anyInVoice = false;
            for (int j = start; j <= end && !anyInVoice; ++j) { anyInVoice = inVoice[j]; }
            if (!anyInVoice) {
                qCDebug(lcSHistory,
                    "  region @ %.4f MHz: width=%.0f Hz — outside voice allocation, skipped",
                    startMhz + (start / static_cast<double>(N)) * bandwidthMhz, widthHz);
                continue;
            }
        }

        // Use caller-supplied slice mode when it is USB or LSB; otherwise fall
        // back to the energy-asymmetry heuristic (useful for AM/FM pans).
        const bool callerKnowsMode = (sliceMode == QStringLiteral("USB") ||
                                      sliceMode == QStringLiteral("LSB"));
        QString mode;
        if (callerKnowsMode) {
            mode = sliceMode;
        } else {
            float lowerE = 0.0f, upperE = 0.0f;
            const int mid = (start + end) / 2;
            for (int j = start; j <= mid; ++j) { lowerE += binsDbm[j] - noiseFloor; }
            for (int j = mid+1; j <= end; ++j) { upperE += binsDbm[j] - noiseFloor; }
            mode = (lowerE >= upperE) ? QStringLiteral("USB") : QStringLiteral("LSB");
        }

        // tryAppend: check peak, compute frequency, emit a DetectedVoiceSignal.
        // For narrow regions (< kVoiceMinHz) the frequency is placed at the
        // peak bin — the 400 Hz fill boundary is off by ±fillHz so using the
        // mode edge would misplace the QRM marker by several hundred Hz.
        // For voice-width regions the mode edge (USB=left, LSB=right) is used.
        auto tryAppend = [&](int lo, int hi, bool isUsb) {
            if (lo > hi) { return; }
            const auto peakIt = std::max_element(binsDbm.constBegin() + lo,
                                                 binsDbm.constBegin() + hi + 1);
            const float pk = *peakIt;
            if (pk < noiseFloor + kMinPeakAboveFloorDb) {
                qCDebug(lcSHistory,
                    "  sub-region @ %.4f MHz: peak=%.1f dBm — rejected (< floor+%.0f dB)",
                    startMhz + (lo / static_cast<double>(N)) * bandwidthMhz,
                    static_cast<double>(pk),
                    static_cast<double>(kMinPeakAboveFloorDb));
                return;
            }
            const double segWidthHz = (hi - lo + 1) * hzPerBin;
            double fMhz;
            if (segWidthHz < kVoiceMinHz) {
                // Narrow carrier / QRM tone: use peak bin for accuracy.
                const int peakBin = static_cast<int>(peakIt - binsDbm.constBegin());
                fMhz = startMhz + (peakBin + 0.5) / N * bandwidthMhz;
            } else {
                int edge = isUsb ? lo : hi;
                if (softEdgeDb >= 0.0f) {
                    // Slope-based edge walk.  Starting from the 6 dB region
                    // boundary, walk outward (toward the carrier) on raw
                    // unfilled bins using the softer noiseFloor+softEdgeDb
                    // threshold.  Require kStopBins consecutive sub-threshold
                    // bins to terminate so isolated noise gaps in the voice
                    // rolloff don't end the walk prematurely.  Then roll back
                    // to the last above-threshold bin so the reported edge
                    // sits at the lowest visible voice energy — typically
                    // 100–200 Hz nearer to the carrier than the 6 dB edge.
                    const float softThr = noiseFloor + softEdgeDb;
                    constexpr int kStopBins = 3;
                    int below = 0;
                    int b     = edge;
                    while (true) {
                        const int next = isUsb ? b - 1 : b + 1;
                        if (next < 0 || next >= N) { break; }
                        if (binsDbm[next] < softThr) {
                            ++below;
                            if (below >= kStopBins) {
                                edge = isUsb ? (b + (kStopBins - 1))
                                             : (b - (kStopBins - 1));
                                edge = std::clamp(edge, 0, N - 1);
                                break;
                            }
                        } else {
                            below = 0;
                        }
                        b = next;
                    }
                    if (below < kStopBins) {
                        edge = std::clamp(b, 0, N - 1);
                    }
                }
                fMhz = startMhz + (edge + 0.5) / N * bandwidthMhz;
            }
            qCDebug(lcSHistory,
                "  DETECTED: %.4f MHz  %s  peak=%.1f dBm (%s)  width=%.0f Hz%s",
                fMhz, qPrintable(mode),
                static_cast<double>(pk), qPrintable(sLabel(pk)),
                static_cast<double>(segWidthHz),
                segWidthHz < kVoiceMinHz ? " [narrow/QRM candidate]" : "");
            results.append({fMhz, pk, mode, segWidthHz});
        };

        const bool isUsb = (mode == QStringLiteral("USB"));
        if (isNarrow) {
            // Narrow carrier (continuous tone, staggered OFDM QRM, etc.).
            // Report as a single entry without 2.7 kHz voice splitting.
            tryAppend(start, end, isUsb);
        } else if (widthHz > kVoiceMaxHz) {
            // Wideband interference (OTH radar, broadcast splatter, etc.).
            // Too wide to be voice — emit ONE entry at the region centre so it
            // is tracked as a single QRM source rather than exploding into many
            // 2.7 kHz voice chunks that clutter the panadapter.
            const float pk = *std::max_element(binsDbm.constBegin() + start,
                                               binsDbm.constBegin() + end + 1);
            if (pk >= noiseFloor + kMinPeakAboveFloorDb) {
                const int   centerBin = (start + end) / 2;
                const double fMhz = startMhz + (centerBin + 0.5) / N * bandwidthMhz;
                qCDebug(lcSHistory,
                    "  WIDEBAND: %.4f MHz  peak=%.1f dBm (%s)  width=%.0f Hz [QRM candidate]",
                    fMhz, static_cast<double>(pk), qPrintable(sLabel(pk)), widthHz);
                results.append({fMhz, pk, mode, widthHz});
            }
        } else {
            // Voice-width signal (1.8–8 kHz).
            // Natural SSB rolloff: USB is strongest on the left (carrier edge) and
            // weakens toward the right; LSB is the mirror.  A single station, even
            // a wide one, becomes ONE marker.  A second marker is only emitted when
            // there is a genuine secondary station: a valley followed by a new peak
            // rising ≥ kSplitValleyDb above the valley floor.
            constexpr float kSplitValleyDb = 8.0f;

            // Fill-corrected carrier-edge boundaries.
            const int adjLo = isUsb ? std::min(start + fillBins, end) : start;
            const int adjHi = isUsb ? end : std::max(end - fillBins, start);

            // Locate the primary (global) peak.
            const auto primIt  = std::max_element(binsDbm.constBegin() + adjLo,
                                                   binsDbm.constBegin() + adjHi + 1);
            const int  primBin = static_cast<int>(primIt - binsDbm.constBegin());

            // Search for a valley + secondary peak on the far side of the primary.
            const int sLo = isUsb ? (primBin + 1) : adjLo;
            const int sHi = isUsb ? adjHi          : (primBin > adjLo ? primBin - 1 : adjLo);

            int splitAt = -1;
            if (sLo < sHi) {
                const auto valleyIt  = std::min_element(binsDbm.constBegin() + sLo,
                                                         binsDbm.constBegin() + sHi + 1);
                const int   valleyBin = static_cast<int>(valleyIt - binsDbm.constBegin());
                const float valleyDb  = *valleyIt;

                const int p2Lo = isUsb ? (valleyBin + 1) : sLo;
                const int p2Hi = isUsb ? sHi : (valleyBin > sLo ? valleyBin - 1 : sLo);
                if (p2Lo <= p2Hi) {
                    const float p2Peak = *std::max_element(binsDbm.constBegin() + p2Lo,
                                                            binsDbm.constBegin() + p2Hi + 1);
                    if (p2Peak >= valleyDb + kSplitValleyDb) {
                        splitAt = valleyBin;
                    }
                }
            }

            if (splitAt < 0) {
                // Single station: one marker at the carrier edge.
                tryAppend(adjLo, adjHi, isUsb);
            } else if (isUsb) {
                tryAppend(adjLo, splitAt - 1, true);
                tryAppend(splitAt + 1, adjHi, true);
            } else {
                tryAppend(splitAt + 1, adjHi, false);
                tryAppend(adjLo, splitAt - 1, false);
            }
        }
    }

    if (results.isEmpty()) {
        qCDebug(lcSHistory, "  no voice-width signals found in this frame");
    }

    return results;
}

QString sLabel(float dbm)
{
    if (dbm >= -73.0f) {
        const int plus = static_cast<int>(std::ceil(dbm + 73.0f));
        return plus > 0 ? QString("S9+%1").arg(plus) : QStringLiteral("S9");
    }
    int sNum = static_cast<int>(std::ceil((dbm + 127.0f) / 6.0f));
    sNum = std::clamp(sNum, 0, 9);
    return QString("S%1").arg(sNum);
}

} // namespace AetherSDR
