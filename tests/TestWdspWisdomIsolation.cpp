// Linked into every test executable by aether_retrofit_tests() (deferred, in
// tests/tests.cmake) unless it sets AETHER_TEST_NO_WISDOM_ISOLATION. Runs before
// main() so a test binary run directly, outside ctest's ENVIRONMENT, still caps
// the FFTW planner and redirects the wisdom cache away from the operator's real
// ~/.cache/aethersdr/wdsp-fftw-wisdom (docs/HERMES.md). setenv(..., 0): an
// explicit value from ctest, CI, or a developer still wins.

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
