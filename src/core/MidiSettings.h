#pragma once

#ifdef HAVE_MIDI

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include "MidiControlManager.h"

namespace AetherSDR {

// Result of importing a profile file. Import policy: import what maps,
// name what doesn't, never silently drop — every row that cannot become a
// binding is either a named skip (the file parsed, that row has no target
// here) or a file-level error (nothing was written).
struct MidiImportResult {
    int importedCount{0};
    QString profileName;             // store name the profile was saved under
    QStringList skippedUnknownParam; // functions/params with no target here
    QStringList skippedBadType;      // out-of-range type/channel/number rows
    QStringList duplicates;          // rows dropped as later duplicates
    QStringList errors;              // file-level problems; nothing written
    bool ok() const { return errors.isEmpty(); }
};

struct MidiExportResult {
    int exportedCount{0};
    QString error;
    bool ok() const { return error.isEmpty(); }
};

// Dedicated settings file for MIDI controller configuration.
// Stored at ~/.config/AetherSDR/midi.settings (XML format).
// Keeps MIDI bindings, device preferences, and profiles separate
// from the main AetherSDR.settings file.
class MidiSettings {
public:
    static MidiSettings& instance();

    // Load/save the default settings file
    void load();
    void save();

    // Bindings
    QVector<MidiBinding> loadBindings() const;
    void saveBindings(const QVector<MidiBinding>& bindings);

    // Device preferences
    QString lastDevice() const { return m_lastDevice; }
    void setLastDevice(const QString& name) { m_lastDevice = name; }

    bool autoConnect() const { return m_autoConnect; }
    void setAutoConnect(bool on) { m_autoConnect = on; }

    // Profile management (~/.config/AetherSDR/midi/<name>.xml)
    QStringList availableProfiles() const;
    // Returns false when the name is refused or the file could not be
    // written, so a caller can report the outcome instead of assuming it. (#5077)
    bool saveProfile(const QString& name, const QVector<MidiBinding>& bindings);
    QVector<MidiBinding> loadProfile(const QString& name) const;
    void deleteProfile(const QString& name);

    // A profile name is a name, not a path: the store concatenates it into
    // profileDir(), so all three operations above refuse a name this predicate
    // rejects (separators, a leading dot — hidden-file semantics that would
    // drop the file from the listing — and empty). Public so the GUI can
    // validate against the same rule it will be held to.
    static bool isValidProfileName(const QString& name);

    // Where the named profiles live. Public so the GUI can name the location
    // in a failed-write message. (#5077)
    static QString profileDir();

    // Whether a profile of this name is already on disk — answered by the
    // filesystem, not by a string compare, so it agrees with what saveProfile()
    // is about to do on every platform (case-insensitive APFS/NTFS overwrite
    // "foo" when asked for "Foo"; case-sensitive ext4 creates a second file).
    // Refused names are never "existing". (#5077)
    static bool profileExists(const QString& name);

    // Import a profile file into the store. Accepts the native <MidiProfile>
    // XML or a SmartSDR iOS/Mac ".map" (auto-detected by content). The store
    // name derives from the file name; an existing name gets a " (2)" style
    // suffix rather than being overwritten. paramValidator returns true when
    // the action registry knows the id (the GUI passes a findParam() check;
    // tests inject a fixed set); an empty validator accepts every id.
    MidiImportResult importProfile(
        const QString& filePath,
        const std::function<bool(const QString&)>& paramValidator = {});

    // Export bindings as a shareable <MidiProfile> XML — the same document
    // importProfile() reads back, so Export → Import round-trips.
    MidiExportResult exportProfile(const QString& filePath,
                                   const QVector<MidiBinding>& bindings) const;

private:
    MidiSettings() = default;
    QString settingsFilePath() const;

    static QVector<MidiBinding> parseBindingsFromXml(const QString& filePath);
    static bool writeBindingsToXml(const QString& filePath,
                                   const QVector<MidiBinding>& bindings);

    QString m_lastDevice;
    bool    m_autoConnect{true};
};

} // namespace AetherSDR

#endif // HAVE_MIDI
