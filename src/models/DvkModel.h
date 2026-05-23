#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QMap>

namespace MasterSDR {

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

signals:
    void commandReady(const QString& cmd);
    void statusChanged(Status status, int id);
    void recordingChanged(int id);
    void recordingsLoaded();

private:
    Status m_status{Unknown};
    int m_activeId{-1};
    bool m_enabled{false};
    QVector<DvkRecording> m_recordings;

    DvkRecording* findRecording(int id);
};

} // namespace MasterSDR
