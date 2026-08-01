#pragma once

#include <QList>
#include <QString>

namespace AetherSDR {

// Linux keycode numeric values.  Every platform backend translates its native
// event source into this space before decoding, so the signatures the mapper
// dialog and MainWindow's dispatcher see are identical on Linux, macOS and
// Windows.  (Linux gets these from <linux/input.h>; the other two have no such
// header, which is why they are spelled out here rather than included.)
namespace UlanziKey {
constexpr int PlayPause    = 164;
constexpr int Mute         = 113;
constexpr int PreviousSong = 165;
constexpr int NextSong     = 163;
constexpr int VolumeUp     = 115;
constexpr int VolumeDown   = 114;
constexpr int LeftCtrl     = 29;
constexpr int RightCtrl    = 97;
constexpr int V            = 47;
constexpr int C            = 46;
constexpr int Y            = 21;
constexpr int Z            = 44;
}  // namespace UlanziKey

// Pure decoder for the Ulanzi Dial's key stream — no Qt object, no device, no
// event loop, so it is unit-testable on every platform (unlike the three
// backends that drive it, each of which only compiles on its own OS).
//
// The dial encodes its rotary as autorepeating volume keys, its top row and
// dial press as bare media keys, and its side buttons as Ctrl+key chords —
// with Mode Cycle arriving as a Ctrl+Y+KEY_PREVIOUSSONG compound.  feed()
// turns one native key transition into zero or more semantic events.
//
// **Every button press must be matched by a release.**  A momentary binding
// (PTT hold, CW keying) keys the transmitter on press and un-keys on release,
// so a swallowed release strands TX — Constitution Principle VI.  Two rules
// keep that invariant under adversarial event ordering:
//
//   - a chord key still held when the chord window closes (Ctrl up, or a
//     second chord key pressed on top of it) is released right then;
//   - the "trailing bare release" latch, which suppresses the physical key-up
//     that follows a synthesised chord release, is disarmed by whichever
//     branch actually consumes that key-up — otherwise a Ctrl bounce leaves it
//     armed against an unrelated later press.
//
// The opposite error — an occasional *extra* release — is harmless: every
// momentary consumer ignores a release it never pressed.
class UlanziChordDecoder {
public:
    struct Event {
        enum class Kind { Tune, Button };
        Kind    kind{Kind::Button};
        int     tuneSteps{0};   // Tune: +1 = clockwise, -1 = counter-clockwise
        QString signature;      // Button: e.g. "KEY_MUTE", "Ctrl+Y"
        int     action{0};      // Button: 1 = press, 0 = release
    };

    // Feed one key transition.  `value` follows the evdev convention:
    // 0 = release, 1 = press, 2 = autorepeat.  Backends that synthesise
    // transitions from polled HID reports only ever pass 0 or 1.
    QList<Event> feed(int linuxKey, int value);

    // Drop all chord state — call on disconnect so a key held across an
    // unplug cannot leak into the next session.
    void reset();

    static QString bareKeySignature(int keycode);
    static QString chordSignature(int keycode, bool withPreviousSong);

private:
    bool m_ctrlDown{false};
    int  m_lastNonModKey{-1};             // chord key currently held, if any
    bool m_prevsongAlongsideCtrl{false};  // KEY_PREVIOUSSONG seen in this chord window
    int  m_suppressReleaseNonModKey{-1};  // trailing bare release to swallow
    bool m_suppressReleasePrevsong{false};
};

}  // namespace AetherSDR
