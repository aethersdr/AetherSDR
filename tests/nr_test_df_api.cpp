// Link-time C API substitute. It preserves DFN3's documented three-hop
// latency but performs only a half-gain transfer function, not inference.
#include "deep_filter.h"
#include <array>
#include <memory>

struct DFState {
    std::array<float, 1440> delay{};
    int cursor{0};
};
namespace { unsigned g_samples{0}; }

extern "C" DFState* df_create(const char*, float, const char*)
{
    return std::make_unique<DFState>().release();
}
extern "C" uintptr_t df_get_frame_length(DFState*) { return 480; }
extern "C" void df_set_atten_lim(DFState*, float) {}
extern "C" void df_set_post_filter_beta(DFState*, float) {}
extern "C" float df_process_frame(DFState* state, float* input, float* output)
{
    for (int index = 0; index < 480; ++index) {
        output[index] = state->delay[state->cursor] * 0.5f;
        state->delay[state->cursor] = input[index];
        state->cursor = (state->cursor + 1) % 1440;
    }
    g_samples += 480;
    return 0.0f;
}
extern "C" void df_free(DFState* state)
{
    const std::unique_ptr<DFState> owned(state);
}
unsigned aetherTestDfProcessedSamples() { return g_samples; }
