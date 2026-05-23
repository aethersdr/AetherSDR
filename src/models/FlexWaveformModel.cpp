#include "FlexWaveformModel.h"
#include <QDebug>

namespace MasterSDR {

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
            const QStringList tokens = entry.split(QChar(0x7F));
            if (tokens.size() != 2) {
                qWarning() << "FlexWaveformModel: malformed installed_list entry:" << entry;
                continue;
            }
            FlexWaveformEntry e;
            e.name        = tokens[0];
            e.version     = tokens[1];
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

    // Add or update the container entry.
    for (FlexWaveformEntry& e : m_waveforms) {
        if (e.isContainer && e.name == name) {
            e.version = version;
            emit waveformsChanged();
            return;
        }
    }
    FlexWaveformEntry e;
    e.name        = name;
    e.version     = version;
    e.isContainer = true;
    m_waveforms.append(e);
    emit waveformsChanged();
}

// object == "waveform wfp_status", kvs contains power=, ready=, ipaddr=
// FlexLib Radio.cs ParseWfpStatus (line 11292).
void FlexWaveformModel::handleWfpStatus(const QMap<QString, QString>& kvs)
{
    bool changed = false;

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

    if (changed)
        emit wfpStatusChanged();
}

void FlexWaveformModel::clear()
{
    m_waveforms.clear();
    m_wfpPowered   = false;
    m_wfpReady     = false;
    m_wfpIpAddress.clear();
    emit waveformsChanged();
    emit wfpStatusChanged();
}

// ── Commands ─────────────────────────────────────────────────────────────────

void FlexWaveformModel::requestUninstall(const QString& name)
{
    emit commandReady(QStringLiteral("waveform uninstall ") + name);
}

void FlexWaveformModel::requestRemoveContainer(const QString& name)
{
    emit commandReady(QStringLiteral("waveform remove_container ") + name);
}

void FlexWaveformModel::requestRestart(const QString& name)
{
    emit commandReady(QStringLiteral("waveform restart ") + name);
}

} // namespace MasterSDR
