#pragma once

#include <QString>
#include <vector>

namespace MasterSDR {

enum class ChangeCategory { Feature, BugFix, Improvement, Infrastructure };

struct ChangeItem {
    ChangeCategory category;
    QString title;
    QString description;
};

struct ReleaseEntry {
    QString version;     // "0.7.16"
    QString date;        // "2026-03-31"
    QString headline;    // "World-First TGXL Relay Control & Global Band Plans"
    std::vector<ChangeItem> items;
};

// Returns all release entries, newest first.
// Generated from CHANGELOG.md by scripts/gen_whatsnew.py at build time.
const std::vector<ReleaseEntry>& whatsNewEntries();

} // namespace MasterSDR
