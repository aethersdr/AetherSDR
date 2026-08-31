#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringList>

#include <utility>

#include "core/dsp/WdspChannel.h"

// The HL2 RX mode table: operator mode string -> WDSP demod mode + default
// passband, plus the restore-boundary guard and the inverse name map. Pulled
// out of Hl2Backend.cpp's anon namespace so it is directly unit-testable
// (tests/hl2_mode_table_test.cpp) without a connected-backend fixture.
//
// Plan 2: RTTY and DFM stop falling through to a plain USB passband. RTTY is a
// filtered-SSB slice feeding the client-side RttyDecoder (its FSK front end
// runs downstream on rxDemodAudioReady), so it demodulates as USB but with a
// narrow passband centred on the decoder's mark/shift. DFM (FM data) is FM with
// an FM-width passband — the wrong-demod SSB fallback made an FM-data signal
// unlistenable and undecodable.

namespace AetherSDR::hl2 {

// The set of modes the HL2 backend genuinely supports, published on every
// SliceDelta so the VFO combo shows exactly these -- no silent fall-through,
// and no D-STAR / DRM / FreeDV entries the raw-IQ path cannot honour (no AMBE
// vocoder, no DRM decoder, no bus-B audio tap yet). "CW" is the app-wide name
// for upper-sideband CW; CWL still works if sent explicitly.
inline const QStringList& supportedModes()
{
    static const QStringList kModes = {
        QStringLiteral("USB"),  QStringLiteral("LSB"),  QStringLiteral("CW"),
        QStringLiteral("AM"),   QStringLiteral("SAM"),  QStringLiteral("FM"),
        QStringLiteral("NFM"),  QStringLiteral("DIGU"), QStringLiteral("DIGL"),
        QStringLiteral("RTTY"), QStringLiteral("DFM"),
    };
    return kModes;
}

inline WdspChannel::Mode modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    if (u == QLatin1String("CWL"))  return WdspChannel::Mode::Cwl;
    // "CW" is the upper-sideband CW mode name the rest of the app uses (it is
    // what TciProtocol::tciToSmartSDR produces for TCI's `cw`, and what a Flex
    // reports); "CWU" is the explicit spelling. Only CWU was listed, so plain CW
    // fell through to the USB fallback below and was demodulated as SSB -- the
    // mode indicator read CW while the passband and detector were not.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) {
        return WdspChannel::Mode::Cwu;
    }
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) {
        return WdspChannel::Mode::Fm;
    }
    // DFM (FM data): FM demod, differing from NFM only in that program material
    // is data and wants the passband flat. This backend has no de-emphasis
    // toggle, so DFM demodulates identically to FM here — but that is FM, not
    // the SSB the USB fallback used to give an FM-data signal.
    if (u == QLatin1String("DFM")) {
        return WdspChannel::Mode::Fm;
    }
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    // RTTY: a narrow USB slice. The Baudot decode is client-side
    // (RttyDecoder, on rxDemodAudioReady), so WDSP just needs to hand it clean
    // audio around the decoder's mark/space tones -- see defaultPassbandForMode.
    if (u == QLatin1String("RTTY")) {
        return WdspChannel::Mode::Usb;
    }
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    // No DRM entry: WDSP's DRM mode only sets the demod up for an EXTERNAL DRM
    // decoder, and there is none in the tree. It is not advertised and maps to
    // the USB fallback if forced, rather than to a mode with nothing behind it.
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) {
        return WdspChannel::Mode::Wbfm;
    }
    return WdspChannel::Mode::Usb;
}

// Is `mode` a name modeFromString() genuinely maps (rather than falling back to
// USB)? The restore boundary uses this so a corrupt document's mode string is
// dropped instead of reaching Receiver::mode, the UI, and -- via capture --
// re-persisting itself (PR #4619 review, Ozy311). It is the advertised set plus
// the alias/legacy spellings modeFromString() also accepts but the combo does
// not feature (explicit CW sidebands, double-sideband, wideband FM). No DRM:
// there is no decoder, so a restored "DRM" is dropped like any other mode this
// backend cannot honour.
inline bool isKnownModeString(const QString& mode) noexcept
{
    static const QStringList kLegacySpellings = {
        QStringLiteral("CWU"),  QStringLiteral("CWL"), QStringLiteral("DSB"),
        QStringLiteral("WBFM"), QStringLiteral("WFM"),
    };
    const QString u = mode.toUpper();
    return supportedModes().contains(u, Qt::CaseInsensitive)
           || kLegacySpellings.contains(u);
}

