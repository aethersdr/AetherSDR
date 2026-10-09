#pragma once

#include "VoiceKeyer.h"
#include "core/GeneratedAudioTransmitter.h"
#include "core/backends/TxAudioSource.h"
#include "core/LocalVoiceKeyerStore.h"

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QVector>

#include <functional>

namespace AetherSDR {

// The client-side voice keyer (RFC #4214): the DVK panel's slots, with the
// recordings kept on this computer instead of on the radio — no SmartSDR+
// licence, portable between radios, never touching a radio's stored DVK.
//
// Recording taps the audio AetherSDR already captures from the PC microphone
// (AudioEngine::txFinalMonitorPcmReady, after the TX voice chain), so it never
// keys the transmitter. That tap only carries the PC mic; with
// a radio-side mic source selected, REC refuses and says why rather than
// recording silence.
//
// On-air playback hands the slot's audio to the shared
// GeneratedAudioTransmitter, which owns keying, pacing and the unkey; every
// transmission starts from an operator PLAY or F-key and ends on its own.
class LocalVoiceKeyer : public VoiceKeyer {
    Q_OBJECT
public:
    static constexpr int kSlotCount = LocalVoiceKeyerStore::kSlotCount;

    explicit LocalVoiceKeyer(const QString& recordingsDir, QObject* parent = nullptr);

    Status status() const override { return m_status; }
    int activeId() const override { return m_activeId; }
    const QVector<VoiceKeyerRecording>& recordings() const override { return m_recordings; }

    void recStart(int id) override;
    void recStop(int id) override;
    void previewStart(int id) override;
    void previewStop(int id) override;
    void playbackStart(int id) override;
    void playbackStop(int id) override;
    void clear(int id) override;
    void remove(int id) override;
    void setName(int id, const QString& name) override;

    void importWav(int id, const QString& path) override;
    void exportWav(int id, const QString& path) override;
    bool canTransferWav() const override { return true; }
    bool isTransferring() const override { return false; }

    QString sourceLabel() const override { return QStringLiteral("Local"); }

    // A local recording may run to the store's cap — 60 s, against the radio
    // DVK's 10 s. The panel reads this rather than a shared constant, so its
    // progress bar and "x of y" readout follow whichever keyer is bound.
    int maxRecordingMs() const override { return LocalVoiceKeyerStore::kMaxDurationMs; }
    // Matches the radio's limit on purpose: a slot name typed under one keyer
    // then fits the other, so switching source never truncates a name.
    int maxNameBytes() const override { return 61; }

    QString recordingsDir() const { return m_store.dir(); }

    // ── Wiring (VoiceKeyerController) ────────────────────────────────────
    // Why recording cannot start now, or an empty string when it can. The
    // radio answers, because whether the PC microphone reaches the transmit
    // chain is a radio-side fact: a radio with its own mic input may be
    // listening to that instead, and a radio without one always takes PC
    // audio and never refuses.
    using RecordRefusalProbe = std::function<QString()>;
    void setRecordRefusalProbe(RecordRefusalProbe probe) { m_recordRefusal = std::move(probe); }

    // Preview plays a slot's file on this computer's speakers. start returns
    // false with a reason; the player reports the end through onPreviewFinished.
    using PreviewStart = std::function<bool(const QString& path, QString& error)>;
    using PreviewStop  = std::function<void()>;
    void setPreviewHandlers(PreviewStart start, PreviewStop stop);

    // Run around a recording. The hold runs first and answers whether recording
    // may begin now: false means it has asked the radio for something and the
    // recording waits for onRecordHoldReady() or onRecordHoldFailed(). The
    // release runs when the recording ends by any route. The radio side uses
    // them to hold VOX off, so the operator's own voice cannot key the
    // transmitter while they record.
    using RecordHold = std::function<bool()>;
    using RecordGuard = std::function<void()>;
    void setRecordGuards(RecordHold hold, RecordGuard release);

    // The hold that answered false has succeeded, or has given up. Recording
    // starts, or is refused with the reason. Both are no-ops once the operator
    // has cancelled the wait.
    void onRecordHoldReady();
    void onRecordHoldFailed(const QString& reason);

    // False while a recording is waiting on its hold, as well as while one is
    // running: the operation is already the operator's, and a second start
    // would race it.
    bool canStartOperation() const override;

    // On-air playback. Without one, PLAY refuses and says so.
    void setTransmitter(GeneratedAudioTransmitter* transmitter);

public slots:
    // AudioEngine::txFinalMonitorPcmReady — 24 kHz stereo int16. Only the
    // operator's own mic is recorded. The other two origins are somebody
    // else's audio on the same tap: ClientLeveled is a TCI/DAX application's,
    // and EngineGenerated is an unattended engine beacon (the WSPR pump).
    void onMicPcm(const QByteArray& int16Stereo, TxAudioSource source);
    void onPreviewFinished();

    // Re-read the folder and labels, e.g. after WAVs were dropped in.
    void reload();

private:
    void refreshSlot(int id);
    void setStatus(Status status, int id);
    void refuse(const QString& verb, int id, const QString& message);
    bool busyRefusal(const QString& verb, int id);
    // Non-empty: why this slot's file may not be played. The store caps what
    // it writes and what it imports, but a WAV the operator drops straight
    // into the folder has passed neither, so the length is checked at play.
    QString playableRefusal(int id) const;
    void onTransmitFinished(GeneratedAudioTransmitter::Outcome outcome, const QString& reason);

    LocalVoiceKeyerStore m_store;
    QVector<VoiceKeyerRecording> m_recordings;
    Status m_status{Idle};
    int m_activeId{-1};

    RecordRefusalProbe m_recordRefusal;
    RecordHold m_holdForRecording;
    RecordGuard m_releaseAfterRecording;
    // The slot whose recording is waiting on its hold, or -1. The status stays
    // Idle through the wait — the radio has not agreed yet, so nothing is
    // recording — and this is what keeps a second start out.
    int m_awaitingHoldSlot{-1};
    PreviewStart m_previewStart;
    PreviewStop m_previewStop;
    QPointer<GeneratedAudioTransmitter> m_transmitter;
    QMetaObject::Connection m_transmitterFinished;

    QByteArray m_recordBuffer;
    bool m_recordCapped{false};
};

} // namespace AetherSDR
