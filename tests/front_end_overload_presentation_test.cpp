// The visible half of RFC #5535, asserted without a GUI.
//
// #5535 approved the automatic RF-gain loop ON THE CONDITION that it is visible,
// and named two things that must be: the clipping, and the regulator's own
// action. This file pins both, plus the rule that keeps the second one from
// making the radio unusable with a screen reader.
//
// Socket-free and widget-free: everything under test is a pure function of
// AetherSDR::FrontEndOverload. The widget draws what these return and owns no
// rules of its own except the red latch, which needs a clock.

#include "gui/FrontEndOverloadPresentation.h"

#include <QCoreApplication>

#include <cstdio>

using AetherSDR::FrontEndLevel;
using AetherSDR::FrontEndOverload;
using namespace AetherSDR::gui;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("[%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond)
        ++g_failures;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── The lamp, modelled on the radio's own LEDs ───────────────────────
    check(lampFor(FrontEndLevel::Unobserved) == LampColour::Dark,
          "no reading is DARK, not green -- an indicator must not reassure "
          "about a measurement nobody took");
    check(lampFor(FrontEndLevel::Clean) == LampColour::Green, "clean is green");
    check(lampFor(FrontEndLevel::Marginal) == LampColour::Amber,
          "clipping occasionally is amber");
    check(lampFor(FrontEndLevel::Hot) == LampColour::Red,
          "clipping most of the time is red");
    check(lampFor(FrontEndLevel::AtFloor) == LampColour::Red,
          "and at the floor is red too");

    // ── THE REGULATOR'S OWN ACTION, which is #5535's second condition ────
    {
        FrontEndOverload s;
        s.autoArmed = true;
        s.autoOffsetDb = 6;
        check(offsetText(s).contains(QStringLiteral("6")),
              "an armed loop holding 6 dB down SAYS SO -- without this, "
              "\"my noise floor moved and I touched nothing\" comes back");
        check(shortText(s).contains(QStringLiteral("6")),
              "and the offset reaches the visible line, not just the tooltip");

        s.autoOffsetDb = 0;
        check(offsetText(s).isEmpty(),
              "an armed loop holding nothing back reports no offset");

        s.autoArmed = false;
        s.autoOffsetDb = 6;
        check(offsetText(s).isEmpty(),
              "a disarmed loop reports no offset even with a stale number");
    }

    // ── The spoken form is not the written one ───────────────────────────
    {
        FrontEndOverload s;
        s.level = FrontEndLevel::AtFloor;
        s.autoArmed = true;
        s.autoOffsetDb = 12;
        const QString spoken = accessibleText(s);
        check(spoken.length() > shortText(s).length(),
              "the screen reader gets more than the abbreviated line");
        check(spoken.contains(QStringLiteral("12")),
              "including the regulator's action");
        check(spoken.contains(QStringLiteral("ttenuation")),
              "and at the floor it says what the operator must actually do, "
              "because no amount of gain management fixes this one");

        FrontEndOverload u;
        check(accessibleText(u).contains(QStringLiteral("no converter reading")),
              "no reading is spoken as absent, not as clean");
    }

    {
        FrontEndOverload s;
        s.level = FrontEndLevel::Clean;
        s.autoArmed = true;
        check(accessibleText(s).contains(QStringLiteral("armed")),
              "an armed loop at zero offset is still announced as armed -- "
              "\"no offset\" and \"no loop\" are different states");
    }

    {
        FrontEndOverload s;
        s.level = FrontEndLevel::Marginal;
        s.reason = QStringLiteral("reducing gain — clipping occasionally");
        check(accessibleText(s).contains(s.reason),
              "the backend's own words are carried through, not re-invented "
              "above the seam");
    }

    // ── WHAT IS WORTH INTERRUPTING A SCREEN READER FOR ───────────────────
    //
    // The inputs move at 10 Hz. A polite announcement on every window would
    // make the radio unusable with a screen reader, which is a WORSE a11y
    // outcome than saying nothing -- so only the transitions that matter speak.
    check(!shouldAnnounce(FrontEndLevel::Clean, FrontEndLevel::Clean),
          "no change says nothing");
    check(shouldAnnounce(FrontEndLevel::Clean, FrontEndLevel::Marginal),
          "it started clipping -- speak");
    check(shouldAnnounce(FrontEndLevel::Clean, FrontEndLevel::Hot),
          "it started clipping hard -- speak");
    check(shouldAnnounce(FrontEndLevel::Hot, FrontEndLevel::Clean),
          "it stopped -- speak, so the operator is not left believing the "
          "warning still stands");
    check(shouldAnnounce(FrontEndLevel::Hot, FrontEndLevel::AtFloor),
          "reaching the floor always speaks: it is the state software cannot fix");
    check(shouldAnnounce(FrontEndLevel::Marginal, FrontEndLevel::AtFloor),
          "from either direction");

    check(!shouldAnnounce(FrontEndLevel::Marginal, FrontEndLevel::Hot),
          "movement WITHIN clipping does not re-announce -- it is the same "
          "news, and at 10 Hz it would be chatter");
    check(!shouldAnnounce(FrontEndLevel::Hot, FrontEndLevel::Marginal),
          "nor does it on the way back down");
    check(!shouldAnnounce(FrontEndLevel::Unobserved, FrontEndLevel::Clean),
          "a first reading arriving is not an event worth speaking over");
    check(!shouldAnnounce(FrontEndLevel::Clean, FrontEndLevel::Unobserved),
          "and losing the reading is not either -- the lamp goes dark, which "
          "is visible, and the radio is probably just gone");

    if (g_failures == 0)
        std::printf("front_end_overload_presentation_test: all checks passed\n");
    else
        std::printf("front_end_overload_presentation_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