// WdspChannel::Mode -> canonical name, for the `dsp.get` readback. The inverse
// of modeFromString() for the values it maps 1:1; the two CW and the two FM
// spellings collapse to one name each, matching what the rest of the app uses.
inline QString wdspModeName(WdspChannel::Mode mode) noexcept
{
    switch (mode) {
    case WdspChannel::Mode::Lsb:  return QStringLiteral("LSB");
    case WdspChannel::Mode::Usb:  return QStringLiteral("USB");
    case WdspChannel::Mode::Dsb:  return QStringLiteral("DSB");
    case WdspChannel::Mode::Cwl:  return QStringLiteral("CWL");
    case WdspChannel::Mode::Cwu:  return QStringLiteral("CWU");
    case WdspChannel::Mode::Fm:   return QStringLiteral("FM");
    case WdspChannel::Mode::Am:   return QStringLiteral("AM");
    case WdspChannel::Mode::Digu: return QStringLiteral("DIGU");
    case WdspChannel::Mode::Spec: return QStringLiteral("SPEC");
    case WdspChannel::Mode::Digl: return QStringLiteral("DIGL");
    case WdspChannel::Mode::Sam:  return QStringLiteral("SAM");
    case WdspChannel::Mode::Drm:  return QStringLiteral("DRM");
    case WdspChannel::Mode::Wbfm: return QStringLiteral("WBFM");
    }
    return QStringLiteral("USB");
}

// Default RX passband per mode, in Hz relative to the carrier. Sign carries the
// sideband, matching SliceModel's convention (USB-family positive, LSB-family
// negative, carrier-straddling modes symmetric) -- a table with the wrong sign
// here would be silently "corrected" by SliceModel::normalizeFilterPolarity and
// the mistake would never surface.
//
// The digital entries are deliberately the widest of the set. DIGU is the mode
// WSJT-X selects, and it must pass the whole 3 kHz audio window the decoder
// expects; a snug SSB passband would clip the top of the FT8 sub-band and drop
// exactly the signals at the edges.
inline std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    // RTTY: a narrow USB window over RttyDecoder's default mark (2125 Hz) and
    // 170 Hz shift. 400 Hz spans the mark plus space in EITHER polarity (space
    // at 1955 normal-reverse or 2295 normal) with a little filter skirt, and no
    // more -- an adjacent RTTY signal a few hundred Hz off must not bleed into
    // the mark/space discriminator. Wider than the shift itself (170 Hz) but far
    // tighter than an SSB voice passband.
    if (u == QLatin1String("RTTY")) return {1950, 2350};
    // CW: 500 Hz CENTRED ON THE CARRIER, both sidebands, because in CW the
    // operator-facing passband is measured from the signal and not from the
    // audio it becomes. The pitch offset lives in the BFO (cwBfoHz) instead --
    // see the note there -- so CWU and CWL share one table entry and differ only
    // in which way the BFO leans.
    //
    // This is the convention the rest of the app already assumes:
    // VfoWidget::applyFilterPreset builds every CW preset as {-w/2, +w/2}
    // ("centred on carrier -- radio's BFO handles pitch offset"), and a Flex
    // reports CW cuts the same way (FlexLib Slice.cs clamps them to
    // +/-12000 - CWPitch, which only makes sense for cuts measured from the
    // carrier).
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {-250, 250};
    // Carrier-straddling modes: symmetric about the carrier, which the envelope
    // and synchronous detectors both need.
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    // FM and DFM (FM data) both take the FM-width window here.
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")
        || u == QLatin1String("DFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

}  // namespace AetherSDR::hl2
