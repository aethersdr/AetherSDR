#include "DvkModel.h"
#include <QRegularExpression>
#include <QDebug>

#include <utility>

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
    setPending(Recording);
    emit replyCommandReady(QString("dvk rec_start id=%1").arg(id), "rec_start", id);
}
void DvkModel::recStop()
{
    emit replyCommandReady(QStringLiteral("dvk rec_stop"), "rec_stop", m_activeId);
}
void DvkModel::previewStart(int id)
{
    setPending(Preview);
    emit replyCommandReady(QString("dvk preview_start id=%1").arg(id), "preview_start", id);
}
void DvkModel::previewStop()
{
    emit replyCommandReady(QStringLiteral("dvk preview_stop"), "preview_stop", m_activeId);
}
void DvkModel::playbackStart(int id)
{
    setPending(Playback);
    emit replyCommandReady(QString("dvk playback_start id=%1").arg(id), "playback_start", id);
}
void DvkModel::playbackStop()
{
    emit replyCommandReady(QStringLiteral("dvk playback_stop"), "playback_stop", m_activeId);
}
void DvkModel::clear(int id)
{
    // The radio is authoritative: fw 4.2.20 `clear` erases the audio and keeps
    // the slot's name, and the client leaves it that way.
    emit replyCommandReady(QString("dvk clear id=%1").arg(id), "clear", id);
}
void DvkModel::setName(int id, const QString& name)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty()) {
        return;
    }
    emit replyCommandReady(
        QString("dvk set_name name=\"%1\" id=%2").arg(clean).arg(id),
        "set_name", id);
}

QString DvkModel::defaultName(int id)
{
    return QStringLiteral("Recording %1").arg(id);
}

QString DvkModel::sanitizeName(const QString& name)
{
    QString clean = name;
    clean.remove(QLatin1Char('"'));
    clean.remove(QLatin1Char('\''));
    clean.remove(QLatin1Char('|'));
    clean = clean.simplified();
    // The limit is in UTF-8 bytes; never split a surrogate pair while trimming.
    while (clean.toUtf8().size() > kMaxNameBytes) {
        const qsizetype last = clean.size() - 1;
        if (last > 0 && clean.at(last).isLowSurrogate()
            && clean.at(last - 1).isHighSurrogate()) {
            clean.chop(2);
        } else {
            clean.chop(1);
        }
    }
    return clean.trimmed();
}

// ── Reply handling ──────────────────────────────────────────────────────────

void DvkModel::handleCommandResponse(const QString& verb, int id, uint code,
                                     const QString& body)
{
    Q_UNUSED(body);
    // The radio publishes a start's status before its reply, so by the reply
    // the status already holds the truth either way and the start is settled.
    if (verb.endsWith(QLatin1String("_start"))) {
        setPending(Unknown);
    }
    if (code == 0) {
        return;  // status broadcast drives the UI
    }

    noteRefusal(code);

    qWarning() << "DvkModel: command" << verb << "id=" << id
               << "failed with code 0x" << QString::number(code, 16);
    emit commandFailed(verb, id, code, dvkErrorString(code));
}

bool DvkModel::canStartOperation() const
{
    return (m_status == Idle || m_status == Unknown) && m_pending == Unknown && !m_transferActive;
}

void DvkModel::setPending(Status pending)
{
    if (m_pending == pending) {
        return;
    }
    m_pending = pending;
    emit admissionChanged();
}

void DvkModel::setTransferActive(bool active)
{
    if (m_transferActive == active) {
        return;
    }
    m_transferActive = active;
    emit admissionChanged();
}

void DvkModel::noteRefusal(uint code)
{
    if (code == kNotLicensed && !m_licenseRefused) {
        m_licenseRefused = true;
        emit licenseRefusedChanged(true);
    }
}

QString DvkModel::dvkErrorString(uint code)
{
    // Labels come from the SmartSDR API wiki (TCPIP-dvk) and FlexLib. Codes
    // documented in neither render as bare hex so the user can report them.
    switch (code) {
    case kNotLicensed:  return QStringLiteral("DVK requires an active SmartSDR+ subscription");
    case 0x50001000u:   return QStringLiteral("radio could not parse the command");
    case 0xE2000000u:   return QStringLiteral("no such slot, or an upload is already pending");
    case 0x50000053u:   return QStringLiteral("radio file server busy, try again in a moment");
    case 0x500000A9u:   return QStringLiteral("port already in use on radio");
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
        // FlexLib DVKStatus: enabled=0 means disabled, and the id is void.
        if (kvs.value("enabled") == "0") {
            newStatus = Disabled;
            id = -1;
        }

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

    // The name is always quoted and may contain spaces; parseKVs keeps a
    // quoted value whole, quotes included.
    QString name = kvs.value("name", "");
    name.remove('"');
    if (name.isEmpty())
        name = defaultName(id);

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

void DvkModel::reset()
{
    const bool wasRefused = m_licenseRefused;
    m_status = Unknown;
    m_activeId = -1;
    m_enabled = false;
    m_licenseRefused = false;
    m_pending = Unknown;
    m_transferActive = false;
    const QVector<DvkRecording> gone = std::exchange(m_recordings, {});
    emit statusChanged(m_status, m_activeId);
    for (const DvkRecording& r : gone) {
        emit recordingChanged(r.id);
    }
    if (wasRefused) {
        emit licenseRefusedChanged(false);
    }
    emit admissionChanged();
}

} // namespace AetherSDR
