#pragma once

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>

namespace AetherSDR {

class DStarWaveformSettings
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

private:
    static constexpr const char* kRootKey = "DStarWaveform";

    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value(kRootKey, QString{}).toString();
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

    static void write(const QJsonObject& obj)
    {
        AppSettings::instance().setValue(kRootKey,
            QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        AppSettings::instance().save();
    }
};

} // namespace AetherSDR
