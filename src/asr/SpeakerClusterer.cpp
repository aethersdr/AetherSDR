#include "asr/SpeakerClusterer.h"

#include <cmath>

namespace AetherSDR {

SpeakerClusterer::SpeakerClusterer(float threshold, int maxSpeakers)
    : m_threshold(threshold)
    , m_maxSpeakers(maxSpeakers > 0 ? maxSpeakers : 1)
{
}

void SpeakerClusterer::reset()
{
    m_centroids.clear();
    m_counts.clear();
}

int SpeakerClusterer::assign(const std::vector<float>& embedding)
{
    if (embedding.empty()) {
        return -1;
    }

    // Best cosine similarity (embeddings + centroids are unit vectors, so a dot).
    int best = -1;
    float bestSim = -2.0f;
    for (int i = 0; i < static_cast<int>(m_centroids.size()); ++i) {
        const std::vector<float>& c = m_centroids[i];
        if (c.size() != embedding.size()) {
            continue;
        }
        float dot = 0.0f;
        for (size_t k = 0; k < c.size(); ++k) {
            dot += c[k] * embedding[k];
        }
        if (dot > bestSim) {
            bestSim = dot;
            best = i;
        }
    }

    const bool matched = best >= 0 && bestSim >= m_threshold;
    if (!matched && static_cast<int>(m_centroids.size()) < m_maxSpeakers) {
        m_centroids.push_back(embedding); // already unit-norm
        m_counts.push_back(1);
        return static_cast<int>(m_centroids.size()) - 1;
    }
    if (best < 0) {
        return -1; // dimension mismatch across the board — shouldn't happen
    }

    // Fold into the matched (or, if full, nearest) centroid as a running mean,
    // then renormalize to keep it a unit vector.
    std::vector<float>& c = m_centroids[best];
    const float n = static_cast<float>(m_counts[best]);
    double norm = 0.0;
    for (size_t k = 0; k < c.size(); ++k) {
        c[k] = (c[k] * n + embedding[k]) / (n + 1.0f);
        norm += static_cast<double>(c[k]) * c[k];
    }
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (float& v : c) {
            v = static_cast<float>(v / norm);
        }
    }
    ++m_counts[best];
    return best;
}

} // namespace AetherSDR
