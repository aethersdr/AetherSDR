#pragma once
#include "models/Kat500Types.h"

namespace AetherSDR::Kat500 {
struct EepromReply {
    bool external = false;
    bool writeAck = false;
    int address = 0;
    int length = 0;
    QByteArray data;
};
QByteArray buildEepromRead(bool external, int address, int length);
// Only captured 02.16 restore regions: P[0020,0400), S[1000,10000).
QByteArray buildEepromWrite(bool external, int address, const QByteArray& data);
std::optional<EepromReply> parseEepromReply(const QByteArray& frame);
QByteArray encodeBackup(const Backup& backup, const QDateTime& savedAt);
std::optional<Backup> decodeBackup(const QByteArray& bytes, QString* error = nullptr);
bool saveBackup(const QString& path, const Backup& backup, QString* error = nullptr);
QList<FaultRecord> decodeFaultHistory(int newestSlot, const QByteArray& bytes);
QByteArray faultHistoryCsv(const QList<FaultRecord>& records);
} // namespace AetherSDR::Kat500
