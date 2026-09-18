#include "ModeFilterPresets.h"

#include "VoiceModeGate.h"   // isCwMode

#include <cstdlib>

namespace AetherSDR::ModeFilters {

const QVector<int>& widthsForMode(const QString& mode)
{
    // From docs/data/vfo_mode_filters.csv — 8 presets per mode, 4x2 grid
    static const QVector<int> usb{1800, 2100, 2400, 2700, 2900, 3300, 4000, 6000};
    static const QVector<int> am {5600, 6000, 8000, 10000, 12000, 14000, 16000, 20000};
    static const QVector<int> cw {50, 100, 250, 400, 500, 600, 800, 1000};
    static const QVector<int> dig{100, 300, 600, 1000, 1500, 2000, 3000, 6000};
    static const QVector<int> rtty{250, 300, 350, 400, 500, 1000, 1500, 3000};
    static const QVector<int> dfm{6000, 8000, 10000, 12000, 14000, 16000, 18000, 20000};
    static const QVector<int> fm{};

    if (mode == "USB" || mode == "LSB") return usb;
    if (mode == "AM" || mode == "SAM") return am;
    if (isCwMode(mode)) return cw;
    if (mode == "DIGU" || mode == "DIGL" || mode == "NT") return dig;
    if (mode == "RTTY") return rtty;
    if (mode == "DFM") return dfm;
    if (mode == "FM" || mode == "NFM") return fm;
    return usb;
}

Edges edgesForWidth(const QString& mode, int widthHz, const SliceContext& ctx)
{
    int lo = 0, hi = 0;

    if (mode == "DIGU") {
        // For widths < 3000 Hz, center the filter on the stored digu_offset.
        // SmartSDR behavior (fw v1.4.0.0): offset is the audio center frequency;
        // filter spans [offset - width/2, offset + width/2], clamped so lo >= 95.
        // For widths >= 3000 Hz, SmartSDR ignores the offset and runs from 95 Hz
        // upward — preserve that behavior unchanged.
        if (widthHz < 3000) {
            int offset = ctx.diguOffset;
            lo = offset - widthHz / 2;
            hi = offset + widthHz / 2;
            if (lo < 95) {
                // Clamp: don't let lo drop below 95 Hz (carrier rejection)
                hi += (95 - lo);
                lo = 95;
            }
        } else {
            lo = 95;
            hi = widthHz;
        }
    } else if (mode == "DIGL") {
        // Mirror of DIGU: offset is negative (below carrier). For widths < 3000 Hz,
        // center on -digl_offset, clamped so hi <= -95.
        // For widths >= 3000 Hz, run from -95 downward.
        if (widthHz < 3000) {
            int offset = ctx.diglOffset;
            hi = -offset + widthHz / 2;
            lo = -offset - widthHz / 2;
            if (hi > -95) {
                lo -= (hi + 95);
                hi = -95;
            }
        } else {
            lo = -widthHz;
            hi = -95;
        }
    } else if (mode == "LSB") {
        // SSB low cut is a fixed 100 Hz (matches SmartSDR for every SSB
        // filter); the high cut is derived as lo + width so the effective
        // passband equals the labeled width. Mirror of USB below the
        // carrier: edge nearest the carrier is -100 Hz. (#3292)
        hi = -100; lo = -100 - widthHz;
    } else if (mode == "RTTY") {
        // RTTY: RF_frequency = mark. Filter is relative to mark.
        // Space is at -rttyShift. Passband should encompass both tones.
        // Expand symmetrically around the midpoint between mark(0) and space(-shift).
        int shift = ctx.rttyShift;
        int mid = -shift / 2;
        lo = mid - widthHz / 2;
        hi = mid + widthHz / 2;
    } else if (mode == "CW" || mode == "CWL" || mode == "CWU") {
        // Centered on carrier — radio's BFO handles pitch offset.
        // CWU belongs with the other two spellings: it was falling through to
        // the final else and getting a USB-shaped {95, width} with no carrier
        // in it. It is reachable — NetSchedulerDialog lists it as a schedulable
        // mode and RadioSetupDialog has it as the CWU/CWL sideband toggle — and
        // it was wrong under the old passband convention too, just less visibly.
        lo = -widthHz / 2;
        hi =  widthHz / 2;
    } else if (mode == "AM" || mode == "SAM" || mode == "DSB"
               || mode == "FM" || mode == "NFM" || mode == "DFM") {
        lo = -(widthHz / 2); hi = (widthHz / 2);
    } else if (mode == "FDVL") {
        lo = -widthHz; hi = -95;
    } else if (mode == "USB") {
        // SSB low cut is a fixed 100 Hz (matches SmartSDR for every SSB
        // filter); the high cut is derived as lo + width so the effective
        // passband equals the labeled width. Previously this sent lo=95,
        // hi=width, which yielded an effective width of (label-95) — e.g.
        // the 2.9k preset produced ~2805 Hz — and left the active-preset
        // matcher comparing against off-by-95 widths. (#3292)
        lo = 100; hi = 100 + widthHz;
    } else {
        // FDVU/FDV/etc: low cut at 95 Hz to reject carrier/hum
        lo = 95; hi = widthHz;
    }
    return {lo, hi};
}

int widthForEdges(const QString& mode, int lo, int hi)
{
    // SSB labels its ladder by the passband it delivers, and the rule pins one
    // edge at 100 Hz, so the span is the label. Every other mode here spans the
    // width directly. Sign does not matter: a passband below the carrier is as
    // wide as the mirror of it above.
    Q_UNUSED(mode);
    return std::abs(hi - lo);
}

} // namespace AetherSDR::ModeFilters
