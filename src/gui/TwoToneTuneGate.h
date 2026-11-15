#pragma once

#include "core/backends/RadioCapabilities.h"
#include "models/RadioModel.h"

#include <QString>

#include <functional>

namespace AetherSDR {

// Two Tone TUNE is only a two-tone signal on a backend that declares a
// generator (RadioCapabilities::twoToneGenerator, Flex alone). Elsewhere the
// same request keys a single carrier, so every road to it (TUNE menu entry,
// keyboard shortcut, MIDI action) asks this first. No radio connected is the
// permissive state, as for every other capability gate.
inline bool twoToneTuneAvailable(bool connected, const RadioCapabilities& caps)
{
    return !connected || caps.twoToneGenerator.has_value();
}

inline QString twoToneTuneUnavailableReason()
{
    return QStringLiteral("Unavailable: this radio has no two-tone generator, "
                          "so Two Tone TUNE would transmit a single carrier. "
                          "Use TUNE.");
}

// Called before any keying call. False means refuse: `refused` (optional)
// receives the reason to show the operator, and the caller must not start.
inline bool twoToneTuneMayStart(const RadioModel& radio,
                                const std::function<void(const QString&)>& refused)
{
    if (twoToneTuneAvailable(radio.isConnected(), radio.backendCapabilities())) {
        return true;
    }
    if (refused) {
        refused(twoToneTuneUnavailableReason());
    }
    return false;
}

} // namespace AetherSDR
