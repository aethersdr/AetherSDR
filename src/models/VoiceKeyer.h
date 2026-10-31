#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

struct VoiceKeyerRecording {
    int id{0};
    QString name;
    int durationMs{0};  // milliseconds; 0 = empty slot
};

// A voice keyer as the DVK panel drives it: numbered slots holding short
// recordings, record / preview / on-air playback intent, and a status the
// panel mirrors. Slot ids are 1-based, matching the radio DVK and F1-F12.
//
// This is the core half of the split the touchpoint audit already names for
// DvkModel ("Core: voice-keyer slots/status + rec/preview/playback intent").
// DvkModel is the radio-hosted implementation — SmartSDR `dvk` verbs,
// recordings stored on the radio, SmartSDR+ required. RFC #4214's client-side
// keyer is the second — recordings stored on this computer, no licence. The
// panel sees only this interface, so one surface serves both.
class VoiceKeyer : public QObject {
    Q_OBJECT
public:
    enum Status { Unknown, Disabled, Idle, Recording, Preview, Playback };
    Q_ENUM(Status)

    explicit VoiceKeyer(QObject* parent = nullptr);
    ~VoiceKeyer() override;

    virtual Status status() const = 0;
    virtual int activeId() const = 0;  // -1 when nothing is active
    virtual const QVector<VoiceKeyerRecording>& recordings() const = 0;

    virtual void recStart(int id) = 0;
    virtual void recStop(int id) = 0;
    virtual void previewStart(int id) = 0;
    virtual void previewStop(int id) = 0;
    virtual void playbackStart(int id) = 0;  // on-air
    virtual void playbackStop(int id) = 0;
    virtual void clear(int id) = 0;
    virtual void remove(int id) = 0;
    virtual void setName(int id, const QString& name) = 0;

    // Copy a WAV into or out of a slot. Progress and the outcome arrive as
    // transferStatusChanged / transferFinished.
    virtual void importWav(int id, const QString& path) = 0;
    virtual void exportWav(int id, const QString& path) = 0;
    virtual bool canTransferWav() const = 0;  // false: import/export unavailable
    virtual bool isTransferring() const = 0;

    // ── Admission ────────────────────────────────────────────────────
    // Whether a new operation may start now. The panel asks before it sends
    // one, because a keyer need not refuse an overlapping operation itself —
    // the radio DVK does not. The default answers from status alone, which is
    // right for a keyer whose operations begin and end locally; DvkModel
    // overrides it to also wait out a command still in flight and any running
    // WAV transfer. Unknown (no status yet) fails open.
    virtual bool canStartOperation() const;
    // A start this keyer has sent and is still waiting on, or Unknown for
    // none. Only a keyer that round-trips to hardware has one.
    virtual Status pendingOperation() const { return Unknown; }

    // ── Per-keyer limits the panel shows and enforces ───────────────────
    // How long a recording may run. The panel sizes its progress bar and its
    // "x of y" readout from this, so it is the keyer's own cap, not a shared
    // constant: the radio stops at the radio's limit, the client-side keyer at
    // its own.
    virtual int maxRecordingMs() const = 0;
    // The slot-name limit, in UTF-8 bytes (the radio counts bytes, not
    // characters).
    virtual int maxNameBytes() const = 0;
    // The name an empty slot shows. Both keyers use "Recording N" so an
    // operator sees the same empty panel either way.
    virtual QString defaultSlotName(int id) const;

    // Short name for where recordings live — "Radio" or "Local" — shown with
    // the panel title so the operator always knows which keyer they drive.
    virtual QString sourceLabel() const = 0;

signals:
    void statusChanged(AetherSDR::VoiceKeyer::Status status, int id);
    void recordingChanged(int id);
    void recordingsLoaded();
    // A command was refused. The panel shows the message and re-syncs its
    // buttons so a refused press doesn't leave one latched. (#3377)
    void commandFailed(const QString& verb, int id, uint code, const QString& message);
    // canStartOperation() may have changed (a start sent or answered, a
    // transfer begun or ended).
    void admissionChanged();
    void transferStatusChanged(const QString& message);
    void transferFinished(bool success, const QString& message);
};

} // namespace AetherSDR
