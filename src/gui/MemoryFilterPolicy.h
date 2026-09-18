#pragma once

#include "core/backends/RadioCapabilities.h"

#include <QString>
#include <QStringList>

namespace AetherSDR {

struct MemoryFilterSpec {
    QString label;
    QStringList names;
};

inline void appendMemoryFilterName(QStringList& names, const QString& candidate)
{
    const QString normalized = candidate.trimmed();
    if (!normalized.isEmpty()
        && !names.contains(normalized, Qt::CaseInsensitive)) {
        names.append(normalized);
    }
}

// Flex memories use radio-owned global/TX profiles. Client-owned memory banks
// use their own groups, supplemented only by native groups an import source
// requires. Keeping the two vocabularies separate prevents stale Flex profile
// state from becoming an Icom Sync argument after a radio-family switch.
[[nodiscard]] inline MemoryFilterSpec memoryFilterSpec(
    const RadioCapabilities& capabilities,
    const QStringList& storedGroups,
    const QStringList& globalProfiles,
    const QStringList& txProfiles,
    bool usesLocalBank = false)
{
    MemoryFilterSpec spec;
    if (!usesLocalBank && capabilities.hasProfiles) {
        spec.label = QStringLiteral("Profile:");
        for (const QString& profile : globalProfiles) {
            appendMemoryFilterName(spec.names, profile);
        }
        for (const QString& profile : txProfiles) {
            appendMemoryFilterName(spec.names, profile);
        }
    } else {
        spec.label = QStringLiteral("Group:");
        for (const QString& group : capabilities.memoryGroups) {
            appendMemoryFilterName(spec.names, group);
        }
        for (const QString& group : storedGroups) {
            appendMemoryFilterName(spec.names, group);
        }
    }
    spec.names.sort(Qt::CaseInsensitive);
    return spec;
}

[[nodiscard]] inline bool memoryRefreshSelectionValid(
    const RadioCapabilities& capabilities,
    const QString& selection)
{
    return !capabilities.memoryRefreshRequiresGroup
        || capabilities.memoryGroups.contains(selection.trimmed(), Qt::CaseInsensitive);
}

} // namespace AetherSDR
