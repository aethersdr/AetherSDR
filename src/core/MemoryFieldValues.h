#pragma once

#include <QString>
#include <QStringList>

// Canonical value lists and sanitization for memory-channel fields, mirroring
// FlexLib Slice.cs / Memory.cs: demod modes; FMTXOffsetDirection simplex/up/down;
// FMToneMode off/ctcss_tx (ctcss_txrx reserved, not exposed). Display form is
// upper-case; wire form is upper-case modes, lower-case offset/tone. Helpers
// convert so GUI, radio I/O and CSV agree.
namespace AetherSDR::MemoryFields {

// Strip characters that corrupt the radio protocol and downstream tooling:
// all C0 control characters (0x00-0x1F, including the NUL bytes that have been
// observed in imported/hand-entered Mode fields) and the lone DEL byte (0x7f),
// which the protocol reserves as the on-the-wire encoding for a space. Callers
// that need a literal space preserved must decode 0x7f -> ' ' before calling.
QString sanitizeText(const QString& in);

// Canonical demodulation modes the FlexRadio supports, in operator-friendly
// order. Upper-case is both the display and the wire form for modes.
const QStringList& modes();

// FM repeater TX offset directions, display (upper-case) form.
const QStringList& offsetDirectionsDisplay();

// FM tone modes, display (upper-case) form.
const QStringList& toneModesDisplay();

// Standard EIA CTCSS tone frequencies as "%.1f" strings (e.g. "88.5").
const QStringList& ctcssTones();

// Common tuning-step suggestions in Hz, as strings.
const QStringList& tuningSteps();

// True if `mode` (compared case-insensitively) is one the radio supports.
bool isKnownMode(const QString& mode);

// Offset direction: any-case in -> canonical lower-case wire form
// ("simplex"/"up"/"down"); empty if unrecognized.
QString offsetDirToWire(const QString& any);
// Offset direction: any-case in -> upper-case display form; defaults to
// "SIMPLEX" when unrecognized.
QString offsetDirToDisplay(const QString& any);

// Tone mode: any-case in -> lower-case wire form ("off"/"ctcss_tx"); empty if
// unrecognized.
QString toneModeToWire(const QString& any);
// Tone mode: any-case in -> upper-case display form; defaults to "OFF".
QString toneModeToDisplay(const QString& any);

// Mode: sanitize and upper-case (the wire form). Does not validate membership.
QString modeToWire(const QString& any);

} // namespace AetherSDR::MemoryFields
