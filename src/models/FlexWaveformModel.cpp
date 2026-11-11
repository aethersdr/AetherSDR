#include "FlexWaveformModel.h"
#include "core/backends/WaveformEnvRequest.h"
#include <QByteArray>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

namespace AetherSDR {

FlexWaveformModel::FlexWaveformModel(QObject* parent)
    : QObject(parent)
{}

// ── Status parsing ────────────────────────────────────────────────────────────

// object == "waveform", kvs contains installed_list=<entries>
// Each entry is "<name><version>" (DEL-separated), entries comma-separated.
// FlexLib Radio.cs UpdateWaveformsInstalledList (line 11162).
void FlexWaveformModel::handleInstalledList(const QMap<QString, QString>& kvs)
{
    const QString raw = kvs.value(QStringLiteral("installed_list"));

    // Remove all non-container (legacy) entries and rebuild from the list.
    m_waveforms.removeIf([](const FlexWaveformEntry& e){ return !e.isContainer; });

    if (!raw.isEmpty()) {
        const QStringList entries = raw.split(QLatin1Char(','));
        for (const QString& entry : entries) {
            if (entry.trimmed().isEmpty()) {
                qWarning() << "FlexWaveformModel: empty entry in installed_list, skipping";
                continue;
            }
            //  (DEL, ASCII 127) separates name from version within each entry
            QStringList tokens = entry.split(QChar(0x7F), Qt::SkipEmptyParts);
            if (tokens.size() < 2) {
                qWarning() << "FlexWaveformModel: malformed installed_list entry:" << entry;
                continue;
            }
            FlexWaveformEntry e;
            e.name        = tokens.first();
            e.version     = tokens.last();
            e.isContainer = false;
            m_waveforms.append(e);
        }
    }

    emit waveformsChanged();
}

// object == "waveform container", kvs contains name=, version=, and optionally removed
// "removed" is a bare-word key (no value) — detected via kvs.contains().
// FlexLib Radio.cs DockerUpdateWaveformList (line 11188).
void FlexWaveformModel::handleContainerStatus(const QMap<QString, QString>& kvs)
{
    const QString name    = kvs.value(QStringLiteral("name"));
    const QString version = kvs.value(QStringLiteral("version"));

    if (name.isEmpty()) {
        qWarning() << "FlexWaveformModel: container status missing name, ignoring";
        return;
    }

    if (kvs.contains(QStringLiteral("removed"))) {
        if (m_waveforms.removeIf([&name](const FlexWaveformEntry& e){
                return e.isContainer && e.name == name;
            }) > 0) {
            emit waveformsChanged();
        }
        return;
    }

    // Add or update the container entry. As in FlexLib, an absent env= is an
    // empty environment: every container status is a full snapshot.
    const QMap<QString, QString> environment =
        decodeEnvironment(kvs.value(QStringLiteral("env")));
    for (FlexWaveformEntry& e : m_waveforms) {
        if (e.isContainer && e.name == name) {
            const bool versionChanged = e.version != version;
            const bool environmentChanged = e.environment != environment;
            e.version = version;
            e.environment = environment;
            if (versionChanged) {
                emit waveformsChanged();
            } else if (environmentChanged) {
                emit this->environmentChanged(name);
            }
            return;
        }
    }
    FlexWaveformEntry e;
    e.name        = name;
    e.version     = version;
    e.isContainer = true;
    e.environment = environment;
    m_waveforms.append(e);
    emit waveformsChanged();
}

QMap<QString, QString> FlexWaveformModel::decodeEnvironment(const QString& base64)
{
    QMap<QString, QString> environment;
    if (base64.isEmpty()) {
        return environment;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError error{};
    const QJsonDocument doc = decoded
        ? QJsonDocument::fromJson(*decoded, &error) : QJsonDocument();
    if (!decoded || error.error != QJsonParseError::NoError || !doc.isObject()) {
        // The payload may hold secrets, so only its size is logged.
        qWarning() << "FlexWaveformModel: undecodable container env (" << base64.size()
                   << "chars), treating it as empty";
        return environment;
    }
    const QJsonObject object = doc.object();
    // FlexLib reads Dictionary<string, string>, which takes scalars as their text
    // and fails on a nested value.
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) {
            qWarning() << "FlexWaveformModel: container env is not a flat map, treating it as empty";
            return {};
        }
        environment.insert(it.key(), it.value().isString() ? it.value().toString()
                                                           : it.value().toVariant().toString());
    }
    return environment;
}

