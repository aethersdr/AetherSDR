#pragma once

#include <QString>

namespace AetherSDR {

struct MemoryEntry {
    int     index{-1};
    QString group;
    QString owner;
    QString channel;          // native radio channel label, when one exists
    // Stable provenance for a channel ingested from a radio. Empty for manual
    // and CSV-created entries. Sync uses this pair to update the same database
    // row on the next pass instead of duplicating it.
    QString importSource;
    QString importKey;
    double  freq{0.0};
    QString name;
    QString mode;
    int     nativeFilter{0}; // vendor memory-record filter selector, if exposed
    int     dataMode{0};     // vendor memory-record data-mode flag, if exposed
    int     step{100};
    QString offsetDir;       // "simplex", "up", "down"
    double  repeaterOffset{0.0};
    QString toneMode;        // "off", "ctcss_tx", ...
    double  toneValue{0.0};
    double  rxToneValue{0.0};
    int     dtcsCode{23};
    bool    dtcsTxReverse{false};
    bool    dtcsRxReverse{false};
    bool    recallable{true};
    bool    squelch{false};
    int     squelchLevel{0};
    int     rxFilterLow{0};
    int     rxFilterHigh{0};
    int     rttyMark{2125};
    int     rttyShift{170};
    int     diglOffset{2210};
    int     diguOffset{1500};

    // Field-wise equality. The local memory bank uses it to skip re-saving a
    // slot that did not actually change — the memory dialog re-asserts the
    // kv-set it just sent, so without this a single edit writes the bank file
    // twice.
    bool operator==(const MemoryEntry&) const = default;
};

} // namespace AetherSDR
