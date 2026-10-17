#include "Kpa500Maintenance.h"

#include <QList>

#include <array>

namespace AetherSDR::Kpa500 {
namespace {

constexpr int kBlockSize = 64;
constexpr int kMemorySize = 1024;
constexpr int kConfigurationSize = 224;
constexpr int kRecordSize = 32;
constexpr int kMaximumFileSize = 64 * 1024;

bool validRange(quint16 address, qsizetype length, int limit)
{
    return length > 0 && length <= kBlockSize
        && static_cast<int>(address) + length <= limit;
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

std::optional<QByteArray> decodeHex(const QByteArray& text)
{
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    QByteArray bytes;
    bytes.reserve(text.size() / 2);
    for (qsizetype index = 0; index < text.size(); index += 2) {
        const int high = hexDigit(text[index]);
        const int low = hexDigit(text[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.append(static_cast<char>((high << 4) | low));
    }
    return bytes;
}

int byteSum(const QByteArray& bytes)
{
    int sum = 0;
    for (const char byte : bytes) {
        sum += static_cast<quint8>(byte);
    }
    return sum;
}

QByteArray eepromHeader(quint16 address, int length)
{
    QByteArray bytes;
    bytes.append(static_cast<char>(address >> 8));
    bytes.append(static_cast<char>(address & 0xff));
    bytes.append(static_cast<char>(length));
    return bytes;
}

QByteArray frame(const QByteArray& command, QByteArray bytes)
{
    bytes.append(static_cast<char>((-byteSum(bytes)) & 0xff));
    return '^' + command + bytes.toHex().toUpper() + ';';
}

bool asciiDigits(const QString& text)
{
    for (const QChar character : text) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return false;
        }
    }
    return !text.isEmpty();
}

bool validIdentity(const ConfigurationBackup& backup)
{
    return backup.serial.size() == 5 && asciiDigits(backup.serial)
        && backup.firmware.size() == 5 && backup.firmware[2] == QLatin1Char('.')
        && asciiDigits(backup.firmware.left(2)) && asciiDigits(backup.firmware.right(2));
}

quint32 updateCrc(quint32 crc, const QByteArray& bytes)
{
    // KPA Utility 1.13.7.16's observed backup CRC covers text without line
    // endings: reflected IEEE polynomial, initial FFFFFFFF, no final XOR.
    for (const char byte : bytes) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xedb88320U : 0U);
        }
    }
    return crc;
}

} // namespace

QByteArray buildEepromRead(quint16 address, int length)
{
    if (!validRange(address, length, kMemorySize)) {
        return {};
    }
    return frame(QByteArrayLiteral("ER"), eepromHeader(address, length));
}

QByteArray buildEepromWrite(quint16 address, const QByteArray& data)
{
    if (!validRange(address, data.size(), kConfigurationSize)) {
        return {};
    }
    return frame(QByteArrayLiteral("EW"), eepromHeader(address, static_cast<int>(data.size())) + data);
}

