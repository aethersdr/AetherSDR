#pragma once
#include <QString>
#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR {
// Worker-owned text finalization. Only the optional carry strategy retains state
// beyond the existing time watermark; no model output or audio is kept here.
class DeepFistCommitter {
public:
    struct Token { int id; double seconds; QString text; };
    double committed() const { return m_committed; }
    std::size_t pendingCount() const { return m_pending.size(); }
    QString process(const std::vector<Token>& tokens, bool gated, double end,
                    double settled, bool carry)
    {
        QString output;
        if (gated) {
            bool pendingTail = false;
            if (carry) {
                for (const Pending& token : m_pending) {
                    if (token.seen >= 2 && token.seconds > m_committed
                        && end - token.last <= 1.21) {
                        if (token.seconds <= settled) { output += token.text; }
                        else { pendingTail = true; }
                    }
                }
            }
            // Close the word only after its confirmed tail has settled or
            // expired; a separator cannot be taken back after publication.
            if (!pendingTail && !m_idle) { output += QLatin1Char(' '); m_idle = true; }
        } else {
            m_idle = false;
            std::vector<Pending> next;
            for (const Token& token : tokens) {
                if (token.seconds > m_committed && token.seconds <= settled) { output += token.text; }
                if (!carry || token.seconds <= std::max(m_committed, settled) || next.size() >= 128) { continue; }
                auto match = m_pending.end();
                double distance = 0.16;
                for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                    const double difference = std::abs(it->seconds - token.seconds);
                    if (it->id == token.id && difference <= distance && end - it->last <= 0.81) {
                        match = it;
                        distance = difference;
                    }
                }
                const int seen = match == m_pending.end() ? 1 : std::min(2, match->seen + 1);
                next.push_back({{token.id, token.seconds, token.text}, seen, end});
                // One-to-one matching preserves repeated characters as separate events.
                if (match != m_pending.end()) { m_pending.erase(match); }
            }
            m_pending = std::move(next);
        }
        m_committed = std::max(m_committed, settled);
        std::erase_if(m_pending, [&](const Pending& token) {
            return token.seconds <= m_committed || end - token.last > 1.21;
        });
        return output;
    }
private:
    struct Pending : Token { int seen; double last; };
    double m_committed = 0;
    // Starts idle: a committer that has published nothing owes no separator.
    bool m_idle = true;
    std::vector<Pending> m_pending;
};
}
