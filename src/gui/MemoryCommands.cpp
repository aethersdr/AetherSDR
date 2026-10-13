#include "MemoryCommands.h"

#include "models/SliceModel.h"

namespace AetherSDR {

MemoryEntry captureMemoryFromSlice(const RadioModel& model,
                                   const SliceModel& slice,
                                   const QString& name)
{
    MemoryEntry memory;
    memory.group = model.activeGlobalProfile();
    memory.owner = model.callsign();
    memory.freq = slice.frequency();
    memory.name = name.trimmed();
    memory.mode = slice.mode();
    memory.step = slice.stepHz();
    memory.offsetDir = slice.repeaterOffsetDir();
    memory.repeaterOffset = slice.fmRepeaterOffsetFreq();
    memory.toneMode = slice.fmToneMode();
    memory.toneValue = slice.fmToneValue().toDouble();
    memory.squelch = slice.squelchOn();
    memory.squelchLevel = slice.squelchLevel();
    // Memory filter fields are USB-form on the wire for FDV modes (FlexLib
    // Memory.cs clamps FDV rx_filter_low to >= 0), while the client stores
    // FDVL canonically negative since #3434. Mirror back to the ecosystem
    // form at capture so radio-side memory slots (and the CSV export built
    // from this struct) stay compatible with SmartSDR/FlexLib clients —
    // a stored negative low would be clamped to 0 there, degrading the slot.
    {
        int memLow = slice.filterLow();
        int memHigh = slice.filterHigh();
        if (SliceModel::filterPolarityLsbFamily(memory.mode)
            && memory.mode.startsWith(QStringLiteral("FDV"))
            && memLow < 0) {
            const int lo = memLow, hi = memHigh;
            memLow  = -hi;
            memHigh = -lo;
        }
        memory.rxFilterLow = memLow;
        memory.rxFilterHigh = memHigh;
    }
    memory.rttyMark = slice.rttyMark();
    memory.rttyShift = slice.rttyShift();
    memory.diglOffset = slice.diglOffset();
    memory.diguOffset = slice.diguOffset();
    return memory;
}

void createMemoryFromSlice(RadioModel* model,
                           const SliceModel* slice,
                           const QString& name,
                           QObject* callbackContext,
                           MemoryCreateCallback cb)
{
    if (!model || !slice) {
        if (cb) { cb(-1, QStringLiteral("No active slice is available."), -1); }
        return;
    }
    const MemoryEntry memory = captureMemoryFromSlice(*model, *slice, name);
    model->createMemory(memory, callbackContext,
        [cb](const RadioControlReply& result, int index) {
        if (cb) {
            cb(result.kind == RadioControlReply::Kind::Accepted ? 0 : (result.code == 0 ? -1 : result.code),
               result.message, index);
        }
    });
}

} // namespace AetherSDR
