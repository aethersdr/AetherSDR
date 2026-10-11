#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QLatin1String>
#include <QMap>

namespace AetherSDR {

struct DvkRecording {
    int id{0};
    QString name;
    int durationMs{0};  // milliseconds
};

class DvkModel : public QObject {
    Q_OBJECT
public:
    explicit DvkModel(QObject* parent = nullptr);

    // The radio keeps slots 1-12 and stops a recording at 10 s (SmartSDR API
    // wiki, TCPIP-dvk). A name is at most 63 bytes including its quotes.
    static constexpr int kSlotCount = 12;
    static constexpr int kMaxRecordingMs = 10'000;
    static constexpr int kMaxNameBytes = 61;

    // State
    enum Status { Unknown, Disabled, Idle, Recording, Preview, Playback };
    Status status() const { return m_status; }
    int activeId() const { return m_activeId; }
    bool enabled() const { return m_enabled; }
    // The radio does not refuse overlapping operations, so a new one may only
    // start from idle (Unknown, before any status, fails open), with no start
    // of ours still awaiting its reply and no WAV transfer running.
    bool canStartOperation() const;
    // The start we sent and are still waiting on, or Unknown for none.
    Status pendingOperation() const { return m_pending; }
    // DvkWavTransfer reports a running import or export here.
    void setTransferActive(bool active);
    // The radio answered a dvk command with 50004001 (feature not licensed).
    bool licenseRefused() const { return m_licenseRefused; }
    // Latches a 50004001 refusal from any dvk path, including WAV transfers.
    void noteRefusal(uint code);
    // The radio later reported the feature licensed (its status outranks an
    // earlier refusal, e.g. a subscription activated mid-session).
    void clearRefusal();
    // The radio's name for the DVK entitlement in `license feature` status.
    static constexpr QLatin1String kLicenseFeature{"digital_voice_keyer"};
    const QVector<DvkRecording>& recordings() const { return m_recordings; }

    static QString defaultName(int id);
    // Drops the characters the radio cannot carry in a quoted name (`"`, `'`,
    // `|`), collapses spaces as the radio does, and trims to kMaxNameBytes.
    static QString sanitizeName(const QString& name);

    // Commands
    void recStart(int id);
    void recStop();
    void previewStart(int id);
    void previewStop();
    void playbackStart(int id);
    void playbackStop();
    void clear(int id);
    void setName(int id, const QString& name);

    // Status parsing (called from RadioModel)
    void applyStatus(const QString& object, const QMap<QString, QString>& kvs);

    // Connection-scoped state goes with the connection.
    void reset();

    // Called by RadioModel when a reply to a DVK command arrives.  Non-zero
    // codes are forwarded as commandFailed() so the UI can surface them
    // instead of leaving the operation silently rejected. (#3377)
    void handleCommandResponse(const QString& verb, int id, uint code, const QString& body);

    // Map a SmartSDR response code to a human-readable hint.  Known codes
    // come from FlexLib's SsdrErrors enum (Principle I); unknown codes
    // render as bare hex.
    static QString dvkErrorString(uint code);

signals:
    // Emitted for commands that need response correlation.  RadioModel
    // attaches a callback that invokes handleCommandResponse() with the
    // verb + slot id captured here. (#3377)
    void replyCommandReady(const QString& cmd, const QString& verb, int id);
    void statusChanged(Status status, int id);
    void recordingChanged(int id);
    void recordingsLoaded();
    // Fired when the radio rejects a DVK command (non-zero response code).
    // DvkPanel maps this to its status label and re-syncs button state so
    // the user sees the failure instead of a stuck "checked" REC button.
    void commandFailed(const QString& verb, int id, uint code, const QString& message);
    void licenseRefusedChanged(bool refused);
    // canStartOperation() may have changed (a start sent or answered, a
    // transfer begun or ended).
    void admissionChanged();

private:
    static constexpr uint kNotLicensed = 0x50004001u;

    Status m_status{Unknown};
    int m_activeId{-1};
    bool m_enabled{false};
    bool m_licenseRefused{false};
    Status m_pending{Unknown};
    int m_pendingId{-1};
    bool m_transferActive{false};

    void setPending(Status pending);
    QVector<DvkRecording> m_recordings;

    DvkRecording* findRecording(int id);
};

} // namespace AetherSDR
