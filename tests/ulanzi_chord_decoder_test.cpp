// Ulanzi Dial chord decoder — press/release balance under adversarial event
// ordering.
//
// The dial's buttons can be bound to momentary transmit actions (PTT hold, CW
// straight key and paddles), which key on press and un-key on release.  A
// swallowed release therefore strands the transmitter, so the invariant these
// cases exist to pin is:
//
//     every emitted press is followed by a matching release
//
// Break it on purpose to see the test earn its keep: drop the
// `m_lastNonModKey != -1` close-out in the chord-press branch of
// UlanziChordDecoder::feed() and "two chord keys" fails; drop the latch
// disarm in the chord-release branch and "Ctrl bounce" fails.

#include "core/UlanziChordDecoder.h"

#include <QString>

#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

constexpr int kRelease = 0;
constexpr int kPress   = 1;
constexpr int kRepeat  = 2;

struct Recorder {
    UlanziChordDecoder decoder;
    std::vector<std::pair<QString, int>> buttons;
    int tuneSteps = 0;

    void feed(int key, int value)
    {
        for (const auto& ev : decoder.feed(key, value)) {
            if (ev.kind == UlanziChordDecoder::Event::Kind::Tune)
                tuneSteps += ev.tuneSteps;
            else
                buttons.emplace_back(ev.signature, ev.action);
        }
    }

    void feedAll(const std::vector<std::pair<int, int>>& seq)
    {
        for (const auto& kv : seq) feed(kv.first, kv.second);
    }

    // The safety invariant: no signature is left pressed, and no release
    // arrives for a signature that was not pressed.
    bool balanced() const
    {
        std::vector<QString> held;
        for (const auto& b : buttons) {
            if (b.second == kPress) {
                held.push_back(b.first);
            } else {
                auto it = std::find(held.begin(), held.end(), b.first);
                if (it == held.end()) return false;   // release with no press
                held.erase(it);
            }
        }
        return held.empty();                          // press with no release
    }

    QString trace() const
    {
        QString out;
        for (const auto& b : buttons)
            out += b.first + (b.second == kPress ? QStringLiteral(":down ")
                                                 : QStringLiteral(":up "));
        return out.trimmed();
    }

    bool sawPress(const QString& sig) const
    {
        for (const auto& b : buttons)
            if (b.first == sig && b.second == kPress) return true;
        return false;
    }
};

bool checkBalanced(Recorder& r, const char* label)
{
    const bool ok = r.balanced();
    if (!ok)
        std::cout << "       trace: " << r.trace().toStdString() << '\n';
    return expect(ok, label);
}

}  // namespace