std::optional<EepromReply> parseEepromReply(const QString& command,
                                          const QString& argument)
{
    const bool writeAck = command.compare(QLatin1String("EW"), Qt::CaseInsensitive) == 0;
    if (!writeAck && command.compare(QLatin1String("ER"), Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    if (argument.size() < 8 || argument.size() > (kBlockSize + 4) * 2) {
        return std::nullopt;
    }
    for (const QChar character : argument) {
        if (character.unicode() > 0x7f) {
            return std::nullopt;
        }
    }
    const std::optional<QByteArray> bytes = decodeHex(argument.toLatin1());
    if (!bytes || (byteSum(*bytes) & 0xff) != 0) {
        return std::nullopt;
    }
    const quint16 address = static_cast<quint16>((static_cast<quint8>((*bytes)[0]) << 8)
                                                | static_cast<quint8>((*bytes)[1]));
    const int length = static_cast<quint8>((*bytes)[2]);
    if (!validRange(address, length, writeAck ? kConfigurationSize : kMemorySize)
        || bytes->size() != (writeAck ? 4 : length + 4)) {
        return std::nullopt;
    }
    return EepromReply{address, length, writeAck ? QByteArray{} : bytes->mid(3, length), writeAck};
}

QByteArray encodeConfigurationBackup(const ConfigurationBackup& backup,
                                    const QDateTime& savedAt)
{
    if (backup.data.size() != kConfigurationSize || !validIdentity(backup) || !savedAt.isValid()) {
        return {};
    }
    QList<QByteArray> lines{
        QByteArrayLiteral("*KPA Configuration saved on ")
            + savedAt.toString(QStringLiteral("yyyy-MM-dd 'at' HH:mm:ss")).toLatin1(),
        QByteArrayLiteral("*Do not edit this file!"),
        QByteArrayLiteral("*Make configuration changes using the KPA front panel and save the resulting configuration"),
        QByteArrayLiteral("KPA Utility Revision = AetherSDR"),
        QByteArrayLiteral("KPA Firmware Revision = ") + backup.firmware.toLatin1(),
        QByteArrayLiteral("KPA Serial Number = ") + backup.serial.toLatin1()
    };
    for (int address = 0; address < kConfigurationSize; address += kRecordSize) {
        const QByteArray header = eepromHeader(static_cast<quint16>(address), kRecordSize).toHex().toUpper();
        lines.append(QByteArrayLiteral("MEM2 ") + header.left(2) + ' ' + header.mid(2, 2)
                     + ' ' + header.right(2) + ' ' + backup.data.mid(address, kRecordSize).toHex().toUpper());
    }
    quint32 crc = 0xffffffffU;
    QByteArray encoded;
    for (const QByteArray& line : lines) {
        crc = updateCrc(crc, line);
        encoded += line + "\r\n";
    }
    encoded += QByteArrayLiteral("CRC32 = 0x") + QByteArray::number(crc, 16).rightJustified(8, '0').toUpper() + "\r\n";
    return encoded;
}

std::optional<ConfigurationBackup> decodeConfigurationBackup(const QByteArray& encoded,
                                                              QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [error](const QString& reason) -> std::optional<ConfigurationBackup> {
        if (error != nullptr) {
            *error = reason;
        }
        return std::nullopt;
    };
    if (encoded.isEmpty() || encoded.size() > kMaximumFileSize) {
        return fail(QStringLiteral("Configuration file is empty or exceeds 64 KiB."));
    }
    QList<QByteArray> lines = encoded.split('\n');
    if (lines.last().isEmpty()) {
        lines.removeLast();
    }
    if (lines.isEmpty()) {
        return fail(QStringLiteral("Configuration file has no records."));
    }
    ConfigurationBackup backup;
    backup.data = QByteArray(kConfigurationSize, '\0');
    std::array<bool, kConfigurationSize / kRecordSize> records{};
    bool hasUtility = false;
    bool hasFirmware = false;
    bool hasSerial = false;
    bool hasCrc = false;
    quint32 crc = 0xffffffffU;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        QByteArray line = lines[index];
        if (line.endsWith('\r')) {
            if (index == lines.size() - 1 && !encoded.endsWith('\n')) {
                return fail(QStringLiteral("Configuration contains a bare carriage return."));
            }
            line.chop(1);
        }
        if (line.isEmpty() || line.size() > 512) {
            return fail(QStringLiteral("Configuration contains an empty or oversized line."));
        }
        for (const char byte : line) {
            if (static_cast<quint8>(byte) < 0x20 || static_cast<quint8>(byte) > 0x7e) {
                return fail(QStringLiteral("Configuration must contain printable ASCII text."));
            }
        }
        if (line.startsWith("CRC32 = 0x")) {
            if (hasCrc || index != lines.size() - 1 || line.size() != 18) {
                return fail(QStringLiteral("Configuration CRC must occur once at the end."));
            }
            const std::optional<QByteArray> crcBytes = decodeHex(line.mid(10));
            if (!crcBytes || crcBytes->size() != 4) {
                return fail(QStringLiteral("Configuration CRC is not eight hexadecimal digits."));
            }
            quint32 stored = 0;
            for (const char byte : *crcBytes) {
                stored = (stored << 8) | static_cast<quint8>(byte);
            }
            if (stored != crc) {
                return fail(QStringLiteral("Configuration CRC does not match its contents."));
            }
            hasCrc = true;
            continue;
        }
        crc = updateCrc(crc, line);
        // Comments start with '*', remain printable and bounded, and participate
        // in the CRC exactly as written. No other unrecognized record is allowed.
        if (line.startsWith('*')) {
            continue;
        }
        if (line.startsWith("KPA Utility Revision = ")) {
            if (hasUtility || line.size() <= 23 || line.size() > 151 || line.mid(23).trimmed().isEmpty()) {
                return fail(QStringLiteral("Configuration utility identity is missing or duplicated."));
            }
            hasUtility = true;
        } else if (line.startsWith("KPA Firmware Revision = ")) {
            if (hasFirmware) {
                return fail(QStringLiteral("Configuration firmware is duplicated."));
            }
            backup.firmware = QString::fromLatin1(line.mid(24));
            hasFirmware = true;
        } else if (line.startsWith("KPA Serial Number = ")) {
            if (hasSerial) {
                return fail(QStringLiteral("Configuration serial number is duplicated."));
            }
            backup.serial = QString::fromLatin1(line.mid(20));
            hasSerial = true;
        } else if (line.startsWith("MEM2 ")) {
            const QList<QByteArray> fields = line.split(' ');
            if (fields.size() != 5 || fields[1].size() != 2 || fields[2].size() != 2
                || fields[3].size() != 2 || fields[4].size() != kRecordSize * 2) {
                return fail(QStringLiteral("Configuration memory record has an invalid shape."));
            }
            const std::optional<QByteArray> header = decodeHex(fields[1] + fields[2] + fields[3]);
            const std::optional<QByteArray> data = decodeHex(fields[4]);
            if (!header || !data) {
                return fail(QStringLiteral("Configuration memory record contains invalid hexadecimal digits."));
            }
            const int address = (static_cast<quint8>((*header)[0]) << 8) | static_cast<quint8>((*header)[1]);
            if (static_cast<quint8>((*header)[2]) != kRecordSize || address % kRecordSize != 0
                || address + kRecordSize > kConfigurationSize || records[address / kRecordSize]) {
                return fail(QStringLiteral("Configuration memory record overlaps or is outside the configuration region."));
            }
            records[address / kRecordSize] = true;
            backup.data.replace(address, kRecordSize, *data);
        } else {
            return fail(QStringLiteral("Configuration contains an unrecognized record."));
        }
    }
    if (!hasUtility || !hasFirmware || !hasSerial || !hasCrc || !validIdentity(backup)) {
        return fail(QStringLiteral("Configuration identity or checksum is missing or invalid."));
    }
    for (const bool present : records) {
        if (!present) {
            return fail(QStringLiteral("Configuration does not contain all 224 configuration bytes."));
        }
    }
    return backup;
}

} // namespace AetherSDR::Kpa500
