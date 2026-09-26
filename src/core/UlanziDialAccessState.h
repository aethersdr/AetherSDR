#pragma once

#include <QString>

namespace AetherSDR {

// Whether a recognized Ulanzi Dial is present but cannot be opened — the
// Linux case where its /dev/input node is root:input 0660 and the udev access
// rule is not installed. Pure state, no device and no event loop, so it is
// unit-testable on every platform (ulanzi_chord_decoder_test).
//
// This is STATE, not an event. The mapper dialog is usually opened long after
// the backend first scanned (a paired dial is found at launch), so a one-shot
// "access required" signal reaches nobody and the dialog's Grant access button
// never appears. The backend therefore keeps the current blocked name, answers
// the dialog when it asks, and re-announces after the dial goes away and comes
// back blocked.
class UlanziDialAccessState {
public:
    // Record one scan's outcome: the blocked device name, or empty when the
    // dial is either absent or opened. Returns true when this scan newly
    // blocks — the moment to warn and tell any open dialog.
    bool update(const QString& blockedNameNow)
    {
        if (blockedNameNow == m_blockedName) {
            return false;
        }
        m_blockedName = blockedNameNow;
        return !m_blockedName.isEmpty();
    }

    bool blocked() const { return !m_blockedName.isEmpty(); }
    QString blockedName() const { return m_blockedName; }

private:
    QString m_blockedName;
};

}  // namespace AetherSDR
