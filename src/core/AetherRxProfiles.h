#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace AetherSDR {

class AudioEngine;

// JSON profile library for the AetherRX window, at
// ~/.config/AetherSDR/AetherRxProfiles.json; holds only what the operator saves
// and recalls (live state stays in AetherSDR.settings). RX-only: a profile
// carries the five RX chain stages, enables, order and NR method, and applying
// one writes nothing on the TX side. Format:
//   {
//     "version": 1,
//     "profiles": {
//       "DX Weak Signal": {
//         "createdBy": "AetherSDR x.y.z",
//         "createdAt": "ISO-8601",
//         "nr":    { "method": "NR4" },
//         "chain": ["Eq","Gate","Comp","Tube","Pudu"],
//         "gate":  { … }, "eq": { … }, "comp": { … },
//         "tube":  { … }, "pudu": { … }, "rn2": false
//       }
//     }
//   }
// Export writes one profile at top level with its "name"; import accepts that
// or a whole library.
class AetherRxProfiles : public QObject {
    Q_OBJECT

public:
    explicit AetherRxProfiles(AudioEngine* engine, QObject* parent = nullptr);

    QStringList profileNames() const;               // sorted, case-insensitive
    bool        hasProfile(const QString& name) const;

    // Capture the current receive chain and store it under `name`,
    // overwriting any profile already using it.
    bool saveFromCurrent(const QString& name);

    // Apply a stored profile to the engine's RX modules. False if absent.
    bool loadProfile(const QString& name);

    bool deleteProfile(const QString& name);

    // Write one profile to a standalone JSON file.
    bool exportToFile(const QString& name, const QString& filePath) const;

    // Read a file written by exportToFile() — or a whole library — without
    // storing anything. Returns the first profile it contains, with the name
    // the file suggests in `suggestedName`, so the caller can ask what to
    // save it as. Empty object on failure, with why in `error`.
    static QJsonObject readFile(const QString& filePath,
                                QString* suggestedName,
                                QString* error);

    // Store an already-parsed profile under `name`. Used to land an import
    // once the operator has named it.
    bool addProfile(const QString& name, const QJsonObject& profile);

    // "Rain Static" -> "Rain Static (2)" — the first spelling that is free.
    QString uniqueName(const QString& desired) const;

signals:
    void profilesChanged();

private:

    // One-time import of the retired ChannelStripPresets library: each preset's
    // receive half lands here and its transmit half in AetherTxProfiles, under the
    // preset's name. The legacy file is only read. The done flag lives in this
    // library's root (not AppSettings) so a deliberately deleted profile is never
    // re-imported.
    void migrateLegacyPresets();

    QString filePath() const;
    bool    loadFromDisk();
    // Atomically replace the library file with `root`. False on any failure,
    // with the file on disk left exactly as it was.
    bool    writeDocument(const QJsonObject& root) const;

    AudioEngine* m_engine{nullptr};
    QJsonObject  m_root;
};

} // namespace AetherSDR
