#include "MemoryCsvCompat.h"

#include <QRegularExpression>

#include <algorithm>

namespace MasterSDR {

namespace {

constexpr int kExpectedColumnCount = 22;
constexpr int kMaxRfPower = 100;
constexpr int kMinSquelchLevel = 0;
constexpr int kMaxSquelchLevel = 100;
constexpr int kMinFilterHz = -12000;
constexpr int kMaxFilterHz = 12000;
constexpr int kMinRttyMarkHz = 100;
constexpr int kMaxRttyMarkHz = 4000;
constexpr int kMinRttyShiftHz = 0;
constexpr int kMaxRttyShiftHz = 4000;
constexpr int kMinDigitalOffsetHz = -12000;
constexpr int kMaxDigitalOffsetHz = 12000;

const QStringList kHeader = {
    QString(),
    "OWNER",
    "GROUP",
    "FREQ",
    "NAME",
    "MODE",
    "STEP",
    "OFFSET_DIRECTION",
    "REPEATER_OFFSET",
    "TONE_MODE",
    "TONE_VALUE",
    "RF_POWER",
    "RX_FILTER_LOW",
    "RX_FILTER_HIGH",
    "HIGHLIGHT",
    "HIGHLIGHT_COLOR",
    "SQUELCH",
    "SQUELCH_LEVEL",
    "RTTY_MARK",
    "RTTY_SHIFT",
    "DIGL_OFFSET",
    "DIGU_OFFSET"
};

QString trimBom(QString value)
{
    if (!value.isEmpty() && value.front() == QChar(0xfeff))
        value.remove(0, 1);
    return value;
}

QString csvEscape(const QString& value)
{
    if (value.contains(',') || value.contains('"') || value.contains('\n') || value.contains('\r')) {
        QString escaped = value;
        escaped.replace('"', "\"\"");
        return '"' + escaped + '"';
    }
    return value;
}

QString describeCsvRow(const QStringList& fields, int lineNumber)
{
    const QString name = fields.size() > 4 ? fields.at(4).trimmed() : QString();
    const QString freq = fields.size() > 3 ? fields.at(3).trimmed() : QString();

    if (!name.isEmpty())
        return QString("Line %1 (%2)").arg(lineNumber).arg(name);
    if (!freq.isEmpty())
        return QString("Line %1 (%2 MHz)").arg(lineNumber).arg(freq);
    return QString("Line %1").arg(lineNumber);
}

QStringList parseCsvLine(const QString& line, bool* ok)
{
    QStringList fields;
    QString current;
    bool inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == '"') {
                current += '"';
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
            continue;
        }

        if (ch == ',' && !inQuotes) {
            fields << current;
            current.clear();
            continue;
        }

        current += ch;
    }

    if (inQuotes) {
        if (ok)
            *ok = false;
        return {};
    }

    fields << current;
    if (ok)
        *ok = true;
    return fields;
}

bool parseDoubleField(const QString& text, double& value)
{
    bool ok = false;
    value = text.trimmed().toDouble(&ok);
    return ok;
}

bool parseIntField(const QString& text, int& value)
{
    bool ok = false;
    value = text.trimmed().toInt(&ok);
    return ok;
}

bool parseBool01Field(const QString& text, bool& value)
{
    const QString normalized = text.trimmed();
    if (normalized == "0") {
        value = false;
        return true;
    }
    if (normalized == "1") {
        value = true;
        return true;
    }
    return false;
}

QString normalizeOffsetDirection(const QString& value)
{
    const QString normalized = value.trimmed().toUpper();
    if (normalized == "UP")
        return "up";
    if (normalized == "DOWN")
        return "down";
    if (normalized == "SIMPLEX")
        return "simplex";
    return {};
}

QString normalizeToneMode(const QString& value)
{
    const QString normalized = value.trimmed().toUpper();
    if (normalized == "OFF")
        return "off";
    if (normalized == "CTCSS_TX")
        return "ctcss_tx";
    return {};
}

QString formatOffsetDirection(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == "up")
        return "UP";
    if (normalized == "down")
        return "DOWN";
    return "SIMPLEX";
}

QString formatToneMode(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == "ctcss_tx")
        return "CTCSS_TX";
    return "OFF";
}

bool validateRange(double value, double minValue, double maxValue)
{
    return value >= minValue && value <= maxValue;
}

bool validateRange(int value, int minValue, int maxValue)
{
    return value >= minValue && value <= maxValue;
}

QString formatDouble(double value, int decimals)
{
    return QString::number(value, 'f', decimals);
}

QString formatFrequency(double value)
{
    QString text = QString::number(value, 'f', 6);
    while (text.contains('.') && text.endsWith('0'))
        text.chop(1);
    if (text.endsWith('.'))
        text += '0';
    return text;
}

QString formatInt(int value)
{
    return QString::number(value);
}

