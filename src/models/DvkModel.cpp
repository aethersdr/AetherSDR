#include "DvkModel.h"
#include <QRegularExpression>
#include <QDebug>

namespace MasterSDR {

DvkModel::DvkModel(QObject* parent) : QObject(parent) {}

// ── Commands ────────────────────────────────────────────────────────────────

void DvkModel::recStart(int id)    { emit commandReady(QString("dvk rec_start id=%1").arg(id)); }
void DvkModel::recStop(int id)     { emit commandReady(QString("dvk rec_stop id=%1").arg(id)); }
void DvkModel::previewStart(int id){ emit commandReady(QString("dvk preview_start id=%1").arg(id)); }
void DvkModel::previewStop(int id) { emit commandReady(QString("dvk preview_stop id=%1").arg(id)); }
void DvkModel::playbackStart(int id){ emit commandReady(QString("dvk playback_start id=%1").arg(id)); }
void DvkModel::playbackStop(int id){ emit commandReady(QString("dvk playback_stop id=%1").arg(id)); }
void DvkModel::clear(int id)       { emit commandReady(QString("dvk clear id=%1").arg(id)); }
void DvkModel::remove(int id)      { emit commandReady(QString("dvk remove id=%1").arg(id)); }
void DvkModel::setName(int id, const QString& name)
{
    emit commandReady(QString("dvk set_name name=\"%1\" id=%2").arg(name).arg(id));
}

// ── Status parsing ──────────────────────────────────────────────────────────

DvkRecording* DvkModel::findRecording(int id)
{
    for (auto& r : m_recordings)
        if (r.id == id) return &r;
    return nullptr;
}

void DvkModel::applyStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    // Global status: status=idle enabled=1
    if (kvs.contains("status")) {
        Status newStatus = Unknown;
        int id = -1;

        const QString& s = kvs["status"];
        if (s == "idle")           newStatus = Idle;
        else if (s == "recording") newStatus = Recording;
        else if (s == "preview")   newStatus = Preview;
        else if (s == "playback")  newStatus = Playback;
        else if (s == "disabled")  newStatus = Disabled;

        if (kvs.contains("id"))
            id = kvs["id"].toInt();

        if (kvs.contains("enabled"))
            m_enabled = kvs["enabled"] == "1";

        if (newStatus != m_status || id != m_activeId) {
            m_status = newStatus;
            m_activeId = id;
            emit statusChanged(m_status, m_activeId);
        }
        return;
    }

    // Deleted: object contains "deleted" (e.g. "dvk deleted id=1" or "dvk id=1 deleted")
    if (object.contains("deleted")) {
        if (kvs.contains("id")) {
            int id = kvs["id"].toInt();
            if (id > 0) {
                for (int i = 0; i < m_recordings.size(); ++i) {
                    if (m_recordings[i].id == id) {
                        m_recordings.removeAt(i);
                        emit recordingChanged(id);
                        break;
                    }
                }
            }
        }
        return;
    }

    // Added or updated: id=N name="..." duration=NNNN
    if (!kvs.contains("id")) return;
    int id = kvs["id"].toInt();
    if (id <= 0) return;

    // Name: may be truncated by KV parser (quotes + spaces).
    // Strip quotes and use default if empty.
    QString name = kvs.value("name", "");
    name.remove('"');
    if (name.isEmpty() || name.startsWith("Recording"))
        name = QString("Recording %1").arg(id);

    int duration = kvs.value("duration", "0").toInt();

    // Find or create recording
    auto* rec = findRecording(id);
    if (rec) {
        if (!name.isEmpty()) rec->name = name;
        rec->durationMs = duration;
    } else {
        DvkRecording newRec;
        newRec.id = id;
        newRec.name = name;
        newRec.durationMs = duration;
        m_recordings.append(newRec);
    }
    emit recordingChanged(id);

    // If we have all 12 slots, signal loaded
    if (m_recordings.size() >= 12)
        emit recordingsLoaded();
}

} // namespace MasterSDR
