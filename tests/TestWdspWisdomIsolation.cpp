// Linked into every registered test target (see the loop at the end of
// tests/tests.cmake). Its whole job is to run BEFORE main() and make it
// impossible for a test process to write the operator's real FFTW wisdom cache.
//
// Why a static initializer and not just a ctest ENVIRONMENT property: the
// property only covers `ctest`. Running a test binary directly —
// `QT_QPA_PLATFORM=offscreen ./build/hl2_rxdsp_test`, which is the normal way
// to debug one — inherits nothing, and WdspChannel::open() would then export
// over ~/.cache/aethersdr/wdsp-fftw-wisdom. That is a silent 20-60 s tax on the
// operator's NEXT REAL CONNECT, paid every time they build a PR, with nothing
// on screen connecting cause to effect. The ctest properties are kept as well,
// so the values are visible in CTestTestfile.cmake rather than only implied.
//
// setenv(..., 0) — do NOT overwrite. An explicit value from ctest, from CI, or
// from a developer investigating plan quality still wins.

#include <cstdlib>

namespace {

struct WdspWisdomIsolation {
    WdspWisdomIsolation() noexcept
    {
#ifdef _WIN32
        if (std::getenv("AETHER_WDSP_WISDOM_DIR") == nullptr)
            _putenv_s("AETHER_WDSP_WISDOM_DIR", AETHER_TEST_WISDOM_DIR);
        if (std::getenv("AETHER_WDSP_FFTW_TIMELIMIT") == nullptr)
            _putenv_s("AETHER_WDSP_FFTW_TIMELIMIT", AETHER_TEST_FFTW_TIMELIMIT_STR);
#else
        ::setenv("AETHER_WDSP_WISDOM_DIR", AETHER_TEST_WISDOM_DIR, 0);
        ::setenv("AETHER_WDSP_FFTW_TIMELIMIT", AETHER_TEST_FFTW_TIMELIMIT_STR, 0);
#endif
    }
};

// Namespace-scope object => constructed during static init, before main().
const WdspWisdomIsolation g_wdspWisdomIsolation;

} // namespace
