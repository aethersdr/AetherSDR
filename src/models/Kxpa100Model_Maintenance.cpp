#include "Kxpa100Model.h"
#include "core/Kxpa100Protocol.h"
#include "core/Kxpa100Maintenance.h"
#include <algorithm>
#include <utility>
namespace AetherSDR {
using namespace Kxpa100;
void Kxpa100Model::readRegion(bool external, int address, int end, std::function<void(QByteArray)> complete)
{
    m_regionExternal = external; m_regionAddress = address; m_regionEnd = end;
    m_regionData.clear(); m_regionComplete = std::move(complete); readRegionBlock();
}
void Kxpa100Model::readRegionBlock()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_regionAddress >= m_regionEnd) {
        auto complete = std::move(m_regionComplete); QByteArray data = std::move(m_regionData);
        if (complete) { complete(data); } return;
    }
    const int address = m_regionAddress; const int length = std::min(64,m_regionEnd-address); const bool external = m_regionExternal;
    enqueue({buildEepromRead(external,address,length),{},[external,address,length](const QByteArray& frame) {
        const auto reply = parseEepromReply(frame);
        return reply && !reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
    },[this,length](const QByteArray& frame) {
        m_regionData += parseEepromReply(frame)->data; m_regionAddress += length;
        m_progress += length; emit operationProgress(m_progress,m_total); readRegionBlock();
    }});
}
bool Kxpa100Model::readBackup()
{
    if (!begin(QStringLiteral("Read complete backup"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("01.39")) { fail(QStringLiteral("Complete EEPROM support is verified only for firmware 01.39.")); return false; }
    m_backup = {}; m_pendingBackup = {}; m_total = 1024 + 16384;
    preflight([this] {
        m_pendingBackup.serial = m_status.serial; m_pendingBackup.firmware = m_status.firmware;
        readRegion(false,0,1024,[this](QByteArray data) {
            m_pendingBackup.internal = std::move(data);
            readRegion(true,0,16384,[this](QByteArray data) {
                m_pendingBackup.external = std::move(data);
                preflight([this] {
                    m_backup = std::move(m_pendingBackup);
                    const Backup result = m_backup;
                    finish(QStringLiteral("Complete backup read, including fault history.")); emit backupRead(result);
                });
            });
        });
    }); return true;
}
bool Kxpa100Model::restoreBackup(const Backup& backup, bool confirmed)
{
    if (!confirmed) { emit operationFailed(QStringLiteral("Explicit restore confirmation is required.")); return false; }
    const QString reason = restoreAllowed(backup,m_status.serial,m_status.firmware);
    if (!reason.isEmpty()) { emit operationFailed(reason); return false; }
    if (!begin(QStringLiteral("Restore and verify backup"),true)) { return false; }
    m_restore = backup; m_restoreExternal = false; m_restoreAddress = 32; m_total = 992 + 14336;
    preflight([this] { restoreBlock(); }); return true;
}
void Kxpa100Model::restoreBlock()
{
    if (!m_connected || !operationBusy()) { return; }
    const int end = m_restoreExternal ? 0x3800 : 1024;
    if (m_restoreAddress >= end) { restoreNextRegion(); return; }
    if (!fresh(QStringLiteral("^PI"),1000) || !fresh(QStringLiteral("^PF"),1000) || !fresh(QStringLiteral("^TP"),1000)) {
        preflight([this] { restoreBlock(); }); return;
    }
    const int address = m_restoreAddress; const bool external = m_restoreExternal;
    const int count = std::min({64, end-address, 256-(address%256)});
    const QByteArray data = (external ? m_restore.external : m_restore.internal).mid(address,count);
    const int length = data.size();
    enqueue({buildEepromWrite(external,address,data),{},[external,address,length](const QByteArray& frame) {
        const auto reply = parseEepromReply(frame); return reply && reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
    },[this,external,address,data,length](const QByteArray&) {
        enqueue({buildEepromRead(external,address,length),{},[external,address,length](const QByteArray& frame) {
            const auto reply = parseEepromReply(frame); return reply && !reply->writeAck && reply->external == external && reply->address == address && reply->length == length;
        },[this,data,length](const QByteArray& frame) {
            if (parseEepromReply(frame)->data != data) { fail(QStringLiteral("EEPROM readback mismatch. Restore stopped without retry or restart.")); return; }
            m_restoreAddress += length; m_progress += length; emit operationProgress(m_progress,m_total); restoreBlock();
        }});
    },3000,true});
}
void Kxpa100Model::restoreNextRegion()
{
    if (!m_connected || !operationBusy()) { return; }
    if (!m_restoreExternal) { m_restoreExternal = true; m_restoreAddress = 0; restoreBlock(); }
    else {
        m_configuration.reset();
        restartAfterMaintenance(QStringLiteral("Restore readback verified; identity and fault history preserved. Restart requested. Reconnect to verify restored settings."));
    }
}
bool Kxpa100Model::readFaultHistory()
{
    if (!begin(QStringLiteral("Read fault history"),false)) { return false; }
    if (m_status.firmware != QStringLiteral("01.39")) { fail(QStringLiteral("Fault-history decoding is verified only for firmware 01.39.")); return false; }
    m_total = 2049;
    preflight([this] {
        readRegion(false,31,32,[this](QByteArray pointer) {
            const int slot = static_cast<quint8>(pointer[0]);
            readRegion(true,0x3800,0x4000,[this,slot](QByteArray data) {
                m_faults = decodeFaultHistory(slot,data); const QList<FaultRecord> result = m_faults;
                finish(QStringLiteral("Fault history read; current fault unchanged.")); emit faultHistoryRead(result);
            });
        });
    }); return true;
}
bool Kxpa100Model::readConfiguration()
{
    if (!begin(QStringLiteral("Read antenna configuration"),false,true)) { return false; }
    enqueue({"^AEA;",{},[](const QByteArray& frame) { return parseConfiguration(frame).has_value(); },
        [this](const QByteArray& frame) {
            m_configuration = parseConfiguration(frame);
            const Configuration result = *m_configuration;
            finish(QStringLiteral("Antenna configuration read.")); emit configurationRead(result);
        }}); return true;
}
bool Kxpa100Model::applyConfiguration(const Configuration& configuration)
{
    if (!m_configuration) { emit operationFailed(QStringLiteral("Read antenna configuration before applying changes.")); return false; }
    for (const int mask : configuration) {
        if (mask < 1 || mask > 3) { emit operationFailed(QStringLiteral("Enable at least one antenna on every band.")); return false; }
    }
    if (!begin(QStringLiteral("Apply antenna configuration"),true,true)) { return false; }
    m_edits.clear();
    for (int band = 0; band < 11; ++band) {
        if ((*m_configuration)[band] != configuration[band]) { m_edits.enqueue({band,configuration[band]}); }
    }
    m_total = m_edits.size(); preflight([this] { applyNextSetting(); }); return true;
}
void Kxpa100Model::applyNextSetting()
{
    if (!m_connected || !operationBusy()) { return; }
    if (m_edits.isEmpty()) {
        const Configuration result = *m_configuration;
        finish(QStringLiteral("All changed antenna settings confirmed.")); emit configurationRead(result); return;
    }
    if (!fresh(QStringLiteral("^PI"),1000) || !fresh(QStringLiteral("^PF"),1000) || !fresh(QStringLiteral("^TP"),1000)) {
        preflight([this] { applyNextSetting(); }); return;
    }
    const auto [band,mask] = m_edits.dequeue();
    enqueue({configurationSet(band,mask) + "^AEA;",{},[](const QByteArray& frame) { return parseConfiguration(frame).has_value(); },
        [this,band,mask](const QByteArray& frame) {
            m_configuration = parseConfiguration(frame);
            const Configuration confirmed = *m_configuration;
            const quint64 generation = m_generation;
            emit configurationRead(confirmed);
            if (!m_connected || generation != m_generation || !operationBusy()) { return; }
            if (confirmed[band] != mask) { fail(QStringLiteral("Antenna setting was not confirmed; remaining edits discarded.")); return; }
            emit operationProgress(++m_progress,m_total); applyNextSetting();
        },2000,true});
}
bool Kxpa100Model::eraseMemories(int band, int antenna, bool confirmed)
{
    const QByteArray command = buildErase(band,antenna);
    if (!confirmed || command.isEmpty() || !begin(QStringLiteral("Erase selected tuner memories"),true,true)) { return false; }
    preflight([this,command] {
        enqueue({command + "^RV;","^RV",[](const QByteArray& frame) { return matchesStatusQuery("^RV;",frame); },
            [this](const QByteArray&) { finish(QStringLiteral("Erase command processed. Inspect affected frequencies to verify memory contents.")); },6000,true});
    }); return true;
}
void Kxpa100Model::restartAfterMaintenance(const QString& message)
{
    if (!m_connected || !operationBusy()) { return; }
    if (!idle()) { fail(QStringLiteral("Restart stopped: RF or tuning is active or stale. Confirmed writes remain applied.")); return; }
    if (!write("^RS;")) { return; }
    finish(message);
    // Reset drops the serial session and can change its baud. Never replay writes
    // or enable RF on reconnect; identification starts afresh.
    disconnectDevice();
}
bool Kxpa100Model::factoryReset(bool confirmed)
{
    if (!confirmed || !begin(QStringLiteral("Reset configuration"),true)) { return false; }
    preflight([this] {
        enqueue({"^EC;^RV;","^RV",[](const QByteArray& frame) { return matchesStatusQuery("^RV;",frame); },
            [this](const QByteArray&) {
                preflight([this] { restartAfterMaintenance(QStringLiteral("Reset and restart requested. Reconnect at the device's default baud and verify configuration.")); });
            },6000,true});
    }); return true;
}
QByteArray Kxpa100Model::encodeBackupFile(const Backup& backup) { return encodeBackup(backup,QDateTime::currentDateTime()); }
std::optional<Backup> Kxpa100Model::decodeBackupFile(const QByteArray& bytes, QString* error) { return decodeBackup(bytes,error); }
bool Kxpa100Model::saveBackupFile(const QString& path, const Backup& backup, QString* error) { return saveBackup(path,backup,error); }
QByteArray Kxpa100Model::faultsCsv(const QList<FaultRecord>& faults) { return faultHistoryCsv(faults); }
} // namespace AetherSDR
