#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace AetherSDR {

class AudioEngine;

// JSON-backed preset library for the Aetherial Audio Channel Strip.
//
// Stored at ~/.config/AetherSDR/ChannelStrip.settings as a single
// JSON document.  Live working state continues to live in
// AetherSDR.settings via the existing per-DSP-module load/save paths
// — this file is purely a preset library that the user explicitly
// saves into and recalls from.
//
// Format:
//   {
//     "version": 1,
//     "presets": {
//       "Broadcast Voice": {
//         "createdBy": "AetherSDR x.y.z",
//         "createdAt": "ISO-8601",
//         "chain":     ["Gate","Eq",...],
//         "gate":      { ... },
//         "eq":        { ... },
//         "comp":      { ... },
//         "deess":     { ... },
//         "tube":      { ... },
//         "pudu":      { ... },
//         "reverb":    { ... }
//       },
//       "Contest Punch": { ... }
//     }
//   }
//
// Single-file Export writes a one-preset file with the same per-preset
// schema at the top level (no "presets" wrapper) so it's easy to
// share online.  Import accepts either form.
class ChannelStripPresets : public QObject {
    Q_OBJECT

public:
    explicit ChannelStripPresets(AudioEngine* engine,
                                 QObject* parent = nullptr);

    QStringList presetNames() const;              // sorted alpha
    bool        hasPreset(const QString& name) const;

    // Apply a stored preset to all engine modules.  Returns false if
    // the preset doesn't exist.
    bool loadPreset(const QString& name);

    // Capture current engine state and save as a named preset.
    // Overwrites any existing preset with the same name.
    bool savePresetFromCurrent(const QString& name);

    bool deletePreset(const QString& name);

    // Export a stored preset to a single-preset JSON file for sharing.
    bool exportPresetToFile(const QString& name,
                            const QString& filePath) const;

    // Export the entire local library (all presets) as a single JSON
    // file in the same format as the on-disk store.  Useful for
    // bundling a personal preset collection or backing up a setup.
    bool exportLibraryToFile(const QString& filePath) const;

    // Capture *current* engine state and write it as a single-preset
    // JSON file under the given name.  Useful for "save the current
    // mix straight to a shareable file" without first stashing it
    // into the local library.
    bool exportCurrentToFile(const QString& presetName,
                             const QString& filePath) const;

    // Reads a single-preset file OR a full library file, merges any
    // presets it contains into the local store, and returns the name
    // of the first preset imported (empty string on failure).
    QString importPresetFromFile(const QString& filePath);

    // The RX half of a preset, on its own. AetherRxProfiles stores exactly
    // this object as a profile, so both libraries read and write one schema.
    // applyRxJson persists the RX modules it touched and nothing else.
    // Where the legacy channel-strip library lives. Public and static so the
    // per-direction profile stores can migrate it once without owning an
    // instance — see AetherTxProfiles::migrateLegacyPresets().
    static QString legacyLibraryPath();

    static QJsonObject captureRxJson(AudioEngine* engine);
    static void        applyRxJson(AudioEngine* engine, const QJsonObject& rx);

    // The TX half, likewise. Unlike the RX block these fields sit loose at the
    // top level of a preset, so capture returns them unwrapped and apply reads
    // them from wherever the caller keeps them.
    static QJsonObject captureTxJson(AudioEngine* engine);
    static void        applyTxJson(AudioEngine* engine, const QJsonObject& tx);

signals:
    void presetsChanged();

private:
    QString     filePath() const;
    bool        loadFromDisk();
    bool        saveToDisk() const;
    QJsonObject capturePresetJson() const;
    void        applyPresetJson(const QJsonObject& preset);

    AudioEngine* m_engine{nullptr};
    QJsonObject  m_root;
};

} // namespace AetherSDR