QStringList recordToFields(const MemoryCsvRecord& record)
{
    const MemoryEntry& memory = record.memory;
    return {
        "MEMORY",
        memory.owner,
        memory.group,
        formatFrequency(memory.freq),
        memory.name,
        memory.mode.trimmed().toUpper(),
        formatInt(memory.step),
        formatOffsetDirection(memory.offsetDir),
        formatDouble(memory.repeaterOffset, 1),
        formatToneMode(memory.toneMode),
        formatDouble(memory.toneValue, 1),
        formatInt(std::clamp(record.rfPower, 0, kMaxRfPower)),
        formatInt(memory.rxFilterLow),
        formatInt(memory.rxFilterHigh),
        record.highlight ? "1" : "0",
        formatInt(std::max(0, record.highlightColor)),
        memory.squelch ? "1" : "0",
        formatInt(std::clamp(memory.squelchLevel, kMinSquelchLevel, kMaxSquelchLevel)),
        formatInt(memory.rttyMark),
        formatInt(memory.rttyShift),
        formatInt(memory.diglOffset),
        formatInt(memory.diguOffset)
    };
}

bool parseRecord(const QStringList& fields,
                 int lineNumber,
                 MemoryCsvRecord& record,
                 QString& error)
{
    QStringList normalizedFields = fields;
    if (normalizedFields.size() > kExpectedColumnCount) {
        const int extraFields = normalizedFields.size() - kExpectedColumnCount;
        const QString mergedName = normalizedFields.mid(4, extraFields + 1).join(',');
        normalizedFields.erase(normalizedFields.begin() + 4,
                               normalizedFields.begin() + 5 + extraFields);
        normalizedFields.insert(4, mergedName);
    }

    if (normalizedFields.size() != kExpectedColumnCount) {
        error = QString("%1: expected %2 columns, got %3.")
            .arg(describeCsvRow(normalizedFields, lineNumber))
            .arg(kExpectedColumnCount)
            .arg(normalizedFields.size());
        return false;
    }

    const QString rowLabel = describeCsvRow(normalizedFields, lineNumber);

    if (normalizedFields.at(0).trimmed().compare("MEMORY", Qt::CaseInsensitive) != 0) {
        error = QString("%1: unsupported record type '%2'.")
            .arg(rowLabel)
            .arg(normalizedFields.at(0));
        return false;
    }

    MemoryEntry memory;
    memory.owner = normalizedFields.at(1).trimmed();
    memory.group = normalizedFields.at(2).trimmed();
    memory.name = normalizedFields.at(4).trimmed();
    memory.mode = normalizedFields.at(5).trimmed().toUpper();

    if (!parseDoubleField(normalizedFields.at(3), memory.freq) || !validateRange(memory.freq, 0.0, 10000.0)) {
        error = QString("%1: invalid frequency '%2'.").arg(rowLabel).arg(normalizedFields.at(3));
        return false;
    }

    if (!parseIntField(normalizedFields.at(6), memory.step) || !validateRange(memory.step, 1, 1000000)) {
        error = QString("%1: invalid step '%2'.").arg(rowLabel).arg(normalizedFields.at(6));
        return false;
    }

    memory.offsetDir = normalizeOffsetDirection(normalizedFields.at(7));
    if (memory.offsetDir.isEmpty()) {
        error = QString("%1: invalid offset direction '%2'.").arg(rowLabel).arg(normalizedFields.at(7));
        return false;
    }

    if (!parseDoubleField(normalizedFields.at(8), memory.repeaterOffset)
            || !validateRange(memory.repeaterOffset, -100.0, 100.0)) {
        error = QString("%1: invalid repeater offset '%2'.").arg(rowLabel).arg(normalizedFields.at(8));
        return false;
    }

    memory.toneMode = normalizeToneMode(normalizedFields.at(9));
    if (memory.toneMode.isEmpty()) {
        error = QString("%1: invalid tone mode '%2'.").arg(rowLabel).arg(normalizedFields.at(9));
        return false;
    }

    if (!parseDoubleField(normalizedFields.at(10), memory.toneValue)
            || !validateRange(memory.toneValue, 0.0, 300.0)) {
        error = QString("%1: invalid tone value '%2'.").arg(rowLabel).arg(normalizedFields.at(10));
        return false;
    }

    if (!parseIntField(normalizedFields.at(11), record.rfPower) || !validateRange(record.rfPower, 0, kMaxRfPower)) {
        error = QString("%1: invalid RF power '%2'.").arg(rowLabel).arg(normalizedFields.at(11));
        return false;
    }

    if (!parseIntField(normalizedFields.at(12), memory.rxFilterLow)
            || !validateRange(memory.rxFilterLow, kMinFilterHz, kMaxFilterHz)) {
        error = QString("%1: invalid RX filter low '%2'.").arg(rowLabel).arg(normalizedFields.at(12));
        return false;
    }

    if (!parseIntField(normalizedFields.at(13), memory.rxFilterHigh)
            || !validateRange(memory.rxFilterHigh, kMinFilterHz, kMaxFilterHz)) {
        error = QString("%1: invalid RX filter high '%2'.").arg(rowLabel).arg(normalizedFields.at(13));
        return false;
    }

    if (!parseBool01Field(normalizedFields.at(14), record.highlight)) {
        error = QString("%1: invalid highlight flag '%2'.").arg(rowLabel).arg(normalizedFields.at(14));
        return false;
    }

    if (!parseIntField(normalizedFields.at(15), record.highlightColor) || record.highlightColor < 0) {
        error = QString("%1: invalid highlight color '%2'.").arg(rowLabel).arg(normalizedFields.at(15));
        return false;
    }

    if (!parseBool01Field(normalizedFields.at(16), memory.squelch)) {
        error = QString("%1: invalid squelch flag '%2'.").arg(rowLabel).arg(normalizedFields.at(16));
        return false;
    }

    if (!parseIntField(normalizedFields.at(17), memory.squelchLevel)
            || !validateRange(memory.squelchLevel, kMinSquelchLevel, kMaxSquelchLevel)) {
        error = QString("%1: invalid squelch level '%2'.").arg(rowLabel).arg(normalizedFields.at(17));
        return false;
    }

    if (!parseIntField(normalizedFields.at(18), memory.rttyMark)
            || !validateRange(memory.rttyMark, kMinRttyMarkHz, kMaxRttyMarkHz)) {
        error = QString("%1: invalid RTTY mark '%2'.").arg(rowLabel).arg(normalizedFields.at(18));
        return false;
    }

    if (!parseIntField(normalizedFields.at(19), memory.rttyShift)
            || !validateRange(memory.rttyShift, kMinRttyShiftHz, kMaxRttyShiftHz)) {
        error = QString("%1: invalid RTTY shift '%2'.").arg(rowLabel).arg(normalizedFields.at(19));
        return false;
    }

    if (!parseIntField(normalizedFields.at(20), memory.diglOffset)
            || !validateRange(memory.diglOffset, kMinDigitalOffsetHz, kMaxDigitalOffsetHz)) {
        error = QString("%1: invalid DIGL offset '%2'.").arg(rowLabel).arg(normalizedFields.at(20));
        return false;
    }

    if (!parseIntField(normalizedFields.at(21), memory.diguOffset)
            || !validateRange(memory.diguOffset, kMinDigitalOffsetHz, kMaxDigitalOffsetHz)) {
        error = QString("%1: invalid DIGU offset '%2'.").arg(rowLabel).arg(normalizedFields.at(21));
        return false;
    }

    record.memory = memory;
    return true;
}

} // namespace

