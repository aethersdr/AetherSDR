#pragma once

#ifdef HAVE_MIDI

#include <QString>
#include <QVector>
#include "MidiControlManager.h"

namespace MasterSDR {

// Dedicated settings file for MIDI controller configuration.
// Stored at ~/.config/MasterSDR/midi.settings (XML format).
// Keeps MIDI bindings, device preferences, and profiles separate
// from the main MasterSDR.settings file.
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

    // Profile management (~/.config/MasterSDR/midi/<name>.xml)
    QStringList availableProfiles() const;
    void saveProfile(const QString& name, const QVector<MidiBinding>& bindings);
    QVector<MidiBinding> loadProfile(const QString& name) const;
    void deleteProfile(const QString& name);

private:
    MidiSettings() = default;
    QString settingsFilePath() const;
    QString profileDir() const;

    static QVector<MidiBinding> parseBindingsFromXml(const QString& filePath);
    static void writeBindingsToXml(const QString& filePath,
                                    const QVector<MidiBinding>& bindings);

    QString m_lastDevice;
    bool    m_autoConnect{true};
};

} // namespace MasterSDR

#endif // HAVE_MIDI
