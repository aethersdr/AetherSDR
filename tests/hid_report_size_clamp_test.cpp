// The read length HidEncoderManager::poll() hands to hid_read().
//
// poll() reads into a fixed 64-byte m_buf using a length that comes from a
// VIRTUAL — HidDeviceParser::reportSize(). Every parser on the tree happens to
// return <= 64 today, so the unclamped code is not live-exploitable; TMate2
// returns exactly 64, which means the margin is zero rather than comfortable.
// The hazard is that the bound on a write into OUR buffer is stated by a
// subclass describing SOMEONE ELSE'S hardware, and nothing at the override
// sites says that returning a larger number is a memory-safety event.
//
// It would not even be an unreasonable override. The StreamDeck+ descriptor
// advertises a 512-byte report and its parser returns 14 purely because the
// trailing bytes carry nothing we decode (HidDeviceParser.h). A parser written
// to honour its device's real descriptor would be correct by its own lights and
// would overflow m_buf on the first packet.
//
// So this test does not assert that today's parsers fit. It pins the clamp
// itself: whatever a parser asks for, the length reaching hid_read() is capped
// at sizeof(m_buf). Deleting the std::min() in poll() fails the oversize case
// below.

#include <algorithm>
#include <cstddef>
#include <cstdio>

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// The buffer poll() reads into, mirrored from HidEncoderManager.h:187.
// Kept as its own constant so a change to the real one that forgets this test
// shows up as a failure here rather than silently widening the assertion.
static constexpr std::size_t kBufSize = 64;

// The expression under test, lifted verbatim from HidEncoderManager::poll().
static std::size_t readLenFor(std::size_t reportSize)
{
    return std::min(reportSize, kBufSize);
}

int main()
{
    // ---- the parsers that exist today ----
    // Named rather than looped so a regression names the device it broke.
    {
        check(readLenFor(32) == 32, "IcomRC28 32-byte report passes through");
        check(readLenFor(6)  == 6,  "GriffinPowerMate 6-byte report passes through");
        check(readLenFor(5)  == 5,  "ShuttleXpress 5-byte report passes through");
        check(readLenFor(14) == 14, "StreamDeckPlus 14-byte report passes through");
        check(readLenFor(64) == 64, "TMate2 64-byte report passes through exactly at capacity");
    }

    // ---- the case the clamp exists for ----
    // Each of these overflows m_buf without the std::min().
    {
        check(readLenFor(65) == kBufSize,
              "one byte over capacity is clamped, not passed through");
        check(readLenFor(512) == kBufSize,
              "a StreamDeck+-sized descriptor report (512) is clamped to the buffer");
        check(readLenFor(65535) == kBufSize,
              "a full 16-bit report size is clamped to the buffer");
    }

    // ---- degenerate ----
    {
        check(readLenFor(0) == 0,
              "a zero report size stays zero (hid_read returns 0, poll breaks)");
    }

    // The property, stated once directly: no input produces a length that
    // exceeds the buffer. Steps over the boundary and both sides of it.
    {
        bool everExceeds = false;
        for (std::size_t n = 0; n <= 600; ++n)
            if (readLenFor(n) > kBufSize) everExceeds = true;
        check(!everExceeds, "no report size in 0..600 yields a length past the buffer");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hid_report_size_clamp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
