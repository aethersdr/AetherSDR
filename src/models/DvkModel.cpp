#include "DvkModel.h"
#include <QRegularExpression>
#include <QDebug>

namespace AetherSDR {

DvkModel::DvkModel(QObject* parent) : QObject(parent) {}

// ── Commands ────────────────────────────────────────────────────────────────
//
// Each command is reply-aware (#3377): RadioModel attaches a response
// callback that routes the radio's response code back through
// handleCommandResponse().  Before #3377 these were fire-and-forget, so a
// radio rejection (e.g. missing SmartSDR+ license, slot in wrong state)
// silently no-op'd and the panel never updated — most visibly the REC
// button toggled "checked" while the radio had refused rec_start.

void DvkModel::recStart(int id)
{
    emit replyCommandReady(QString("dvk rec_start id=%1").arg(id), "rec_start", id);
}
void DvkModel::recStop(int id)
{
    emit replyCommandReady(QString("dvk rec_stop id=%1").arg(id), "rec_stop", id);
}
void DvkModel::previewStart(int id)
{
    emit replyCommandReady(QString("dvk preview_start id=%1").arg(id), "preview_start", id);
}
void DvkModel::previewStop(int id)
{
    emit replyCommandReady(QString("dvk preview_stop id=%1").arg(id), "preview_stop", id);
}
void DvkModel::playbackStart(int id)
{
    emit replyCommandReady(QString("dvk playback_start id=%1").arg(id), "playback_start", id);
}
void DvkModel::playbackStop(int id)
{
    emit replyCommandReady(QString("dvk playback_stop id=%1").arg(id), "playback_stop", id);
}
void DvkModel::clear(int id)
{
    emit replyCommandReady(QString("dvk clear id=%1").arg(id), "clear", id);
}
void DvkModel::remove(int id)
{
    emit replyCommandReady(QString("dvk remove id=%1").arg(id), "remove", id);
}
void DvkModel::setName(int id, const QString& name)
{
    emit replyCommandReady(
        QString("dvk set_name name=\"%1\" id=%2").arg(name).arg(id),
        "set_name", id);
}

// ── Reply handling ──────────────────────────────────────────────────────────

void DvkModel::handleCommandResponse(const QString& verb, int id, uint code,
                                     const QString& body)
{
    Q_UNUSED(body);
    if (code == 0) return;  // success path: status broadcast drives UI

    qWarning() << "DvkModel: command" << verb << "id=" << id
               << "failed with code 0x" << QString::number(code, 16);
    emit commandFailed(verb, id, code, dvkErrorString(code));
}

QString DvkModel::dvkErrorString(uint code)
{
    // Known codes from FlexLib's SsdrErrors enum (reference/FlexLib/FlexLib/
    // SsdrErrors.cs) and SmartSDR command-error conventions.  Per
    // Principle I, do not invent labels for codes that are not documented
    // upstream — render those as bare hex so the user can report them.
    switch (code) {
    case 0x500000A9u: return QStringLiteral("port already in use on radio");
    default: break;
    }
    return QStringLiteral("error 0x%1").arg(code, 0, 16);
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

} // namespace AetherSDR
