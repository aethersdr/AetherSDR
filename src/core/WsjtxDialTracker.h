#pragma once

#include <QHash>
#include <QString>

#include <optional>

namespace AetherSDR {

// Per-instance dial-frequency memory for the WSJT-X UDP feed (#3595).
//
// A WSJT-X Decode datagram carries only the audio offset of the decoded
// signal (0–5000 Hz); the dial frequency it must be added to arrives
// separately, in that instance's Status datagram. Every WSJT-X message
// begins with the instance `id` ("WSJT-X", "WSJT-X - 2", …), and two
// instances sharing one UDP port interleave their traffic freely, so a
// single "last dial frequency seen" is wrong whenever more than one
// instance is running: a decode from the 40 m instance added to the 20 m
// instance's dial paints a 40 m station on the 20 m panadapter.
//
// This keeps one dial frequency per instance id. A decode whose instance
// has not yet reported a dial frequency cannot be placed on any band and
// is refused (nullopt) rather than guessed — WSJT-X emits a Status with
// every decode cycle, so in practice the only unplaceable decode is the
// first one after AetherSDR starts listening mid-cycle. Header-only and
// Qt-Core-only so it is testable without WsjtxClient's socket and logging
// dependencies.
class WsjtxDialTracker {
public:
    // Record the dial frequency `id` reported in its Status message.
    // Non-positive frequencies are ignored: WSJT-X reports 0 Hz while it has
    // no rig connection, and a 0 Hz dial would place every decode at the
    // audio offset itself.
    void noteStatus(const QString& id, double dialFreqHz)
    {
        if (dialFreqHz <= 0.0) {
            return;
        }
        m_dialFreqHzById.insert(id, dialFreqHz);
    }

    // The dial frequency to add to a Decode from `id`, or nullopt when that
    // instance has not reported one yet.
    std::optional<double> dialFreqHzFor(const QString& id) const
    {
        const auto it = m_dialFreqHzById.constFind(id);
        if (it == m_dialFreqHzById.constEnd()) {
            return std::nullopt;
        }
        return it.value();
    }

    // WSJT-X sends a Close (type 6) datagram on exit; forgetting the id then
    // keeps a relaunched instance from inheriting a stale band until its
    // first Status arrives.
    void forget(const QString& id) { m_dialFreqHzById.remove(id); }

    void clear() { m_dialFreqHzById.clear(); }

    int instanceCount() const { return static_cast<int>(m_dialFreqHzById.size()); }

private:
    QHash<QString, double> m_dialFreqHzById;
};

}  // namespace AetherSDR
