#pragma once

#include <QString>

namespace MasterSDR {

struct MemoryEntry {
    int     index{-1};
    QString group;
    QString owner;
    double  freq{0.0};
    QString name;
    QString mode;
    int     step{100};
    QString offsetDir;       // "simplex", "up", "down"
    double  repeaterOffset{0.0};
    QString toneMode;        // "off", "ctcss_tx", ...
    double  toneValue{0.0};
    bool    squelch{false};
    int     squelchLevel{0};
    int     rxFilterLow{0};
    int     rxFilterHigh{0};
    int     rttyMark{2125};
    int     rttyShift{170};
    int     diglOffset{2210};
    int     diguOffset{1500};
};

} // namespace MasterSDR
