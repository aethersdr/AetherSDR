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

    // Audio the segmenter threw away (a burst too short to be speech) never
    // gets a final, so its preview text must go when the discard is reported.
    {
        LivePreviewAssembler a;
        a.addResult(0, 1 * kSec, QStringLiteral("old"), false);
        a.closeBlock(0, 2 * kSec);
        a.addResult(2 * kSec, 3 * kSec, QStringLiteral("crash"), true);
        a.addResult(3 * kSec, 4 * kSec, QStringLiteral("bang"), false);
        expect(is(a.discardOpenThrough(5 * kSec), 1, ""),
               "a discard empties the open block's text from before its end");
        expect(!a.addResult(3 * kSec, 4 * kSec, QStringLiteral("bang again"), false).has_value(),
               "a late result inside the discarded span is dropped");
        expect(is(a.addResult(0, 1 * kSec, QStringLiteral("old revised"), true), 0, "old revised"),
               "a discard leaves the closed, unresolved block alone");
        expect(is(a.addResult(5 * kSec, 6 * kSec, QStringLiteral("real speech"), false), 1,
                  "real speech"),
               "text after the discard goes to the open block as before");
        expect(!a.discardOpenThrough(4 * kSec).has_value(),
               "a discard that ends before the open block starts changes nothing");
    }
    {
        LivePreviewAssembler a;
        a.addResult(0, 1 * kSec, QStringLiteral("noise"), true);
        a.addResult(3 * kSec, 4 * kSec, QStringLiteral("hello"), false);
        expect(is(a.discardOpenThrough(2 * kSec), 0, "hello"),
               "a discard drops only the text that starts inside it");
    }
    {
        // The worker reports a segment's edge late (it was busy decoding the
        // previous one) while the recogniser has already run past that edge.
        LivePreviewAssembler a;
        a.addResult(0, 10 * kSec, QStringLiteral("one two three"), true);
        a.addResult(10 * kSec, 11 * kSec, QStringLiteral("four"), true);
        a.addResult(11 * kSec, 12 * kSec, QStringLiteral("five six"), false);
        const auto moved = a.closeBlock(0, 8 * kSec);
        bool block0 = false;
        bool block1 = false;
        for (const auto& u : moved) {
            block0 = block0 || (u.block == 0 && u.text == QStringLiteral("one two three"));
            block1 = block1 || (u.block == 1 && u.text == QStringLiteral("four five six"));
        }
        expect(moved.size() == 2 && block0 && block1,
               "a late close moves what starts after the edge into the next block");
        expect(is(a.addResult(11 * kSec, 13 * kSec, QStringLiteral("five six seven"), false), 1,
                  "four five six seven"),
               "later text from past the edge joins it there, not twice");
        a.resolve(0);
        expect(is(a.addResult(13 * kSec, 14 * kSec, QStringLiteral("eight"), true), 1,
                  "four five six seven eight"),
               "resolving the closed block keeps the moved text");
    }
    {
        LivePreviewAssembler a;
        a.addResult(0, 2 * kSec, QStringLiteral("hello"), false);
        expect(a.closeBlock(0, 5 * kSec).empty(),
               "a close with nothing past the edge reports no change");
    }

    if (g_failures == 0) {
        std::printf("Live preview assembler: ALL PASS\n");
        return 0;
    }
    std::printf("Live preview assembler: %d FAILURE(S)\n", g_failures);
    return 1;
}
