#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFileInfo>
#include <QString>

namespace AetherSDR {

class DigitalVoiceWaveformSettings
{
public:
    enum class Backend {
        ThumbDv
    };

    static Backend backend()
    {
        return backendFromString(readObj().value(QStringLiteral("Backend")).toString());
    }

    static void setBackend(Backend backend)
    {
        QJsonObject obj = readObj();
        obj[QStringLiteral("Backend")] = backendId(backend);
        write(obj);
    }

    static QString backendId(Backend backend)
    {
        switch (backend) {
        case Backend::ThumbDv:
            return QStringLiteral("ThumbDV");
        }
        return QStringLiteral("ThumbDV");
    }

    static QString backendArgument(Backend backend)
    {
        switch (backend) {
        case Backend::ThumbDv:
            return QStringLiteral("thumbdv");
        }
        return QStringLiteral("thumbdv");
    }

    static QString backendLabel(Backend backend)
    {
        switch (backend) {
        case Backend::ThumbDv:
            return QStringLiteral("ThumbDV / DV3000");
        }
        return QStringLiteral("ThumbDV / DV3000");
    }

    static Backend backendFromString(const QString& value)
    {
        Q_UNUSED(value);
        return Backend::ThumbDv;
    }

    static bool backendRequiresSerial(Backend backend)
    {
        Q_UNUSED(backend);
        return true;
    }

    static bool autoStart()
    {
        return readObj().value(QStringLiteral("AutoStart")).toBool(false);
    }

    static void setAutoStart(bool on)
    {
        QJsonObject obj = readObj();
        obj[QStringLiteral("AutoStart")] = on;
        write(obj);
    }

    static QString executablePath()
    {
        return readObj().value(QStringLiteral("ExecutablePath")).toString().trimmed();
    }

    static void setExecutablePath(const QString& path)
    {
        QJsonObject obj = readObj();
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            obj.remove(QStringLiteral("ExecutablePath"));
        } else {
            obj[QStringLiteral("ExecutablePath")] = trimmed;
        }
        write(obj);
    }

    static QString serialPort()
    {
        return readObj().value(QStringLiteral("SerialPort")).toString().trimmed();
    }

    static void setSerialPort(const QString& port)
    {
        QJsonObject obj = readObj();
        const QString trimmed = port.trimmed();
        if (trimmed.isEmpty()) {
            obj.remove(QStringLiteral("SerialPort"));
        } else {
            obj[QStringLiteral("SerialPort")] = trimmed;
        }
        write(obj);
    }

    static QString myCall()
    {
        return normalizeUpperTrimmed(
            readDStarObj().value(QStringLiteral("MyCall")).toString());
    }

    static QString myCallSuffix()
    {
        return normalizeUpperTrimmed(
            readDStarObj().value(QStringLiteral("MyCallSuffix")).toString());
    }

    static QString urCall()
    {
        const QString value = normalizeRoutingField(
            readDStarObj().value(QStringLiteral("UrCall")).toString());
        return value.isEmpty() ? QStringLiteral("CQCQCQ") : value;
    }

    static QString rpt1()
    {
        const QString value = normalizeRoutingField(
            readDStarObj().value(QStringLiteral("Rpt1")).toString());
        return value.isEmpty() ? QStringLiteral("DIRECT") : value;
    }

    static QString rpt2()
    {
        const QString value = normalizeRoutingField(
            readDStarObj().value(QStringLiteral("Rpt2")).toString());
        return value.isEmpty() ? QStringLiteral("DIRECT") : value;
    }

    static QString message()
    {
        return readDStarObj().value(QStringLiteral("Message")).toString();
    }

    static void setRouting(const QString& myCall,
                           const QString& myCallSuffix,
                           const QString& urCall,
                           const QString& rpt1,
                           const QString& rpt2,
                           const QString& message)
    {
        QJsonObject obj = readObj();
        QJsonObject dStar = obj.value(QStringLiteral("DStar")).toObject();
        setOrRemove(dStar, QStringLiteral("MyCall"), normalizeUpperTrimmed(myCall));
        setOrRemove(dStar, QStringLiteral("MyCallSuffix"),
                    normalizeUpperTrimmed(myCallSuffix));
        setOrRemove(dStar, QStringLiteral("UrCall"), normalizeUpperTrimmed(urCall));
        setOrRemove(dStar, QStringLiteral("Rpt1"), normalizeUpperTrimmed(rpt1));
        setOrRemove(dStar, QStringLiteral("Rpt2"), normalizeUpperTrimmed(rpt2));
        setOrRemove(dStar, QStringLiteral("Message"), message);
        obj[QStringLiteral("DStar")] = dStar;
        write(obj);
    }

