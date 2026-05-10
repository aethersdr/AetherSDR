#pragma once

#include "models/MemoryEntry.h"

namespace AetherSDR {

double memoryRepeaterTxOffsetFreq(const MemoryEntry& memory);
QString buildMemoryRecallRetuneCommand(int sliceId, const MemoryEntry& memory);
QString buildMemoryRecallSliceFixupCommand(int sliceId, const MemoryEntry& memory);

} // namespace AetherSDR
