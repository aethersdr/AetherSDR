// Phase-1 linkage smoke test for the vendored whisper.cpp ASR engine (RFC #4333).
//
// This does NOT run inference (no model is bundled — weights are downloaded on
// demand). It only proves the vendored whisper.cpp/ggml CPU engine compiles and
// links: we call two model-free entry points and assert the library reports a
// version and a non-empty system-info string. If whisper/ggml failed to build
// or link, this test would not compile or would fail at runtime.

#include <whisper.h>

#include <cstdio>
#include <cstring>

int main() {
    const char* version = whisper_version();
    if (version == nullptr || std::strlen(version) == 0) {
        std::fprintf(stderr, "whisper_version() returned empty\n");
        return 1;
    }

    const char* sysinfo = whisper_print_system_info();
    if (sysinfo == nullptr || std::strlen(sysinfo) == 0) {
        std::fprintf(stderr, "whisper_print_system_info() returned empty\n");
        return 1;
    }

    std::printf("whisper.cpp linked OK: version=%s\n", version);
    std::printf("ggml system info: %s\n", sysinfo);
    return 0;
}
