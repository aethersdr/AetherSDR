#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace AetherSDR {

class AudioEngine;

// JSON-backed profile library for the AetherTX window.
//
// Stored at ~/.config/AetherSDR/AetherTxProfiles.json. Live working state
// still lives in AetherSDR.settings through the per-module load/save paths;
// this file is only what the operator explicitly saves into and recalls from.
//
// TX-only, the mirror of AetherRxProfiles. ChannelStripPresets already stores
// a whole strip — both directions at once — so recalling one to change how you
// sound on air would take your receive chain with it. A profile here carries
// the seven transmit chain stages, their enables and their order, and the
// final limiter; applying one writes nothing on the RX side.
//
// Format:
//   {
//     "version": 1,
//     "profiles": {
//       "Contest Punch": {
//         "createdBy": "AetherSDR x.y.z",
//         "createdAt": "ISO-8601",
//         "chain": ["Gate","Eq","DeEss","Comp","Tube","Enh","Reverb"],
//         "gate":  { … }, "eq":     { … }, "deess": { … },
//         "comp":  { … }, "tube":   { … }, "pudu":  { … },
//         "reverb":{ … }, "finalLimiter": { … }
//       }
//     }
//   }
//
// Export writes one profile at the top level with its "name" alongside, so a
// file is readable on its own and easy to pass to someone else. Import
// accepts that form or a whole library.
class AetherTxProfiles : public QObject {
    Q_OBJECT

public:
    explicit AetherTxProfiles(AudioEngine* engine, QObject* parent = nullptr);

    QStringList profileNames() const;               // sorted, case-insensitive
    bool        hasProfile(const QString& name) const;

    // Capture the current transmit chain and store it under `name`,
    // overwriting any profile already using it.
    bool saveFromCurrent(const QString& name);

    // Apply a stored profile to the engine's TX modules. False if absent.
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

    // One-time import of the retired channel-strip library.
    //
    // ChannelStripPresets stored both directions in one preset, and the window
    // that was its only UI is gone. Rather than strand what an operator saved,
    // each preset is split: its transmit half lands here, its receive half in
    // AetherRxProfiles, both under the preset's own name. The legacy file is
    // read, never written or deleted -- if this goes wrong the original is
    // still there, and a future version can try again.
    //
    // Runs once. The flag lives in this library's own root rather than in
    // AppSettings so the decision travels with the file: re-importing after
    // the operator has deliberately deleted a migrated profile would be worse
    // than not importing at all.
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