    static void setConfiguration(bool autoStart,
                                 const QString& executablePath,
                                 const QString& serialPort,
                                 const QString& myCall,
                                 const QString& myCallSuffix,
                                 const QString& urCall,
                                 const QString& rpt1,
                                 const QString& rpt2,
                                 const QString& message)
    {
        QJsonObject obj = readObj();
        obj[QStringLiteral("Backend")] = backendId(Backend::ThumbDv);
        obj[QStringLiteral("AutoStart")] = autoStart;
        setOrRemove(obj, QStringLiteral("ExecutablePath"), executablePath.trimmed());
        setOrRemove(obj, QStringLiteral("SerialPort"), serialPort.trimmed());

        QJsonObject dStar = obj.value(QStringLiteral("DStar")).toObject();
        setOrRemove(dStar, QStringLiteral("MyCall"), normalizeUpperTrimmed(myCall));
        setOrRemove(dStar, QStringLiteral("MyCallSuffix"),
                    normalizeUpperTrimmed(myCallSuffix));
        setOrRemove(dStar, QStringLiteral("UrCall"), normalizeUpperTrimmed(urCall));
        setOrRemove(dStar, QStringLiteral("Rpt1"), normalizeUpperTrimmed(rpt1));
        setOrRemove(dStar, QStringLiteral("Rpt2"), normalizeUpperTrimmed(rpt2));
        setOrRemove(dStar, QStringLiteral("Message"), message);
        obj[QStringLiteral("DStar")] = dStar;
        write(obj);
    }

    // One-time schema migration. This is deliberately not a runtime fallback:
    // the legacy key is removed after its values have been copied.
    static bool migrateLegacySettings()
    {
        AppSettings& settings = AppSettings::instance();
        if (!settings.contains(kLegacyRootKey)) {
            return false;
        }

        if (!settings.contains(kRootKey)) {
            const QJsonObject legacy = parseObject(
                settings.value(kLegacyRootKey, QString{}).toString());
            const QJsonObject migrated = migrateLegacyObject(legacy);
            settings.setValue(kRootKey,
                QString::fromUtf8(QJsonDocument(migrated).toJson(QJsonDocument::Compact)));
        }
        settings.remove(kLegacyRootKey);
        settings.save();
        return true;
    }

    static QJsonObject migrateLegacyObject(const QJsonObject& legacy)
    {
        QJsonObject migrated;
        copyIfPresent(legacy, migrated, QStringLiteral("Backend"));
        copyIfPresent(legacy, migrated, QStringLiteral("AutoStart"));
        copyIfPresent(legacy, migrated, QStringLiteral("SerialPort"));

        const QString executable =
            legacy.value(QStringLiteral("ExecutablePath")).toString().trimmed();
        if (!executable.isEmpty() && !isLegacyBundledExecutable(executable)) {
            migrated[QStringLiteral("ExecutablePath")] = executable;
        }

        QJsonObject dStar;
        for (const QString& key : {QStringLiteral("MyCall"),
                                   QStringLiteral("MyCallSuffix"),
                                   QStringLiteral("UrCall"),
                                   QStringLiteral("Rpt1"),
                                   QStringLiteral("Rpt2"),
                                   QStringLiteral("Message")}) {
            copyIfPresent(legacy, dStar, key);
        }
        if (!dStar.isEmpty()) {
            migrated[QStringLiteral("DStar")] = dStar;
        }
        return migrated;
    }

    static QString normalizeCallsign(const QString& value, int maximumLength)
    {
        return value.trimmed().toUpper().left(maximumLength);
    }

    static QString normalizeRoutingField(const QString& value)
    {
        return normalizeUpperTrimmed(value);
    }

