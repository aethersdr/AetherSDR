#include "MemoryCsvCompat.h"

#include "MemoryFieldValues.h"

#include <QHash>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcMemoryCsv, "aether.memory.csv")

namespace AetherSDR {

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
        MemoryFields::sanitizeText(memory.owner),
        MemoryFields::sanitizeText(memory.group),
        formatFrequency(memory.freq),
        MemoryFields::sanitizeText(memory.name),
        MemoryFields::sanitizeText(memory.mode).trimmed().toUpper(),
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
    // Strip NUL/control bytes from every free-text field on the way in so a
    // corrupt CSV (or one another program wrote) can't seed bad memories.
    memory.owner = MemoryFields::sanitizeText(normalizedFields.at(1)).trimmed();
    memory.group = MemoryFields::sanitizeText(normalizedFields.at(2)).trimmed();
    memory.name = MemoryFields::sanitizeText(normalizedFields.at(4)).trimmed();
    memory.mode = MemoryFields::sanitizeText(normalizedFields.at(5)).trimmed().toUpper();
    if (!memory.mode.isEmpty() && !MemoryFields::isKnownMode(memory.mode)) {
        qCInfo(lcMemoryCsv) << rowLabel << "imported with unrecognized mode"
                            << memory.mode << "- passing through to radio for validation";
    }

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

// ── CHIRP-next generic CSV import ───────────────────────────────────────────
//
// CHIRP (chirp_common.Memory.CSV_FORMAT) writes a header whose first column is
// "Location". The columns AetherSDR consumes, with CHIRP's units:
//   Frequency  MHz      ("146.520000")   -> MemoryEntry::freq      (MHz, same)
//   Duplex     +/-/split/off/""          -> offsetDir  up/down/simplex
//   Offset     MHz      ("0.600000")     -> repeaterOffset          (MHz, same)
//   Tone       Tone/TSQL/DTCS/Cross/""   -> toneMode   off/ctcss_tx
//   rToneFreq  CTCSS Hz (TX tone)        -> toneValue               (Hz)
//   cToneFreq  CTCSS Hz (RX tone, TSQL)  -> toneValue               (Hz)
//   Mode       FM/NFM/AM/USB/LSB/CW/...  -> mode (mapped to Flex modes)
//   TStep      kHz      ("5.00")         -> step  (Hz — scaled ×1000)
//   Name                                 -> name
// CHIRP-only columns (Location, DtcsCode, CrossMode, Skip, Power, Comment,
// URCALL/RPT*/DVCODE) have no MemoryEntry home and are dropped; the DTCS and
// D-STAR/DV tone systems the FlexRadio can't reproduce degrade to a plain
// (tone-off) memory rather than failing the row.

bool looksLikeChirpHeader(const QStringList& fields)
{
    if (fields.isEmpty()) {
        return false;
    }
    if (fields.at(0).trimmed().compare("Location", Qt::CaseInsensitive) != 0) {
        return false;
    }
    // Require the one column we cannot import a channel without.
    for (const QString& f : fields) {
        if (f.trimmed().compare("Frequency", Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

// CHIRP header name -> column index (case-insensitive), so we tolerate the
// column-count drift between CHIRP releases and only read what we recognize.
QHash<QString, int> chirpColumnIndex(const QStringList& header)
{
    QHash<QString, int> map;
    for (int i = 0; i < header.size(); ++i) {
        map.insert(header.at(i).trimmed().toLower(), i);
    }
    return map;
}

QString chirpField(const QStringList& fields, const QHash<QString, int>& cols, const char* name)
{
    const int idx = cols.value(QString::fromLatin1(name).toLower(), -1);
    if (idx < 0 || idx >= fields.size()) {
        return {};
    }
    return fields.at(idx).trimmed();
}

QString chirpModeToWire(const QString& chirpMode)
{
    static const QHash<QString, QString> kMap = {
        {"WFM", "FM"},   {"FM", "FM"},    {"NFM", "NFM"},
        {"AM", "AM"},    {"NAM", "AM"},
        {"USB", "USB"},  {"LSB", "LSB"},
        {"CW", "CW"},    {"CWR", "CW"},   {"NCW", "CW"},  {"NCWR", "CW"},
        {"RTTY", "RTTY"},{"RTTYR", "RTTY"},{"FSK", "RTTY"},{"FSKR", "RTTY"},
        {"DIG", "DIGU"}, {"PKT", "DIGU"},
        {"DV", "DSTR"},  {"DSTAR", "DSTR"},
    };
    const QString upper = MemoryFields::sanitizeText(chirpMode).trimmed().toUpper();
    const auto it = kMap.constFind(upper);
    if (it != kMap.constEnd()) {
        return it.value();
    }
    // Unknown/vendor mode (DMR, P25, DN, Auto…): pass the upper-case token
    // through and let the radio validate, matching the SmartSDR-import path.
    return upper;
}

QString chirpDuplexToOffsetDir(const QString& duplex)
{
    const QString d = duplex.trimmed().toLower();
    if (d == "+") { return "up"; }
    if (d == "-") { return "down"; }
    // "", "split", "off": the Flex memory model has no split/odd-split slot, so
    // anything that isn't a simple +/- repeater shift lands as simplex.
    return "simplex";
}

bool parseChirpRecord(const QStringList& fields,
                      const QHash<QString, int>& cols,
                      int lineNumber,
                      MemoryCsvRecord& record,
                      QString& error)
{
    MemoryEntry memory;

    memory.name = MemoryFields::sanitizeText(chirpField(fields, cols, "Name")).trimmed();

    const QString rowLabel = !memory.name.isEmpty()
        ? QString("Line %1 (%2)").arg(lineNumber).arg(memory.name)
        : QString("Line %1").arg(lineNumber);

    // Frequency — CHIRP writes MHz, the same unit as MemoryEntry::freq.
    const QString freqText = chirpField(fields, cols, "Frequency");
    if (!parseDoubleField(freqText, memory.freq) || !validateRange(memory.freq, 0.0, 10000.0)) {
        error = QString("%1: invalid CHIRP frequency '%2'.").arg(rowLabel).arg(freqText);
        return false;
    }

    // Mode.
    memory.mode = chirpModeToWire(chirpField(fields, cols, "Mode"));
    if (memory.mode.isEmpty()) {
        memory.mode = QStringLiteral("FM");  // CHIRP's implicit VHF/UHF default.
    }
    if (!MemoryFields::isKnownMode(memory.mode)) {
        qCInfo(lcMemoryCsv) << rowLabel << "CHIRP mode mapped to unrecognized"
                            << memory.mode << "- passing through to radio for validation";
    }

    // TStep — CHIRP writes kHz (e.g. "5.00"); MemoryEntry::step is Hz.
    const QString stepText = chirpField(fields, cols, "TStep");
    double stepKhz = 0.0;
    if (!stepText.isEmpty() && parseDoubleField(stepText, stepKhz) && stepKhz > 0.0) {
        memory.step = std::clamp(int(std::lround(stepKhz * 1000.0)), 1, 1000000);
    }
    // else: keep MemoryEntry's default step.

    // Duplex/Offset — CHIRP offset is MHz, matching repeaterOffset.
    memory.offsetDir = chirpDuplexToOffsetDir(chirpField(fields, cols, "Duplex"));
    if (memory.offsetDir == QLatin1String("simplex")) {
        memory.repeaterOffset = 0.0;
    } else {
        double offMhz = 0.0;
        if (parseDoubleField(chirpField(fields, cols, "Offset"), offMhz)) {
            memory.repeaterOffset = std::clamp(offMhz, -100.0, 100.0);
        }
    }

    // Tone — map CHIRP's tone-mode taxonomy onto the Flex off/ctcss_tx pair. A
    // FlexRadio memory carries a single TX CTCSS tone; DTCS and cross modes it
    // can't reproduce degrade to tone-off rather than failing the row.
    const QString tmode = chirpField(fields, cols, "Tone").toUpper();
    double tone = 0.0;
    if (tmode == QLatin1String("TONE")) {
        parseDoubleField(chirpField(fields, cols, "rToneFreq"), tone);
        memory.toneMode = QStringLiteral("ctcss_tx");
    } else if (tmode == QLatin1String("TSQL")) {
        parseDoubleField(chirpField(fields, cols, "cToneFreq"), tone);
        memory.toneMode = QStringLiteral("ctcss_tx");
    } else if (tmode == QLatin1String("CROSS")) {
        // Cross modes are "<tx>-><rx>"; honor the TX side when it is a tone.
        const QString cross = chirpField(fields, cols, "CrossMode").toUpper();
        if (cross.startsWith(QLatin1String("TONE"))) {
            parseDoubleField(chirpField(fields, cols, "rToneFreq"), tone);
            memory.toneMode = QStringLiteral("ctcss_tx");
        } else {
            memory.toneMode = QStringLiteral("off");
        }
    } else {
        memory.toneMode = QStringLiteral("off");  // "", DTCS, or unrecognized.
    }
    // CTCSS tones live in ~67–254.1 Hz; a parse miss leaves tone at 0.0, and
    // an inclusive 0-lower-bound accepted it — importing ctcss_tx with an
    // invalid 0 Hz tone instead of taking the off fallback below (#4129
    // review: reproducible with a trimmed export whose rToneFreq column is
    // absent while Tone=Tone). 50 Hz floors out missing/garbage values.
    if (memory.toneMode == QLatin1String("ctcss_tx") && validateRange(tone, 50.0, 300.0)) {
        memory.toneValue = tone;
    } else if (memory.toneMode == QLatin1String("ctcss_tx")) {
        memory.toneMode = QStringLiteral("off");  // tone freq missing/out of range.
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
    QHash<QString, int> chirpCols;
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
            if (!fields.isEmpty())
                fields[0] = trimBom(fields.at(0));

            // Native SmartSDR memory CSV: header must match exactly.
            if (fields.size() == kHeader.size() && fields == kHeader) {
                result.format = MemoryCsvFormat::SmartSdr;
                sawHeader = true;
                continue;
            }
            // CHIRP-next generic CSV: "Location"-led header; map by column name.
            if (looksLikeChirpHeader(fields)) {
                result.format = MemoryCsvFormat::Chirp;
                chirpCols = chirpColumnIndex(fields);
                sawHeader = true;
                continue;
            }

            result.errors << "Line 1: header does not match the SmartSDR or CHIRP memory CSV format.";
            return result;
        }

        MemoryCsvRecord record;
        QString error;
        const bool parsed = (result.format == MemoryCsvFormat::Chirp)
            ? parseChirpRecord(fields, chirpCols, i + 1, record, error)
            : parseRecord(fields, i + 1, record, error);
        if (!parsed) {
            result.errors << error;
            continue;
        }
        result.records << record;
    }

    if (!sawHeader)
        result.errors << "Missing SmartSDR or CHIRP memory CSV header.";

    return result;
}

QString MemoryCsvCompat::formatName(MemoryCsvFormat format)
{
    switch (format) {
    case MemoryCsvFormat::SmartSdr: return QStringLiteral("SmartSDR");
    case MemoryCsvFormat::Chirp:    return QStringLiteral("CHIRP");
    case MemoryCsvFormat::Unknown:  break;
    }
    return QStringLiteral("unknown");
}

MemoryCsvRecord MemoryCsvCompat::fromMemoryEntry(const MemoryEntry& memory)
{
    MemoryCsvRecord record;
    record.memory = memory;
    return record;
}

} // namespace AetherSDR
