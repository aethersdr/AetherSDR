#include "DvkModel.h"
#include <QRegularExpression>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QPointer>

#include <utility>

namespace AetherSDR {

DvkModel::DvkModel(QObject* parent) : VoiceKeyer(parent) {}

namespace {
QString operationName(DvkRequest::Operation operation)
{
    using Operation = DvkRequest::Operation;
    switch (operation) {
    case Operation::RecordStart: return QStringLiteral("rec_start");
    case Operation::RecordStop: return QStringLiteral("rec_stop");
    case Operation::PreviewStart: return QStringLiteral("preview_start");
    case Operation::PreviewStop: return QStringLiteral("preview_stop");
    case Operation::PlaybackStart: return QStringLiteral("playback_start");
    case Operation::PlaybackStop: return QStringLiteral("playback_stop");
    case Operation::Clear: return QStringLiteral("clear");
    case Operation::Remove: return QStringLiteral("remove");
    case Operation::Name: return QStringLiteral("set_name");
    }
    return {};
}
}

void DvkModel::request(const DvkRequest& command)
{
    if (QThread::currentThread() != thread()) { return; }
    if (!command.valid() || (!command.stops() && m_requests.size() >= 29)
        || (!command.stops() && (m_transferActive || isTransferring()))
        || (command.starts() && !canStartOperation())) {
        emit commandFailed(operationName(command.operation), command.slot, 0,
                           tr("Voice keyer request was not dispatched."));
        return;
    }
    // Reserve one pending request for each global stop verb. Repeated stops
    // share the unanswered request, so cleanup stays available and bounded.
    if (command.stops()) {
        for (const DvkRequest& pending : m_requests) {
            if (pending.operation == command.operation) { return; }
        }
    }
    const quint64 requestId = ++m_nextRequest;
    m_requests.insert(requestId, command);
    const QPointer<DvkModel> alive(this);
    if (command.starts()) {
        m_startRequest = requestId;
        m_startObserved = false;
        m_pendingId = command.slot;
        const Status pending = command.operation == DvkRequest::Operation::RecordStart ? Recording
            : command.operation == DvkRequest::Operation::PreviewStart ? Preview : Playback;
        setPending(pending);
    }
    // Admission observers may reset the session before dispatch.
    if (!alive || !m_requests.contains(requestId)) { return; }
    QTimer::singleShot(30000, this, [this, requestId] {
        handleControlReply(requestId, {TransmitServiceReply::Outcome::Unknown, 0,
                                      tr("No radio response; the operation may still be running.")});
    });
    emit controlRequested(command, requestId);
}

void DvkModel::recStart(int id) { request({DvkRequest::Operation::RecordStart, id, {}}); }
void DvkModel::previewStart(int id) { request({DvkRequest::Operation::PreviewStart, id, {}}); }
void DvkModel::playbackStart(int id) { request({DvkRequest::Operation::PlaybackStart, id, {}}); }
// Stop is explicit operator intent. The radio stops its global operation;
// changing the selected slot must not redirect or suppress that intent.
void DvkModel::recStop(int) { request({DvkRequest::Operation::RecordStop, m_activeId > 0 ? m_activeId : m_pendingId, {}}); }
void DvkModel::previewStop(int) { request({DvkRequest::Operation::PreviewStop, m_activeId > 0 ? m_activeId : m_pendingId, {}}); }
void DvkModel::playbackStop(int) { request({DvkRequest::Operation::PlaybackStop, m_activeId > 0 ? m_activeId : m_pendingId, {}}); }
void DvkModel::clear(int id) { request({DvkRequest::Operation::Clear, id, {}}); }
void DvkModel::remove(int id) { request({DvkRequest::Operation::Remove, id, {}}); }
void DvkModel::setName(int id, const QString& name)
{
    const QString clean = sanitizeName(name);
    if (!clean.isEmpty()) { request({DvkRequest::Operation::Name, id, clean}); }
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
    clean.remove(QChar(0x5c));
    clean.remove(QChar(0));
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

void DvkModel::importWav(int id, const QString& path)
{
    emit wavUploadRequested(id, path);
}
void DvkModel::exportWav(int id, const QString& path)
{
    emit wavDownloadRequested(id, path);
}

// ── Reply handling ──────────────────────────────────────────────────────────

void DvkModel::handleControlReply(quint64 requestId, const TransmitServiceReply& reply)
{
    const auto found = m_requests.find(requestId);
    if (found == m_requests.end()) { return; }
    const DvkRequest command = *found;
    m_requests.erase(found);
    using Outcome = TransmitServiceReply::Outcome;
    if (reply.outcome == Outcome::Accepted) { return; }
    const QPointer<DvkModel> alive(this);
    if (requestId == m_startRequest && !m_startObserved
        && (reply.outcome == Outcome::NotDispatched || reply.outcome == Outcome::Rejected)) {
        m_startRequest = 0;
        m_pendingId = -1;
        setPending(Unknown);
        if (!alive) { return; }
        emit operationSettled(requestId);
    }
    if (!alive) { return; }
    if (reply.outcome == Outcome::Rejected) { noteRefusal(reply.code); }
    if (alive) {
        emit commandFailed(operationName(command.operation), command.slot, reply.code,
            reply.outcome == Outcome::Rejected ? dvkErrorString(reply.code) : reply.detail);
    }
}

bool DvkModel::canStartOperation() const
{
    return (m_status == Idle || m_status == Unknown) && m_pending == Unknown && !m_transferActive && !m_licenseRefused;
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

void DvkModel::clearRefusal()
{
    if (m_licenseRefused) {
        m_licenseRefused = false;
        emit licenseRefusedChanged(false);
    }
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

        const quint64 originalStart = m_startRequest;
        if (originalStart && newStatus == m_pending && id == m_pendingId) {
            m_startObserved = true;
        }
        const bool settled = originalStart && m_startObserved
            && (newStatus == Idle || newStatus == Disabled);
        if (settled) {
            m_requests.remove(originalStart);
            m_startRequest = 0;
            m_startObserved = false;
            m_pending = Unknown;
            m_pendingId = -1;
        }
        const QPointer<DvkModel> alive(this);
        if (newStatus != m_status || id != m_activeId) {
            // A stop for the previous observed operation must not suppress a
            // fresh stop after the radio advances to another activity.
            for (auto pending = m_requests.begin(); pending != m_requests.end();) {
                if (pending->stops()) { pending = m_requests.erase(pending); }
                else { ++pending; }
            }
            m_status = newStatus;
            m_activeId = id;
            emit statusChanged(m_status, m_activeId);
        }
        if (alive && settled) {
            emit operationSettled(originalStart);
            if (alive) { emit admissionChanged(); }
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
    m_requests.clear();
    m_startRequest = 0;
    m_startObserved = false;
    m_status = Unknown;
    m_activeId = -1;
    m_enabled = false;
    m_licenseRefused = false;
    m_pending = Unknown;
    m_pendingId = -1;
    // m_transferActive belongs to DvkWavTransfer, which clears it when its own
    // transfer ends; a transfer can outlive the connection by its timeout.
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
