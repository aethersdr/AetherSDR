#include "core/deepfist/DeepFistCommitter.h"
#include <cmath>
#include <cstdio>
using AetherSDR::DeepFistCommitter;
namespace {
// A committer that has published nothing owes no word separator.
bool freshCommitterIsIdle()
{
    AetherSDR::DeepFistCommitter fresh;
    return fresh.process({}, true, 1.0, 5.0, false).isEmpty();
}
}
int main()
{
    using Token = DeepFistCommitter::Token;
    if (!freshCommitterIsIdle()) { return 9; }
    // Recorded TT failure: two windows agree on the pending second T, then
    // activity closes before it reaches the normal publication cutoff.
    for (bool carry : {false, true}) {
        DeepFistCommitter c;
        QString output = c.process({Token{1, 2.8, "T"}, Token{1, 3.69, "T"}}, false, 4.2, 2.9, carry);
        output += c.process({Token{1, 2.8, "T"}, Token{1, 3.70, "T"}}, false, 4.8, 3.5, carry);
        output += c.process({}, true, 5.4, 4.1, carry);
        if (output.trimmed() != (carry ? "TT" : "T")) { return 1; }
        if (!c.process({}, true, 6.0, 4.7, carry).isEmpty() || c.pendingCount()) { return 2; }
    }
    // A single unstable observation cannot be resurrected after the gate closes.
    DeepFistCommitter single;
    single.process({Token{2, 1.9, "E"}}, false, 2.4, 1.1, true);
    if (!single.process({}, true, 3.6, 2.3, true).trimmed().isEmpty()) { return 3; }
    // Stable tokens expire rather than waiting indefinitely for a boundary.
    DeepFistCommitter expired;
    expired.process({Token{2, 1.9, "E"}}, false, 2.4, 1.1, true);
    expired.process({Token{2, 1.91, "E"}}, false, 2.8, 1.5, true);
    if (!expired.process({}, true, 4.4, 3.1, true).trimmed().isEmpty()) { return 4; }
    // Equal text is not identity: two nearby repeated events both survive.
    DeepFistCommitter repeated;
    repeated.process({Token{2, 2.0, "E"}, Token{2, 2.2, "E"}}, false, 2.8, 1.5, true);
    repeated.process({Token{2, 2.01, "E"}, Token{2, 2.21, "E"}}, false, 3.2, 1.9, true);
    if (repeated.process({}, true, 3.6, 2.3, true).trimmed() != "EE") { return 5; }
    repeated = DeepFistCommitter{};
    if (!repeated.process({}, true, 3.6, 2.3, true).trimmed().isEmpty()) { return 6; }
    DeepFistCommitter bounded;
    std::vector<Token> dense;
    for (int i = 0; i < 512; ++i) { dense.push_back({2, 2.0 + i * .001, "E"}); }
    bounded.process(dense, false, 2.8, 1.5, true);
    if (bounded.pendingCount() > 128) { return 7; }
    bounded.process({}, true, 5.0, 3.7, true);
    if (bounded.pendingCount()) { return 8; }
    const auto expectText = [](const QString& actual, const char* expected, const char* step) {
        if (actual == QString::fromLatin1(expected)) { return true; }
        std::fprintf(stderr, "%s: expected '%s', got '%s'\n", step, expected, qPrintable(actual));
        return false;
    };
    // A confirmed tail can settle after the first gated tick. Its separator
    // must follow the tail, and still separate the next active word.
    for (bool carry : {false, true}) {
        DeepFistCommitter delayed;
        QString output = delayed.process({{2, 2.8, "A"}, {21, 4.00, "T"}}, false, 4.4, 3.1, carry);
        output += delayed.process({{2, 2.8, "A"}, {21, 4.01, "T"}}, false, 4.8, 3.5, carry);
        output += delayed.process({}, true, 5.2, 3.9, carry);
        if (!expectText(output, carry ? "A" : "A ", "before tail settles")) { return 9; }
        output += delayed.process({}, true, 5.6, 4.3, carry);
        if (!expectText(output, carry ? "AT " : "A ", "after tail settles")) { return 10; }
        if (!delayed.process({}, true, 6.0, 4.7, carry).isEmpty()) { return 11; }
        output += delayed.process({{6, 5.0, "E"}}, false, 6.4, 5.1, carry);
        if (!expectText(output, carry ? "AT E" : "A E", "next active word")) { return 12; }
    }
    // Repeated tail letters may settle on separate gated ticks; neither the
    // first carried letter nor the idle separator may reorder the second.
    DeepFistCommitter staggered;
    QString tail = staggered.process({{2, 2.8, "A"}, {21, 4.00, "T"}, {21, 4.40, "T"}}, false, 4.4, 3.1, true);
    tail += staggered.process({{21, 4.01, "T"}, {21, 4.41, "T"}}, false, 4.8, 3.5, true);
    tail += staggered.process({}, true, 5.2, 3.9, true);
    if (!expectText(tail, "A", "two pending tails")) { return 13; }
    tail += staggered.process({}, true, 5.6, 4.3, true);
    if (!expectText(tail, "AT", "first tail settles")) { return 14; }
    tail += staggered.process({}, true, 6.0, 4.7, true);
    if (!expectText(tail, "ATT ", "second tail settles")) { return 15; }
    if (!staggered.process({}, true, 6.4, 5.1, true).isEmpty()) { return 16; }
    // A slow guard may outlive pending evidence. Expiry closes the word once
    // without publishing the expired character or leaving separation stuck.
    DeepFistCommitter delayedExpiry;
    QString expiry = delayedExpiry.process({{2, 2.0, "A"}, {21, 4.20, "T"}}, false, 4.4, 2.2, true);
    expiry += delayedExpiry.process({{21, 4.21, "T"}}, false, 4.8, 2.6, true);
    expiry += delayedExpiry.process({}, true, 5.2, 3.0, true);
    expiry += delayedExpiry.process({}, true, 5.6, 3.4, true);
    expiry += delayedExpiry.process({}, true, 6.0, 3.8, true);
    if (!expectText(expiry, "A", "tail awaiting slow guard")) { return 17; }
    expiry += delayedExpiry.process({}, true, 6.4, 4.2, true);
    if (!expectText(expiry, "A ", "tail expires")) { return 18; }
    if (!delayedExpiry.process({}, true, 6.8, 4.6, true).isEmpty()) { return 19; }
    expiry += delayedExpiry.process({{6, 5.0, "E"}}, false, 7.2, 5.0, true);
    if (!expectText(expiry, "A E", "active word after expiry")) { return 20; }
    // Confidence (CONSTRUCTED values, arithmetic only): a published letter
    // carries the mean over its sightings, one per window; the separator is
    // shown as certain; a carried letter averages the sightings it had.
    using Piece = DeepFistCommitter::Piece;
    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    DeepFistCommitter scored;
    std::vector<Piece> pieces;
    scored.process({{2, 2.8, "A", 0.9f}, {21, 3.60, "T", 0.2f}}, false, 4.0, 2.7, true, &pieces);
    scored.process({{2, 2.81, "A", 0.6f}, {21, 3.61, "T", 0.4f}}, false, 4.4, 2.7, true, &pieces);
    if (!pieces.empty()) { return 21; }
    scored.process({{2, 2.8, "A", 0.3f}, {21, 3.62, "T", 0.6f}}, false, 4.8, 3.0, true, &pieces);
    if (pieces.size() != 1 || pieces[0].text != "A" || !near(pieces[0].confidence, 0.6f)) { return 22; }
    pieces.clear();
    scored.process({}, true, 5.2, 3.9, true, &pieces);
    if (pieces.size() != 2 || pieces[0].text != "T" || !near(pieces[0].confidence, 0.4f)
        || pieces[1].text != " " || !near(pieces[1].confidence, 1.f)) { return 23; }
    // A repeated letter inside the 0.12 s tolerance keeps its own score (CONSTRUCTED,
    // the review's case): two E 80 ms apart, seen identically in two windows.
    DeepFistCommitter closePair;
    pieces.clear();
    closePair.process({{6, 2.80, "E", 0.9f}, {6, 2.88, "E", 0.1f}}, false, 4.0, 2.0, false, &pieces);
    closePair.process({{6, 2.80, "E", 0.9f}, {6, 2.88, "E", 0.1f}}, false, 4.4, 3.0, false, &pieces);
    if (pieces.size() != 2 || !near(pieces[0].confidence, 0.9f) || !near(pieces[1].confidence, 0.1f)) {
        return 24;
    }
    // The same pair with the later window's times shifted 50 ms: matched by order,
    // since the closest earlier sighting of the first E is now the second E's.
    DeepFistCommitter shiftedPair;
    pieces.clear();
    shiftedPair.process({{6, 2.80, "E", 0.9f}, {6, 2.88, "E", 0.1f}}, false, 4.0, 2.0, false, &pieces);
    shiftedPair.process({{6, 2.85, "E", 0.9f}, {6, 2.93, "E", 0.1f}}, false, 4.4, 3.0, false, &pieces);
    if (pieces.size() != 2 || !near(pieces[0].confidence, 0.9f) || !near(pieces[1].confidence, 0.1f)) {
        return 25;
    }
    // Counts differ (the earlier window saw one E): each letter takes the closest
    // sighting, so both average in the earlier 0.9.
    DeepFistCommitter unevenPair;
    pieces.clear();
    unevenPair.process({{6, 2.80, "E", 0.9f}}, false, 4.0, 2.0, false, &pieces);
    unevenPair.process({{6, 2.80, "E", 0.9f}, {6, 2.88, "E", 0.1f}}, false, 4.4, 3.0, false, &pieces);
    if (pieces.size() != 2 || !near(pieces[0].confidence, 0.9f) || !near(pieces[1].confidence, 0.5f)) {
        return 26;
    }
    std::puts("committer: carry mutation, expiry, repeated tokens, reset, delayed separators, confidence passed");
}
