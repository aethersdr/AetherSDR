#pragma once

#include "models/NetEntry.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Portable, versioned JSON persistence for the net schedule. This is the
// canonical backup/share format — pretty-printed, self-describing, and evolved
// additively (new fields optional with defaults; unknown fields ignored) so
// files stay forward/backward compatible. Recurrence lives as an RRULE string
// rather than enumerated dates, and every entry carries a stable UUID so
// re-import and user-to-user sharing can merge deterministically.
//
// Envelope:
//   {
//     "format": "aether.netschedule",
//     "version": 1,
//     "exportedAt": "2026-06-19T14:00:00Z",
//     "exportedBy": "AetherSDR",
//     "nets": [ { ...NetEntry... } ]
//   }
class NetScheduleStore {
public:
    static constexpr int kFormatVersion = 1;
    static constexpr const char* kFormatId = "aether.netschedule";

    struct ParseResult {
        QList<NetEntry> nets;
        QStringList errors;
        int version{0};

        bool ok() const { return errors.isEmpty(); }
    };

    // Merge policy for importing entries whose id already exists locally.
    enum class MergePolicy {
        Skip,       // keep the existing entry
        Overwrite,  // replace the existing entry with the imported one
        Duplicate,  // import as a brand-new entry with a fresh id
    };

    // Serialize to pretty-printed JSON bytes. `exportedAtIso` is stamped into
    // the envelope (pass QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
    // from the caller — kept as a parameter so this stays clock-free/testable).
    static QByteArray serialize(const QList<NetEntry>& nets, const QString& exportedAtIso = {});

    // Parse JSON bytes. Tolerant: unknown keys are ignored, missing optional
    // fields fall back to NetEntry defaults, and entries without an id are
    // assigned one. Malformed top-level JSON populates errors and returns empty.
    static ParseResult parse(const QByteArray& bytes);

    // Merge `incoming` into `existing` by id per `policy`. Returns the merged
    // list; never mutates in place. Entries without an id are treated as new.
    static QList<NetEntry> merge(const QList<NetEntry>& existing,
                                 const QList<NetEntry>& incoming,
                                 MergePolicy policy);

    // Generate a fresh stable id (UUID without braces).
    static QString newId();
};

} // namespace AetherSDR
