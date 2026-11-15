// Socket-free algorithm substitute for wrapper tests. This is a half-gain
// transfer function, not NVIDIA inference or hardware qualification.
#include <algorithm>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#define TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
struct State {
    unsigned rate{0};
    bool resetSinceLoad{false};
};
unsigned g_samples{0};
unsigned g_resets{0};
// Frames run on an effect that was loaded but never NvAFX_Reset. The real
// SDK can hand a new effect a destroyed effect's leftover state, so the
// wrapper must reset every effect before its first Run.
unsigned g_unresetRuns{0};
}

TEST_EXPORT int NvAFX_CreateEffect(const char*, void** handle)
{
    *handle = std::make_unique<State>().release();
    return 0;
}
TEST_EXPORT int NvAFX_DestroyEffect(void* handle)
{
    const std::unique_ptr<State> state(static_cast<State*>(handle));
    return 0;
}
TEST_EXPORT int NvAFX_SetU32(void* handle, const char* key, unsigned value)
{
    if (std::strcmp(key, "input_sample_rate") == 0) {
        static_cast<State*>(handle)->rate = value;
    }
    return 0;
}
TEST_EXPORT int NvAFX_SetString(void*, const char*, const char*) { return 0; }
TEST_EXPORT int NvAFX_SetFloat(void*, const char*, float) { return 0; }
TEST_EXPORT int NvAFX_GetU32(void*, const char*, unsigned* value)
{
    *value = 480;
    return 0;
}
TEST_EXPORT int NvAFX_Load(void* handle)
{
    auto* state = static_cast<State*>(handle);
    state->resetSinceLoad = false;
    return state->rate == 48000 ? 0 : 1;
}
TEST_EXPORT int NvAFX_Reset(void* handle, bool* resetList, unsigned listLength)
{
    if (!resetList || listLength != 1 || !resetList[0]) {
        return 1;
    }
    static_cast<State*>(handle)->resetSinceLoad = true;
    ++g_resets;
    return 0;
}
TEST_EXPORT int NvAFX_Run(void* handle, const float** input, float** output,
                          unsigned samples, unsigned channels)
{
    if (samples != 480 || channels != 1) {
        return 1;
    }
    if (!static_cast<State*>(handle)->resetSinceLoad) {
        ++g_unresetRuns;
    }
    for (unsigned index = 0; index < samples; ++index) {
        output[0][index] = input[0][index] * 0.5f;
    }
    g_samples += samples;
    return 0;
}
TEST_EXPORT unsigned aetherTestProcessedSamples() { return g_samples; }
TEST_EXPORT unsigned aetherTestResets() { return g_resets; }
TEST_EXPORT unsigned aetherTestUnresetRuns() { return g_unresetRuns; }
