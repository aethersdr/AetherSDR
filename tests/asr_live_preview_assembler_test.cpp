// LivePreviewAssembler: provisional recogniser results sorted into one block per
// ASR segment, so each segment's grey text can be replaced by its final text.
// Pure; no threads, no recogniser.

#include "asr/LivePreviewAssembler.h"

#include <cstdio>

using AetherSDR::LivePreviewAssembler;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

bool is(const std::optional<LivePreviewAssembler::Update>& u, quint64 block, const char* text)
{
    return u.has_value() && u->block == block && u->text == QString::fromUtf8(text);
}

constexpr qint64 kSec = 16000;

} // namespace

int main()
{
    {
        LivePreviewAssembler a;
        expect(is(a.addResult(0, 1 * kSec, QStringLiteral("Okay"), false), 0, "Okay"),
               "a provisional result before any segment closes goes to block 0");
        expect(is(a.addResult(0, 2 * kSec, QStringLiteral("Okay, go ahead"), false), 0,
                  "Okay, go ahead"),
               "a later provisional result from the same start replaces the earlier one");
        expect(!a.addResult(0, 2 * kSec, QStringLiteral("Okay, go ahead"), false).has_value(),
               "an update that changes nothing on screen is not reported");
        expect(is(a.addResult(0, 2 * kSec, QStringLiteral("Okay, go ahead."), true), 0,
                  "Okay, go ahead."),
               "a final result supersedes the provisional text it covers");
        expect(is(a.addResult(2 * kSec, 3 * kSec, QStringLiteral(" pick it"), false), 0,
                  "Okay, go ahead. pick it"),
               "provisional text after a final is appended to the block's finals");
    }
    {
        LivePreviewAssembler a;
        a.addResult(0, 3 * kSec, QStringLiteral("first over"), false);
        a.closeBlock(0, 5 * kSec);
        expect(is(a.addResult(5 * kSec, 6 * kSec, QStringLiteral("second"), false), 1, "second"),
               "a result starting at the close edge opens the next block");
        expect(is(a.addResult(0, 5 * kSec, QStringLiteral("first over done"), true), 0,
                  "first over done"),
               "a late result for a closed but unresolved block still updates that block");
        a.resolve(0);
        expect(!a.addResult(1 * kSec, 4 * kSec, QStringLiteral("stale"), true).has_value(),
               "a result inside a resolved block is dropped");
        expect(is(a.addResult(5 * kSec, 7 * kSec, QStringLiteral("second over"), false), 1,
                  "second over"),
               "resolving one block leaves the next block's text intact");
    }
    {
        LivePreviewAssembler a;
        a.closeBlock(0, 5 * kSec);
        a.closeBlock(1, 9 * kSec);
        expect(is(a.addResult(6 * kSec, 8 * kSec, QStringLiteral("middle"), false), 1, "middle"),
               "a result is assigned to the closed block that contains its start");
        expect(is(a.addResult(10 * kSec, 11 * kSec, QStringLiteral("open"), false), 2, "open"),
               "a result after the last close goes to the open block");
        a.resolve(1);
        expect(!a.addResult(6 * kSec, 8 * kSec, QStringLiteral("middle again"), false).has_value(),
               "resolving a block also resolves every older block");
    }
    {
        LivePreviewAssembler a;
        a.addResult(0, 1 * kSec, QStringLiteral("  spaced   out "), false);
        expect(is(a.addResult(1 * kSec, 2 * kSec, QStringLiteral(" text"), true), 0,
                  "spaced out text"),
               "block text is whitespace-normalised and in audio order");
        a.closeBlock(0, 2 * kSec);
        a.resolve(0);
        a.reset();
        expect(is(a.addResult(0, 1 * kSec, QStringLiteral("fresh"), false), 0, "fresh"),
               "reset starts a new timeline at block 0");
    }

    if (g_failures == 0) {
        std::printf("Live preview assembler: ALL PASS\n");
        return 0;
    }
    std::printf("Live preview assembler: %d FAILURE(S)\n", g_failures);
    return 1;
}