// object == "waveform restarted", kvs name= result=ok|error.
// FlexLib Radio.cs ParseWaveformRestarted (line 11804): only "error" fails.
void FlexWaveformModel::handleRestarted(const QMap<QString, QString>& kvs)
{
    const QString name = kvs.value(QStringLiteral("name"));
    if (name.isEmpty()) {
        return;
    }
    emit restarted(name, kvs.value(QStringLiteral("result"))
                             .compare(QLatin1String("error"), Qt::CaseInsensitive) != 0);
}

// object == "waveform wfp_status", kvs contains power=, ready=, ipaddr=
// FlexLib Radio.cs ParseWfpStatus (line 11292).
void FlexWaveformModel::handleWfpStatus(const QMap<QString, QString>& kvs)
{
    bool changed = !m_wfpStatusSeen;
    m_wfpStatusSeen = true;

    if (kvs.contains(QStringLiteral("power"))) {
        const bool powered = kvs[QStringLiteral("power")].compare(
            QLatin1String("on"), Qt::CaseInsensitive) == 0;
        if (m_wfpPowered != powered) {
            m_wfpPowered = powered;
            changed = true;
        }
    }
    if (kvs.contains(QStringLiteral("ready"))) {
        const bool ready = kvs[QStringLiteral("ready")].compare(
            QLatin1String("true"), Qt::CaseInsensitive) == 0;
        if (m_wfpReady != ready) {
            m_wfpReady = ready;
            changed = true;
        }
    }
    if (kvs.contains(QStringLiteral("ipaddr"))) {
        const QString addr = kvs[QStringLiteral("ipaddr")];
        if (m_wfpIpAddress != addr) {
            m_wfpIpAddress = addr;
            changed = true;
        }
    }

    if (changed) {
        emit wfpStatusChanged();
    }
}

void FlexWaveformModel::handleGenericStatus(const QMap<QString, QString>& kvs)
{
    if (kvs.isEmpty()) {
        return;
    }

    emit genericStatusReceived(kvs);
    m_statusReports.append(kvs);
    constexpr qsizetype kMaxStatusReports = 32;
    while (m_statusReports.size() > kMaxStatusReports) {
        m_statusReports.removeFirst();
    }
    emit statusReportsChanged();
}

void FlexWaveformModel::clear()
{
    m_waveforms.clear();
    m_statusReports.clear();
    m_wfpStatusSeen = false;
    m_wfpPowered   = false;
    m_wfpReady     = false;
    m_wfpIpAddress.clear();
    emit waveformsChanged();
    emit wfpStatusChanged();
    emit statusReportsChanged();
}

// ── Commands ─────────────────────────────────────────────────────────────────

void FlexWaveformModel::requestUninstall(const QString& name)
{
    if (!waveformNameTokenValid(name)) {
        qWarning() << "FlexWaveformModel: refusing unsafe waveform uninstall name:" << name;
        return;
    }
    emit controlRequested({WaveformAction::Uninstall, name});
}

void FlexWaveformModel::requestRemoveContainer(const QString& name)
{
    if (!waveformNameTokenValid(name)) {
        qWarning() << "FlexWaveformModel: refusing unsafe waveform container removal name:" << name;
        return;
    }
    emit controlRequested({WaveformAction::RemoveContainer, name});
}

void FlexWaveformModel::handleCommandReply(const WaveformRequest& request, const RadioControlReply& reply)
{
    emit commandFinished(waveformActionVerb(request.action), request.name, reply.kind == RadioControlReply::Kind::Accepted,
                         reply.message.trimmed());
}

void FlexWaveformModel::requestRestart(const QString& name)
{
    if (!waveformNameTokenValid(name)) {
        qWarning() << "FlexWaveformModel: refusing unsafe waveform restart name:" << name;
        return;
    }
    emit controlRequested({WaveformAction::Restart, name});
}

} // namespace AetherSDR
