#pragma once

#include "models/MemoryEntry.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Portable, versioned JSON persistence for the CLIENT-side memory bank — the
// channels an operator saves on a radio that has no memory storage of its own
// (Hermes-Lite 2, Kiwi, the demo backend). On a Flex the radio owns the slots
// and this file is never touched; see RadioCapabilities::persistsMemories.
//
// Envelope:
//   {
//     "format": "aether.memories",
//     "version": 1,
//     "savedAt": "2026-07-29T14:00:00Z",
//     "savedBy": "AetherSDR",
//     "memories": [ { "index": 0, ...MemoryEntry... } ]
//   }
//
// Evolved additively the same way the net schedule is: new fields are optional
// with MemoryEntry's defaults, unknown fields are ignored, and a file whose
// version is NEWER than this build is reported as an error rather than
// half-read — a downgrade must not silently drop channels it cannot represent
// and then write the loss back to disk.
//
// The slot index is stored, not derived from array position. It is the handle
// the whole memory UX addresses a channel by (spot ids, `memory apply`, the
// browse panel), so it has to survive a save/load cycle intact even when the
// bank is sparse.
class LocalMemoryStore {
public:
    static constexpr int kFormatVersion = 1;
    static constexpr const char* kFormatId = "aether.memories";

    struct ParseResult {
        QMap<int, MemoryEntry> memories;
        QStringList errors;
        int version{0};
        // The file exists but could not be UNDERSTOOD: unparseable JSON, a
        // non-object root, a foreign format id, or a version newer than this
        // build. Distinct from `errors`, which also carries recoverable
        // complaints (a skipped duplicate slot, an entry with no index) where
        // everything else in the file was read correctly.
        //
        // This is the flag that decides whether overwriting is safe. Anything
        // this build could not read is somebody's data it cannot represent, so
        // saving over it would destroy channels rather than lose a field —
        // which is the whole reason the version check exists (see above).
        bool unreadable{false};

        bool ok() const { return errors.isEmpty(); }
        // Safe to replace this file wholesale with what we parsed?
        bool overwritable() const { return !unreadable; }
    };

    // Serialize to pretty-printed JSON bytes, ordered by slot index so the file
    // stays diffable across saves. `savedAtIso` is stamped into the envelope —
    // a parameter rather than a clock read so this stays deterministic and
    // testable (the same reason NetScheduleStore takes one).
    static QByteArray serialize(const QMap<int, MemoryEntry>& memories,
                                const QString& savedAtIso = {});

    static ParseResult parse(const QByteArray& bytes);

    // ~/.config/AetherSDR/memories.json (and the platform equivalents), matching
    // the GenericConfigLocation base AppSettings uses. Empty only if Qt cannot
    // resolve a writable config location at all.
    static QString defaultFilePath();

    // Reads `path`. A missing file is NOT an error — it is an empty bank, which
    // is what a first run looks like.
    static ParseResult load(const QString& path);

    // Atomic write via QSaveFile: the bank is only ever replaced wholesale, so a
    // crash mid-save must leave the previous file intact rather than a truncated
    // one. Returns false and fills `error` on failure.
    static bool save(const QString& path,
                     const QMap<int, MemoryEntry>& memories,
                     const QString& savedAtIso = {},
                     QString* error = nullptr);
};

}  // namespace AetherSDR