QByteArray MemoryCsvCompat::serialize(const QList<MemoryCsvRecord>& records)
{
    QStringList lines;
    lines.reserve(records.size() + 1);
    lines << kHeader.join(',');

    for (const MemoryCsvRecord& record : records) {
        const QStringList fields = recordToFields(record);
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const QString& field : fields)
            escaped << csvEscape(field);
        lines << escaped.join(',');
    }

    return lines.join("\r\n").toUtf8() + QByteArray("\r\n");
}

MemoryCsvParseResult MemoryCsvCompat::parse(const QByteArray& bytes)
{
    MemoryCsvParseResult result;
    const QString text = QString::fromUtf8(bytes);
    const QStringList lines = text.split(QRegularExpression("\r\n|\n|\r"), Qt::KeepEmptyParts);

    bool sawHeader = false;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (i == 0)
            line = trimBom(line);
        if (line.isEmpty())
            continue;

        bool ok = false;
        QStringList fields = parseCsvLine(line, &ok);
        if (!ok) {
            result.errors << QString("Line %1: unterminated quoted field.").arg(i + 1);
            continue;
        }

        if (!sawHeader) {
            if (fields.size() != kHeader.size()) {
                result.errors << QString("Line 1: expected %1 header columns, got %2.")
                                 .arg(kHeader.size()).arg(fields.size());
                return result;
            }

            fields[0] = trimBom(fields.at(0));
            if (fields != kHeader) {
                result.errors << "Line 1: header does not match SmartSDR memory CSV format.";
                return result;
            }
            sawHeader = true;
            continue;
        }

        MemoryCsvRecord record;
        QString error;
        if (!parseRecord(fields, i + 1, record, error)) {
            result.errors << error;
            continue;
        }
        result.records << record;
    }

    if (!sawHeader)
        result.errors << "Missing SmartSDR memory CSV header.";

    return result;
}

MemoryCsvRecord MemoryCsvCompat::fromMemoryEntry(const MemoryEntry& memory)
{
    MemoryCsvRecord record;
    record.memory = memory;
    return record;
}

} // namespace MasterSDR
