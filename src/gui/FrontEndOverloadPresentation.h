#pragma once

// HOW THE FRONT-END STATE IS SAID, in three registers: a lamp, a line of text,
// and what a screen reader is told.
//
// SEPARATED FROM THE WIDGET SO IT CAN BE TESTED. Everything here is a pure
// function of AetherSDR::FrontEndOverload -- no Qt widgets, no painting, no
// clock. The widget draws what these return and owns no rules of its own. That
// division is what lets front_end_overload_presentation_test assert the
// behaviour RFC #5535 made a merge condition without instantiating a GUI.
//
// MODELLED ON THE RADIO'S OWN LEDS, which is #5535's wording and not a
// decoration: an operator watching an HL2 already reads clipping off the board,
// so the indicator that replaces that glance should not require learning a new
// vocabulary. Dark when there is nothing to say, green when clean, amber when it
// is starting, red when it is bad -- and red LATCHES BRIEFLY, because a
// converter that rails for 200 ms and recovers is exactly the event a glance
// would miss.

#include "core/backends/FrontEndOverload.h"

#include <QCoreApplication>
#include <QString>

namespace AetherSDR::gui {

enum class LampColour { Dark, Green, Amber, Red };

[[nodiscard]] inline LampColour lampFor(FrontEndLevel level)
{
    switch (level) {
    case FrontEndLevel::Unobserved: return LampColour::Dark;
    case FrontEndLevel::Clean:      return LampColour::Green;
    case FrontEndLevel::Marginal:   return LampColour::Amber;
    case FrontEndLevel::Hot:        return LampColour::Red;
    case FrontEndLevel::AtFloor:    return LampColour::Red;
    }
    return LampColour::Dark;
}

// THE REGULATOR'S ACTION, as a short suffix, or empty when there is none to
// report. "-6 dB" is the whole point of the second half of #5535's condition:
// the operator must be able to see that something moved their gain.
[[nodiscard]] inline QString offsetText(const FrontEndOverload& s)
{
    if (!s.autoArmed || s.autoOffsetDb <= 0) {
        return {};
    }
    return QCoreApplication::translate("FrontEndOverload", "−%1 dB")
        .arg(s.autoOffsetDb);
}

// The line beside the lamp. Deliberately short -- it sits next to the RF Gain
// slider, not in a dialog.
[[nodiscard]] inline QString shortText(const FrontEndOverload& s)
{
    const auto tr_ = [](const char* k) {
        return QCoreApplication::translate("FrontEndOverload", k);
    };
    QString head;
    switch (s.level) {
    case FrontEndLevel::Unobserved: head = tr_("No ADC reading"); break;
    case FrontEndLevel::Clean:      head = tr_("Clean");          break;
    case FrontEndLevel::Marginal:   head = tr_("Clipping");       break;
    case FrontEndLevel::Hot:        head = tr_("Clipping hard");  break;
    case FrontEndLevel::AtFloor:    head = tr_("At floor");       break;
    }
    const QString off = offsetText(s);
    return off.isEmpty() ? head : QStringLiteral("%1  %2").arg(head, off);
}

// WHAT A SCREEN READER IS TOLD, which is not the same string. The lamp carries
// colour and the line is abbreviated for space; neither survives being read
// aloud, so this spells out the state, the regulator's action and the backend's
// own reason in one sentence. docs/a11y.md asks for exactly this rather than a
// terse label that happens to be technically present.
[[nodiscard]] inline QString accessibleText(const FrontEndOverload& s)
{
    const auto tr_ = [](const char* k) {
        return QCoreApplication::translate("FrontEndOverload", k);
    };
    QString out;
    switch (s.level) {
    case FrontEndLevel::Unobserved:
        out = tr_("Front end: no converter reading available");
        break;
    case FrontEndLevel::Clean:
        out = tr_("Front end clean");
        break;
    case FrontEndLevel::Marginal:
        out = tr_("Front end clipping occasionally");
        break;
    case FrontEndLevel::Hot:
        out = tr_("Front end clipping most of the time");
        break;
    case FrontEndLevel::AtFloor:
        out = tr_("Front end still clipping at the automatic gain floor. "
                  "Attenuation or a filter ahead of the radio is needed.");
        break;
    }
    if (s.autoArmed && s.autoOffsetDb > 0) {
        out += QStringLiteral(". ")
            + tr_("Automatic gain is holding %1 dB below your setting")
                  .arg(s.autoOffsetDb);
    } else if (s.autoArmed) {
        out += QStringLiteral(". ") + tr_("Automatic gain is armed and holding");
    }
    if (!s.reason.isEmpty()) {
        out += QStringLiteral(". ") + s.reason;
    }
    return out;
}

// WHETHER A CHANGE IS WORTH INTERRUPTING A SCREEN READER FOR.
//
// A polite announcement on every 10 Hz telemetry window would make the radio
// unusable with a screen reader, which is a worse a11y outcome than saying
// nothing. So only TRANSITIONS THAT MATTER speak: becoming un-clean, reaching
// the floor, and recovering to clean. Movement within clipping (Marginal to Hot
// and back) does not re-announce, and neither does the offset changing on its
// own -- both stay readable on demand through accessibleText().
[[nodiscard]] inline bool shouldAnnounce(FrontEndLevel before, FrontEndLevel after)
{
    if (before == after) {
        return false;
    }
    const auto clipping = [](FrontEndLevel l) {
        return l == FrontEndLevel::Marginal || l == FrontEndLevel::Hot
            || l == FrontEndLevel::AtFloor;
    };
    if (after == FrontEndLevel::AtFloor) {
        return true;   // the state software cannot fix
    }
    if (!clipping(before) && clipping(after)) {
        return true;   // it started
    }
    if (clipping(before) && after == FrontEndLevel::Clean) {
        return true;   // it stopped
    }
    return false;
}

}  // namespace AetherSDR::gui