int main()
{
    bool ok = true;
    const int CTRL = UlanziKey::LeftCtrl;
    const int Y = UlanziKey::Y;
    const int Z = UlanziKey::Z;
    const int PREV = UlanziKey::PreviousSong;
    const int MUTE = UlanziKey::Mute;

    // ---- bare keys -------------------------------------------------------
    {
        Recorder r;
        r.feedAll({{MUTE, kPress}, {MUTE, kRelease}});
        ok &= expect(r.buttons.size() == 2, "bare key emits exactly press + release");
        ok &= expect(r.buttons[0].first == QStringLiteral("KEY_MUTE") &&
                     r.buttons[0].second == kPress, "bare press carries the bare signature");
        ok &= checkBalanced(r, "bare key is balanced");
    }

    // ---- rotary ----------------------------------------------------------
    {
        Recorder r;
        r.feedAll({{UlanziKey::VolumeUp, kPress}, {UlanziKey::VolumeUp, kRepeat},
                   {UlanziKey::VolumeDown, kPress}});
        ok &= expect(r.tuneSteps == 1, "rotary counts autorepeat as a detent");
        ok &= expect(r.buttons.empty(), "rotary emits no button events");
    }

    // ---- single-key chord, key released first ----------------------------
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {Y, kRelease}, {CTRL, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("Ctrl+Y:down Ctrl+Y:up"),
                     "chord released before Ctrl emits one press/release pair");
        ok &= checkBalanced(r, "chord, key-first release is balanced");
    }

    // ---- single-key chord, Ctrl released first (the #4611 symptom) -------
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {CTRL, kRelease}, {Y, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("Ctrl+Y:down Ctrl+Y:up"),
                     "Ctrl-first release still emits the chord release, not a bare one");
        ok &= checkBalanced(r, "chord, Ctrl-first release is balanced");
    }

    // ---- two chord keys held together ------------------------------------
    // Both physical orders: the operator can press the two adjacent side
    // buttons at once, and either can come up first.
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {Z, kPress},
                   {Z, kRelease}, {Y, kRelease}, {CTRL, kRelease}});
        ok &= expect(r.sawPress(QStringLiteral("Ctrl+Y")) &&
                     r.sawPress(QStringLiteral("Ctrl+Z")), "two chord keys both press");
        ok &= checkBalanced(r, "two chord keys, second released first, is balanced");
    }
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {Z, kPress},
                   {Y, kRelease}, {Z, kRelease}, {CTRL, kRelease}});
        ok &= checkBalanced(r, "two chord keys, first released first, is balanced");
    }

    // ---- Ctrl bounce while a chord key stays held -------------------------
    // Ctrl up (synthesises the chord release), Ctrl down again, then the key
    // up.  The suppression latch must not survive into the next bare press.
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {CTRL, kRelease},
                   {CTRL, kPress}, {Y, kRelease}, {CTRL, kRelease}});
        ok &= checkBalanced(r, "Ctrl bounce mid-chord is balanced");

        r.buttons.clear();
        r.feedAll({{Y, kPress}, {Y, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("KEY_21:down KEY_21:up"),
                     "a later bare press of the same key keeps its release");
        ok &= checkBalanced(r, "bare press after a Ctrl bounce is balanced");
    }

    // ---- trailing bare release after a Ctrl-first chord is swallowed -----
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}, {CTRL, kRelease}});
        r.buttons.clear();
        r.feedAll({{Y, kRepeat}, {Y, kRelease}});
        ok &= expect(r.buttons.empty(),
                     "the physical key-up after a synthesised chord release is suppressed");
    }

    // ---- Mode Cycle compound (Ctrl+Y+KEY_PREVIOUSSONG) -------------------
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {PREV, kPress}, {Y, kPress},
                   {Y, kRelease}, {PREV, kRelease}, {CTRL, kRelease}});
        ok &= expect(r.sawPress(QStringLiteral("Ctrl+Y+KEY_PREVIOUSSONG")),
                     "PREVIOUSSONG inside the chord window decorates the signature");
        ok &= checkBalanced(r, "compound chord is balanced");
    }
    {
        // ...and PREVIOUSSONG on its own is still an ordinary bare key.
        Recorder r;
        r.feedAll({{PREV, kPress}, {PREV, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("KEY_PREVIOUSSONG:down KEY_PREVIOUSSONG:up"),
                     "bare PREVIOUSSONG is a plain button");
    }

    // ---- reset() drops a key held across a disconnect --------------------
    // A key still down when the dial unplugs leaves no chord state behind, so
    // the next session starts clean.  The stale key-up that may arrive first
    // surfaces as a bare release with no press — the harmless direction (every
    // momentary consumer ignores a release it never pressed), and the opposite
    // of the stuck-TX failure the balance invariant guards against.
    {
        Recorder r;
        r.feedAll({{CTRL, kPress}, {Y, kPress}});
        r.decoder.reset();
        r.buttons.clear();
        r.feedAll({{Y, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("KEY_21:up"),
                     "after reset a stale key-up degrades to a harmless bare release");

        r.buttons.clear();
        r.feedAll({{Y, kPress}, {Y, kRelease}});
        ok &= expect(r.trace() == QStringLiteral("KEY_21:down KEY_21:up"),
                     "after reset the next press/release pair is clean");
        ok &= checkBalanced(r, "post-reset press/release is balanced");
    }

    std::cout << (ok ? "ALL PASS" : "FAILURES") << '\n';
    return ok ? 0 : 1;
}