    static bool isValidMyCall(const QString& value)
    {
        const QString normalized = normalizeUpperTrimmed(value);
        if (normalized.size() < 3 || normalized.size() > 8) {
            return false;
        }

        bool hasLetter = false;
        bool hasDigit = false;
        for (const QChar ch : normalized) {
            if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) {
                hasLetter = true;
            } else if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) {
                hasDigit = true;
            } else {
                return false;
            }
        }
        return hasLetter && hasDigit;
    }

    static bool isValidSuffix(const QString& value)
    {
        const QString normalized = normalizeUpperTrimmed(value);
        if (normalized.size() > 4) {
            return false;
        }
        for (const QChar ch : normalized) {
            if (!((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                  || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')))) {
                return false;
            }
        }
        return true;
    }

    static bool isValidRoutingField(const QString& value)
    {
        const QString normalized = normalizeUpperTrimmed(value);
        if (normalized.isEmpty() || normalized.size() > 8) {
            return false;
        }
        for (const QChar ch : normalized) {
            if (!((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                  || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                  || ch == QLatin1Char(' '))) {
                return false;
            }
        }
        return true;
    }

    static bool isValidUrCall(const QString& value)
    {
        const QString normalized = normalizeUpperTrimmed(value);
        if (isValidRoutingField(normalized)) {
            return true;
        }
        return normalized.startsWith(QLatin1Char('/'))
            && normalized.size() > 1
            && normalized.size() <= 8
            && isValidRoutingField(normalized.mid(1));
    }

    static bool isValidMessage(const QString& value)
    {
        if (value.size() > 20) {
            return false;
        }
        for (const QChar ch : value) {
            if (ch.unicode() < 0x20 || ch.unicode() > 0x7e
                    || ch == QLatin1Char('|')) {
                return false;
            }
        }
        return true;
    }

    static QString validationError(const QString& fallbackMyCall = {})
    {
        const QString effectiveMyCall = myCall().isEmpty()
            ? normalizeUpperTrimmed(fallbackMyCall)
            : myCall();
        if (!isValidMyCall(effectiveMyCall)) {
            return QStringLiteral("Configure a valid D-STAR MYCALL (3-8 letters and digits)");
        }
        if (!isValidSuffix(myCallSuffix())) {
            return QStringLiteral("D-STAR MYCALL suffix must contain at most 4 letters or digits");
        }
        if (!isValidUrCall(urCall())) {
            return QStringLiteral(
                "D-STAR URCALL must contain 1-8 letters, digits, or spaces, with an optional leading slash");
        }
        if (!isValidRoutingField(rpt1()) || !isValidRoutingField(rpt2())) {
            return QStringLiteral("D-STAR repeater fields must contain 1-8 letters, digits, or spaces");
        }
        if (!isValidMessage(message())) {
            return QStringLiteral("D-STAR message must contain at most 20 printable ASCII characters");
        }
        return {};
    }

    static QString effectiveMyCall(const QString& fallbackMyCall = {})
    {
        const QString configured = myCall();
        return configured.isEmpty() ? normalizeUpperTrimmed(fallbackMyCall) : configured;
    }

private:
    static constexpr const char* kRootKey = "DigitalVoiceWaveform";
    static constexpr const char* kLegacyRootKey = "DStarWaveform";

    static QJsonObject readObj()
    {
        migrateLegacySettings();
        const QString json =
            AppSettings::instance().value(kRootKey, QString{}).toString();
        return parseObject(json);
    }

    static QJsonObject readDStarObj()
    {
        return readObj().value(QStringLiteral("DStar")).toObject();
    }

    static void write(const QJsonObject& obj)
    {
        AppSettings::instance().setValue(kRootKey,
            QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        AppSettings::instance().save();
    }

    static void setOrRemove(QJsonObject& obj, const QString& key, const QString& value)
    {
        if (value.isEmpty()) {
            obj.remove(key);
        } else {
            obj[key] = value;
        }
    }

    static QString normalizeUpperTrimmed(const QString& value)
    {
        return value.trimmed().toUpper();
    }

    static QJsonObject parseObject(const QString& json)
    {
        if (json.isEmpty()) {
            return {};
        }

        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            return {};
        }
        return doc.object();
    }

    static void copyIfPresent(const QJsonObject& source,
                              QJsonObject& destination,
                              const QString& key)
    {
        if (source.contains(key)) {
            destination[key] = source.value(key);
        }
    }

    static bool isLegacyBundledExecutable(const QString& path)
    {
        const QString fileName = QFileInfo(path).fileName();
        return fileName.compare(QStringLiteral("aether-dstar-waveform"),
                                Qt::CaseInsensitive) == 0
            || fileName.compare(QStringLiteral("aether-dstar-waveform.exe"),
                                Qt::CaseInsensitive) == 0;
    }
};

} // namespace AetherSDR
