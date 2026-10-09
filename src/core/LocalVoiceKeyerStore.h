#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

// The client-side voice keyer's recordings on disk (RFC #4214): one WAV per
// slot, `slot-01.wav` … `slot-12.wav`, in a folder the operator can open, copy
// between computers, and drop WAVs into.
//
// Everything written here is 24 kHz / stereo / 16-bit PCM — the format the TX
// monitor tap delivers, so a recording is saved as captured and an imported
// file is converted once, on the way in. Writes go through QSaveFile, so an
// interrupted save never leaves a half-written slot. Files are read back
// through VoiceKeyerWavDecoder, which treats them as untrusted input — a
// dropped-in WAV is exactly that.
class LocalVoiceKeyerStore {
public:
    static constexpr int kSlotCount     = 12;
    static constexpr int kSampleRate    = 24000;
    static constexpr int kChannels      = 2;
    static constexpr int kMaxDurationMs = 60000;   // a CQ or an exchange, with room

    explicit LocalVoiceKeyerStore(QString dir);

    QString dir() const { return m_dir; }
    QString slotPath(int id) const;

    // Length of the slot's recording; 0 when the slot is empty or its file is
    // not a usable WAV.
    int durationMs(int id) const;

    // Save 24 kHz stereo int16 PCM as the slot's recording, replacing any
    // previous one.
    bool writeSlot(int id, const QByteArray& int16Stereo, QString& error);

    // Convert any supported WAV into the slot's format and save it.
    bool importSlot(int id, const QString& sourcePath, QString& error);

    // Copy the slot's recording out to destPath.
    bool exportSlot(int id, const QString& destPath, QString& error) const;

    // Delete the slot's recording. Succeeds when there was nothing to delete.
    bool removeSlot(int id, QString& error);

private:
    QString m_dir;
};

} // namespace AetherSDR
