#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace AetherSDR::Kpa500 {

struct ConfigurationBackup {
    QByteArray data;
    QString firmware;
    QString serial;
};

// Invalid ranges return an empty command. Writes are limited to configuration
// bytes 0x0000..0x00df; both commands allow at most 64 bytes.
// Neither helper sends anything to a device.
QByteArray buildEepromRead(quint16 address, int length);
QByteArray buildEepromWrite(quint16 address, const QByteArray& data);

struct EepromReply {
    quint16 address = 0;
    int length = 0;
    QByteArray data;
    bool writeAck = false;
};

// command is ER or EW; argument excludes the command prefix and semicolon.
std::optional<EepromReply> parseEepromReply(const QString& command,
                                          const QString& argument);

// Exactly 224 configuration bytes and the reported firmware/serial are needed.
// Invalid input returns an empty encoding. Parsing validates the complete file.
// Files may use LF or CRLF; printable ASCII comment lines begin with an asterisk.
// All text, including comments and whitespace, participates in the file checksum.
QByteArray encodeConfigurationBackup(const ConfigurationBackup& backup,
                                    const QDateTime& savedAt);
std::optional<ConfigurationBackup> decodeConfigurationBackup(
    const QByteArray& encoded, QString* error = nullptr);

} // namespace AetherSDR::Kpa500
