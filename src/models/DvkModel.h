#pragma once

#include <QObject>
#include <QString>
#include <QVector>
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

    // State
    enum Status { Unknown, Disabled, Idle, Recording, Preview, Playback };
    Status status() const { return m_status; }
    int activeId() const { return m_activeId; }
    bool enabled() const { return m_enabled; }
    const QVector<DvkRecording>& recordings() const { return m_recordings; }

    // Commands
    void recStart(int id);
    void recStop(int id);
    void previewStart(int id);
    void previewStop(int id);
    void playbackStart(int id);
    void playbackStop(int id);
    void clear(int id);
    void remove(int id);
    void setName(int id, const QString& name);

    // Status parsing (called from RadioModel)
    void applyStatus(const QString& object, const QMap<QString, QString>& kvs);

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

private:
    Status m_status{Unknown};
    int m_activeId{-1};
    bool m_enabled{false};
    QVector<DvkRecording> m_recordings;

    DvkRecording* findRecording(int id);
};

} // namespace AetherSDR
