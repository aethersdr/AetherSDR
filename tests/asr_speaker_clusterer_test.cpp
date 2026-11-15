// Unit test for SpeakerClusterer — online A/B/C labeling from unit embeddings.
// Pure C++ (no ONNX), so it always runs.
#include "asr/SpeakerClusterer.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {
int g_failures = 0;
void expect(bool cond, const char* what)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

// Unit vector near `axis` with a small perturbation on another axis.
std::vector<float> vec(int axis, float perturb)
{
    std::vector<float> v(8, 0.0f);
    v[axis] = 1.0f;
    v[(axis + 3) % 8] = perturb;
    double n = 0;
    for (float x : v) n += static_cast<double>(x) * x; // accumulate in double
    n = std::sqrt(n);
    for (float& x : v) x = static_cast<float>(x / n);
    return v;
}
} // namespace

int main()
{
    // Two distinct speaker directions; each appears twice, A B A B.
    const std::vector<float> a1 = vec(0, 0.05f);
    const std::vector<float> a2 = vec(0, -0.04f);
    const std::vector<float> b1 = vec(1, 0.05f);
    const std::vector<float> b2 = vec(1, -0.03f);

    SpeakerClusterer clus(0.5f);
    const int la1 = clus.assign(a1);
    const int lb1 = clus.assign(b1);
    const int la2 = clus.assign(a2);
    const int lb2 = clus.assign(b2);

    expect(la1 == 0, "first speaker is A (0)");
    expect(lb1 == 1, "second, distinct speaker is B (1)");
    expect(la2 == la1, "speaker A recognized again");
    expect(lb2 == lb1, "speaker B recognized again");
    expect(clus.speakerCount() == 2, "exactly two speakers found");

    // Empty embedding (embedder unavailable) → unknown.
    expect(clus.assign({}) == -1, "empty embedding → -1");

    // reset() relabels from scratch.
    clus.reset();
    expect(clus.speakerCount() == 0, "reset clears speakers");
    expect(clus.assign(b1) == 0, "after reset, next speaker is A again");

    // A high threshold splits even the same nominal speaker's perturbations.
    SpeakerClusterer strict(0.999f);
    strict.assign(a1);
    const int split = strict.assign(a2);
    expect(split == 1, "very high threshold splits near-duplicates");

    std::printf(g_failures == 0 ? "\nSpeakerClusterer: ALL PASS\n"
                                : "\nSpeakerClusterer: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
