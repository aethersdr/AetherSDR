#include "AdifParser.h"
#include "CtyDatParser.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QThread>
#include <QtAlgorithms>

#include <utility>

namespace AetherSDR {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

QString AdifParser::normaliseMode(const QString& mode, const QString& submode)
{
    // Check submode first (handles MFSK/FT8, MFSK/FT4, etc.)
    const QString sub = submode.toUpper();
    if (!sub.isEmpty()) {
        if (sub == "FT8" || sub == "FT4" || sub == "JS8" ||
            sub == "JT65" || sub == "JT9" || sub == "WSPR" ||
            sub == "PSK31" || sub == "PSK63" || sub == "RTTY")
            return "DATA";
    }

    const QString m = mode.toUpper();
    if (m == "CW")   return "CW";
    if (m == "SSB" || m == "USB" || m == "LSB" || m == "AM" || m == "FM")
        return "PHONE";
    if (m == "FT8"  || m == "FT4"  || m == "RTTY" || m == "PSK31" ||
        m == "PSK63" || m == "WSPR" || m == "JT65" || m == "JT9"  ||
        m == "JS8"   || m == "MFSK" || m == "OLIVIA" || m == "CONTESTIA" ||
        m == "SSTV"  || m == "PACKET" || m == "HELL"  || m == "ATV")
        return "DATA";
    // Unknown — treat as DATA
    if (!m.isEmpty()) return "DATA";
    return "PHONE";  // default
}

QString AdifParser::freqToBand(double mhz)
{
    if (mhz >= 1.8   && mhz < 2.0)   return "160m";
    if (mhz >= 3.5   && mhz < 4.0)   return "80m";
    if (mhz >= 5.0   && mhz < 5.6)   return "60m";
    if (mhz >= 7.0   && mhz < 7.3)   return "40m";
    if (mhz >= 10.1  && mhz < 10.15) return "30m";
    if (mhz >= 14.0  && mhz < 14.35) return "20m";
    if (mhz >= 18.068&& mhz < 18.168)return "17m";
    if (mhz >= 21.0  && mhz < 21.45) return "15m";
    if (mhz >= 24.89 && mhz < 24.99) return "12m";
    if (mhz >= 28.0  && mhz < 29.7)  return "10m";
    if (mhz >= 50.0  && mhz < 54.0)  return "6m";
    if (mhz >= 70.0  && mhz < 70.5)  return "4m";
    if (mhz >= 144.0 && mhz < 148.0) return "2m";
    if (mhz >= 430.0 && mhz < 440.0) return "70cm";
    return {};
}

// ---------------------------------------------------------------------------
// Core parser
// ---------------------------------------------------------------------------

QVector<QsoRecord> AdifParser::parse(const QByteArray& data)
{
    QVector<QsoRecord> records;
    const QString text = QString::fromUtf8(data);

    // ADIF IV.A.1: data is length-delimited, including ignored fields.
    // Only look for another specifier after consuming that data, so literal
    // <CALL>, <EOH> or <EOR> text in a comment cannot become structure.
    static const QRegularExpression fieldRe(
        R"(<([^<>:]+)(?::([0-9]+)(?::[A-Z])?)?>)",
        QRegularExpression::CaseInsensitiveOption);
    QHash<QString, QString> fields;
    qsizetype pos = 0;
    while (pos < text.size()) {
        const QRegularExpressionMatch match = fieldRe.match(text, pos);
        if (!match.hasMatch()) {
            break;
        }
        pos = match.capturedEnd();
        const QString name = match.captured(1).toUpper();
        if (!match.captured(2).isEmpty()) {
            bool validLength = false;
            const qlonglong length = match.captured(2).toLongLong(&validLength);
            // Subtract first: an untrusted length must never overflow a
            // start + length expression. A malformed field reads as absent;
            // scanning continues so later well-formed fields can survive.
            validLength = validLength && length <= text.size() - pos;
            if ((name == "CALL" || name == "BAND" || name == "FREQ"
                 || name == "MODE" || name == "SUBMODE") && !fields.contains(name)) {
                fields.insert(name, validLength ? text.mid(pos, length) : QString());
            }
            if (validLength) {
                pos += length;
            }
            continue;
        }
        if (name == "EOH") {
            fields.clear();
            records.clear();
            continue;
        }
        if (name != "EOR") {
            continue;
        }
        const QHash<QString, QString> recordFields = std::move(fields);
        fields.clear();

        QsoRecord rec;
        rec.callsign = recordFields.value("CALL").trimmed().toUpper();
        if (rec.callsign.isEmpty()) continue;

        // Band: prefer explicit <BAND> field, fall back to <FREQ> → freqToBand.
        // Some loggers export bare numbers ("10", "20") instead of ADIF-standard
        // labels ("10m", "20m") — normalise those here.
        rec.band = recordFields.value("BAND").trimmed().toLower();
        if (!rec.band.isEmpty() && !rec.band.endsWith('m') && !rec.band.endsWith("cm")) {
            static const QHash<QString,QString> bandMap = {
                {"160","160m"},{"80","80m"},{"60","60m"},{"40","40m"},
                {"30","30m"},{"20","20m"},{"17","17m"},{"15","15m"},
                {"12","12m"},{"10","10m"},{"6","6m"},{"4","4m"},
                {"2","2m"},{"70","70cm"},
            };
            const QString mapped = bandMap.value(rec.band);
            if (!mapped.isEmpty()) rec.band = mapped;
        }
        if (rec.band.isEmpty()) {
            const QString freqStr = recordFields.value("FREQ").trimmed();
            if (!freqStr.isEmpty()) {
                bool ok = false;
                double mhz = freqStr.toDouble(&ok);
                if (ok) rec.band = freqToBand(mhz);
            }
        }

        // Mode
        const QString mode    = recordFields.value("MODE").trimmed();
        const QString submode = recordFields.value("SUBMODE").trimmed();
        rec.modeGroup = normaliseMode(mode, submode);

        records.append(rec);
    }

    return records;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

QVector<QsoRecord> AdifParser::parseFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parse(f.readAll());
}

void AdifParser::parseFileAsync(const QString& path)
{
    // If the file is held open by an external logger (common on Windows),
    // QFile::open() will fail.  Retry a few times before giving up so that
    // a brief write-lock doesn't wipe every spot colour.
    constexpr int kMaxAttempts  = 3;
    constexpr int kRetryDelayMs = 500;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto records = parse(f.readAll());
            // If a CtyDatParser was provided, resolve DXCC prefixes here on the
            // worker thread instead of on the GUI thread.  This avoids
            // multi-hundred-ms stalls for large logs (e.g. 125k QSOs).
            if (m_ctyParser) {
                for (auto& r : records)
                    r.dxccPrefix = m_ctyParser->resolvePrimaryPrefix(r.callsign);
            }
            emit finished(records);
            return;
        }
        if (attempt + 1 < kMaxAttempts)
            QThread::msleep(kRetryDelayMs);
    }

    // All attempts failed — tell the caller so it can keep its existing
    // worked status rather than replacing it with an empty result set.
    emit openFailed(path);
}

} // namespace AetherSDR
