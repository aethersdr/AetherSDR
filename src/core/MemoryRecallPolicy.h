#pragma once

#include "models/MemoryEntry.h"

namespace MasterSDR {

double memoryRepeaterTxOffsetFreq(const MemoryEntry& memory);
QString buildMemoryRecallRetuneCommand(int sliceId, const MemoryEntry& memory);
QString buildMemoryRecallSliceFixupCommand(int sliceId, const MemoryEntry& memory);

} // namespace MasterSDR
