#pragma once

#include <QHash>
#include <QString>

#include <optional>

namespace AetherSDR {

// Per-instance dial frequency for the WSJT-X UDP feed (#3595). Decodes carry
// only the audio offset (0–5000 Hz); the dial comes in each instance's Status,
// and instances (id "WSJT-X", "WSJT-X - 2", …) share the port, so one global
// dial would misplace spots. A decode from an instance with no dial yet is
// refused (nullopt). Header-only, Qt-Core-only for testing.
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
