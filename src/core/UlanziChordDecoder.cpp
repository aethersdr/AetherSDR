#include "UlanziChordDecoder.h"

namespace AetherSDR {

QString UlanziChordDecoder::bareKeySignature(int keycode)
{
    switch (keycode) {
        case UlanziKey::PlayPause:    return QStringLiteral("KEY_PLAYPAUSE");
        case UlanziKey::Mute:         return QStringLiteral("KEY_MUTE");
        case UlanziKey::PreviousSong: return QStringLiteral("KEY_PREVIOUSSONG");
        case UlanziKey::NextSong:     return QStringLiteral("KEY_NEXTSONG");
        default:                      return QStringLiteral("KEY_%1").arg(keycode);
    }
}

QString UlanziChordDecoder::chordSignature(int keycode, bool withPreviousSong)
{
    QString letter;
    switch (keycode) {
        case UlanziKey::V: letter = QStringLiteral("V"); break;
        case UlanziKey::C: letter = QStringLiteral("C"); break;
        case UlanziKey::Y: letter = QStringLiteral("Y"); break;
        case UlanziKey::Z: letter = QStringLiteral("Z"); break;
        default:           letter = QStringLiteral("KEY_%1").arg(keycode); break;
    }
    if (withPreviousSong)
        return QStringLiteral("Ctrl+%1+KEY_PREVIOUSSONG").arg(letter);
    return QStringLiteral("Ctrl+%1").arg(letter);
}

void UlanziChordDecoder::reset()
{
    m_ctrlDown = false;
    m_lastNonModKey = -1;
    m_prevsongAlongsideCtrl = false;
    m_suppressReleaseNonModKey = -1;
    m_suppressReleasePrevsong = false;
}

QList<UlanziChordDecoder::Event> UlanziChordDecoder::feed(int linuxKey, int value)
{
    QList<Event> out;
    const auto tune = [&out](int steps) {
        Event e;
        e.kind = Event::Kind::Tune;
        e.tuneSteps = steps;
        out.push_back(e);
    };
    const auto button = [&out](const QString& signature, int action) {
        Event e;
        e.kind = Event::Kind::Button;
        e.signature = signature;
        e.action = action;
        out.push_back(e);
    };

    // Rotary: one tick per press or autorepeat.  The ~30 Hz autorepeat rate
    // from the dial firmware gives a natural acceleration feel under
    // continuous rotation.  (Backends that poll HID never see autorepeat and
    // synthesise one press per detent instead — same result.)
    if (linuxKey == UlanziKey::VolumeUp) {
        if (value == 1 || value == 2) tune(+1);
        return out;
    }
    if (linuxKey == UlanziKey::VolumeDown) {
        if (value == 1 || value == 2) tune(-1);
        return out;
    }

    if (linuxKey == UlanziKey::LeftCtrl || linuxKey == UlanziKey::RightCtrl) {
        m_ctrlDown = (value != 0);
        if (!m_ctrlDown) {
            // Ctrl released — chord window closing.  A chord key still held
            // gets its release now, so listeners never miss it when Ctrl comes
            // up before the non-mod key.  Its physical key-up follows as a
            // *bare* event and is swallowed by the latch below.
            if (m_lastNonModKey != -1)
                button(chordSignature(m_lastNonModKey, m_prevsongAlongsideCtrl), 0);
            m_lastNonModKey = -1;
            m_prevsongAlongsideCtrl = false;
        }
        return out;
    }

    // PREVIOUSSONG can arrive alongside a Ctrl chord (Mode Cycle emits
    // Ctrl+Y+KEY_PREVIOUSSONG as a compound), so inside a chord window it only
    // decorates the signature rather than acting as a key of its own.
    if (linuxKey == UlanziKey::PreviousSong) {
        if (m_ctrlDown) {
            if (value == 1) {
                m_prevsongAlongsideCtrl = true;
                m_suppressReleasePrevsong = true;
            } else if (value == 0) {
                m_suppressReleasePrevsong = false;
            }
            return out;
        }
        if (m_suppressReleasePrevsong) {
            if (value == 0) {
                m_suppressReleasePrevsong = false;
                return out;   // trailing release from the chord window
            }
            if (value == 2)
                return out;   // still held; autorepeat is not a fresh press
        }
        if (value == 1 || value == 0)
            button(bareKeySignature(linuxKey), value);
        return out;
    }

    // Bare key press/release (no Ctrl).
    if (!m_ctrlDown) {
        if (linuxKey == m_suppressReleaseNonModKey) {
            if (value == 0) {
                m_suppressReleaseNonModKey = -1;
                return out;   // trailing release from the chord window
            }
            if (value == 2)
                return out;   // still held; autorepeat is not a fresh press
        }
        if (value == 1 || value == 0)
            button(bareKeySignature(linuxKey), value);
        return out;
    }

    // Ctrl is down — this is a chord.
    if (value == 1) {
        // A second chord key pressed while the first is still held: close the
        // first one out now, so its press does not go unreleased.
        if (m_lastNonModKey != -1 && m_lastNonModKey != linuxKey)
            button(chordSignature(m_lastNonModKey, m_prevsongAlongsideCtrl), 0);
        m_lastNonModKey = linuxKey;
        m_suppressReleaseNonModKey = linuxKey;
        button(chordSignature(linuxKey, m_prevsongAlongsideCtrl), 1);
    } else if (value == 0) {
        if (linuxKey == m_lastNonModKey) {
            button(chordSignature(linuxKey, m_prevsongAlongsideCtrl), 0);
            m_lastNonModKey = -1;
            m_prevsongAlongsideCtrl = false;
        }
        // Disarm the latch whichever branch consumed the key-up.  Without
        // this, a Ctrl bounce (Ctrl up, Ctrl down again, key up) leaves it
        // armed and it swallows the release of an unrelated later bare press.
        if (linuxKey == m_suppressReleaseNonModKey)
            m_suppressReleaseNonModKey = -1;
    }
    return out;
}

}  // namespace AetherSDR
