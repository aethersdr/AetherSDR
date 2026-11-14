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

// The radio's waveform status (decoded by the Flex status ingress).
void FlexWaveformModel::applyStatus(const WaveformStatusDelta& d)
{
    switch (d.kind) {
    case WaveformStatusDelta::Kind::InstalledList: applyInstalledList(d); return;
    case WaveformStatusDelta::Kind::Container: applyContainer(d); return;
    case WaveformStatusDelta::Kind::Wfp: applyWfpStatus(d); return;
    case WaveformStatusDelta::Kind::Restarted: emit restarted(d.name, d.ok); return;
    case WaveformStatusDelta::Kind::Report: handleGenericStatus(d.report); return;
    }
}

// Every legacy (non-container) entry is replaced by the radio's list.
void FlexWaveformModel::applyInstalledList(const WaveformStatusDelta& d)
{
    m_waveforms.removeIf([](const FlexWaveformEntry& e){ return !e.isContainer; });
    for (const auto& [name, version] : d.installed) {
        FlexWaveformEntry e;
        e.name        = name;
        e.version     = version;
        e.isContainer = false;
        m_waveforms.append(e);
    }
    emit waveformsChanged();
}

void FlexWaveformModel::applyContainer(const WaveformStatusDelta& d)
{
    const QString& name = d.name;
    if (d.removed) {
        if (m_waveforms.removeIf([&name](const FlexWaveformEntry& e){
                return e.isContainer && e.name == name;
            }) > 0) {
            emit waveformsChanged();
        }
        return;
    }
    for (FlexWaveformEntry& e : m_waveforms) {
        if (e.isContainer && e.name == name) {
            const bool versionChanged = e.version != d.version;
            const bool environmentChanged = e.environment != d.environment;
            e.version = d.version;
            e.environment = d.environment;
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
    e.version     = d.version;
    e.isContainer = true;
    e.environment = d.environment;
    m_waveforms.append(e);
    emit waveformsChanged();
}

void FlexWaveformModel::applyWfpStatus(const WaveformStatusDelta& d)
{
    bool changed = !m_wfpStatusSeen;
    m_wfpStatusSeen = true;
    if (d.powered && m_wfpPowered != *d.powered) {
        m_wfpPowered = *d.powered;
        changed = true;
    }
    if (d.ready && m_wfpReady != *d.ready) {
        m_wfpReady = *d.ready;
        changed = true;
    }
    if (d.ipAddress && m_wfpIpAddress != *d.ipAddress) {
        m_wfpIpAddress = *d.ipAddress;
        changed = true;
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
