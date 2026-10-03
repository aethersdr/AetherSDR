#pragma once
#include <QString>
#include <algorithm>
#include <cmath>
#include <vector>

namespace AetherSDR {
// Worker-owned text finalization. Only the optional carry strategy retains state
// beyond the existing time watermark; no model output or audio is kept here,
// except each token's confidence from the last few seconds of windows.
class DeepFistCommitter {
public:
    // confidence: the model's posterior for this token in one window, 0..1.
    struct Token { int id; double seconds; QString text; float confidence = 1.f; };
    // One published run of text and the confidence it is shown with.
    struct Piece { QString text; float confidence; };
    double committed() const { return m_committed; }
    std::size_t pendingCount() const { return m_pending.size(); }
    QString process(const std::vector<Token>& tokens, bool gated, double end,
                    double settled, bool carry, std::vector<Piece>* pieces = nullptr)
    {
        QString output;
        // A letter is re-decoded in every window it stays in (~3 before it
        // settles). Its confidence is the mean over those sightings: one per
        // earlier window, same token within 0.12 s.
        const auto publish = [&](const Token& token, bool current) {
            output += token.text;
            if (!pieces) { return; }
            double sum = current ? token.confidence : 0.0;
            int count = current ? 1 : 0;
            std::vector<double> ticks;
            for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
                if (it->id != token.id || std::abs(it->seconds - token.seconds) > 0.12
                    || std::find(ticks.begin(), ticks.end(), it->end) != ticks.end()) { continue; }
                ticks.push_back(it->end);
                sum += it->confidence;
                ++count;
            }
            pieces->push_back({token.text, count ? static_cast<float>(sum / count) : token.confidence});
        };
        const auto separate = [&] {
            output += QLatin1Char(' ');
            if (pieces) { pieces->push_back({QStringLiteral(" "), 1.f}); }
        };
        if (gated) {
            bool pendingTail = false;
            if (carry) {
                for (const Pending& token : m_pending) {
                    if (token.seen >= 2 && token.seconds > m_committed
                        && end - token.last <= 1.21) {
                        if (token.seconds <= settled) { publish(token, false); }
                        else { pendingTail = true; }
                    }
                }
            }
            // Close the word only after its confirmed tail has settled or
            // expired; a separator cannot be taken back after publication.
            if (!pendingTail && !m_idle) { separate(); m_idle = true; }
        } else {
            m_idle = false;
            std::vector<Pending> next;
            for (const Token& token : tokens) {
                if (token.seconds > m_committed && token.seconds <= settled) { publish(token, true); }
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
                next.push_back({token, seen, end});
                // One-to-one matching preserves repeated characters as separate events.
                if (match != m_pending.end()) { m_pending.erase(match); }
            }
            m_pending = std::move(next);
            for (const Token& token : tokens) { m_history.push_back({token.id, token.seconds, token.confidence, end}); }
        }
        std::erase_if(m_history, [&](const Sighting& sighting) { return end - sighting.end > 8.0; });
        m_committed = std::max(m_committed, settled);
        std::erase_if(m_pending, [&](const Pending& token) {
            return token.seconds <= m_committed || end - token.last > 1.21;
        });
        return output;
    }
private:
    struct Pending : Token { int seen; double last; };
    struct Sighting { int id; double seconds; float confidence; double end; };
    double m_committed = 0;
    // Starts idle: a committer that has published nothing owes no separator.
    bool m_idle = true;
    std::vector<Pending> m_pending;
    std::vector<Sighting> m_history;
};
}
