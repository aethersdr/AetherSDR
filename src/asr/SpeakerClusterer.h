#pragma once

#include <vector>

namespace AetherSDR {

// Online speaker clustering (RFC #4333 follow-up). Assigns each utterance's
// L2-normalized speaker embedding to a speaker index (0 = A, 1 = B, …): the
// nearest existing centroid if cosine similarity clears the threshold, else a
// new speaker. Centroids are running means, renormalized to stay unit vectors.
//
// Suited to half-duplex radio (one transmitter at a time → each utterance is a
// single speaker), so no overlap handling is needed. Session-relative labels,
// reset per listening session. Pure C++, unit-testable.
class SpeakerClusterer {
public:
    explicit SpeakerClusterer(float threshold = 0.50f, int maxSpeakers = 16);

    // Speaker index for this embedding (adds a speaker if none is close enough).
    // Returns -1 for an empty embedding (embedder unavailable).
    int assign(const std::vector<float>& embedding);

    int speakerCount() const { return static_cast<int>(m_centroids.size()); }
    void setThreshold(float t) { m_threshold = t; }
    float threshold() const { return m_threshold; }
    void reset();

private:
    float m_threshold;
    int m_maxSpeakers;
    std::vector<std::vector<float>> m_centroids; // unit-norm running centroids
    std::vector<int> m_counts;
};

} // namespace AetherSDR
