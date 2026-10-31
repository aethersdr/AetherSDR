#pragma once
#include "models/Kxpa100Types.h"

namespace AetherSDR::Kxpa100 {
struct EepromReply {
    bool external = false;
    bool writeAck = false;
    int address = 0;
    int length = 0;
    QByteArray data;
};
QByteArray buildEepromRead(bool external, int address, int length);
// Captured firmware 01.39 restore regions: P[0020,0400), S[0000,3800).
QByteArray buildEepromWrite(bool external, int address, const QByteArray& data);
std::optional<EepromReply> parseEepromReply(const QByteArray& frame);
QByteArray encodeBackup(const Backup& backup, const QDateTime& savedAt);
std::optional<Backup> decodeBackup(const QByteArray& bytes, QString* error = nullptr);
bool saveBackup(const QString& path, const Backup& backup, QString* error = nullptr);
QString restoreAllowed(const Backup& backup, const QString& serial, const QString& firmware);
QList<FaultRecord> decodeFaultHistory(int newestSlot, const QByteArray& bytes);
QByteArray faultHistoryCsv(const QList<FaultRecord>& records);
} // namespace AetherSDR::Kxpa100
