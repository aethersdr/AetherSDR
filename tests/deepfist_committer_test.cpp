#include "core/deepfist/DeepFistCommitter.h"
#include <cstdio>
using AetherSDR::DeepFistCommitter;
int main()
{
    using Token = DeepFistCommitter::Token;
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
    std::puts("committer: carry mutation, expiry, repeated tokens, reset passed");
}
