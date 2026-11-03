#include "core/backends/SlicePresetRequest.h"

#include <iostream>

using namespace AetherSDR;

int main()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    MemoryEntry entry;
    expect(SlicePresetRequest::repeaterFixup(entry).empty(), "an absent preset changes no repeater fields");
    entry.toneValue = 123.0;
    SlicePresetRequest request = SlicePresetRequest::repeaterFixup(entry);
    expect(request.valid() && !request.direction && !request.offsetMhz && !request.toneMode
               && request.toneHz == 123.0, "tone-only correction leaves mode and offsets absent");
    entry.toneMode = QStringLiteral("OFF");
    entry.toneValue = 0.0;
    entry.offsetDir = QStringLiteral(" DOWN ");
    entry.repeaterOffset = -0.6;
    request = SlicePresetRequest::repeaterFixup(entry);
    expect(request.valid() && request.direction == QStringLiteral("down") && request.offsetMhz == 0.6
               && request.toneMode == QStringLiteral("off") && request.toneHz == 0.0,
           "present direction and tone are normalized without inventing absent values");
    entry.offsetDir = QStringLiteral("simplex");
    request = SlicePresetRequest::repeaterFixup(entry);
    expect(request.valid() && request.direction == QStringLiteral("simplex"), "simplex remains an explicit correction");
    entry.toneMode = QStringLiteral("off\ntransmit tune 1");
    expect(!SlicePresetRequest::repeaterFixup(entry).valid(), "malformed tone mode is refused");
    return failures ? 1 : 0;
}
