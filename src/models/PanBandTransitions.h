#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// Band changes this client has sent to each pan whose reply has not arrived.
// A Flex answers `display pan set … band=` with the new band's saved pan and
// waterfall status (its own fps= and line_duration=) and then the reply, so
// until the reply the client still holds the band it left as the pan's
// rates. While a pan is in flight the adaptive throttle neither restores
// those rates (they would land on the new band) nor caps it (the radio would
// save the cap there); the cap goes on when the last reply settles (#6791).
//
// Each begin() hands out a token, and only a live token settles. A reply
// that comes back after forget() or clear() (pan closed, disconnect), or
// after its pan ID was reused, therefore changes nothing.
class PanBandTransitions {
public:
    quint64 begin(const QString& panId)
    {
        const quint64 token = ++m_lastToken;
        m_open[panId].insert(token);
        return token;
    }

    // True when this settles the pan's last outstanding band change.
    bool settle(const QString& panId, quint64 token)
    {
        auto it = m_open.find(panId);
        if (it == m_open.end() || !it->remove(token)) {
            return false;
        }
        if (!it->isEmpty()) {
            return false;
        }
        m_open.erase(it);
        return true;
    }

    bool inFlight(const QString& panId) const { return m_open.contains(panId); }
    void forget(const QString& panId) { m_open.remove(panId); }
    void clear() { m_open.clear(); }

private:
    QHash<QString, QSet<quint64>> m_open;
    quint64 m_lastToken{0};
};

} // namespace AetherSDR
