#include "models/Rn2SettingsModel.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

Q_LOGGING_CATEGORY(lcRn2Settings, "aether.rn2.settings")

constexpr int kConfigVersion = 1;
const QString kRootKey = QStringLiteral("RN2");

// RN2 shipped with no adjustable parameters, so there are no legacy flat keys
// to claim here — unlike NR2, this document starts clean.

QJsonObject toJson(const Rn2SettingsModel::Config& config)
{
    return QJsonObject{
        {QStringLiteral("version"), config.version},
        {QStringLiteral("rxDryMix"), config.rxDryMix},
    };
}

Rn2SettingsModel::Config fromJson(const QJsonObject& object)
{
    Rn2SettingsModel::Config config = Rn2SettingsModel::defaults();
    config.version = object.value(QStringLiteral("version"))
                         .toInt(kConfigVersion);
    config.rxDryMix = static_cast<float>(
        object.value(QStringLiteral("rxDryMix")).toDouble(config.rxDryMix));
    return config;
}

} // namespace

Rn2SettingsModel& Rn2SettingsModel::instance()
{
    static Rn2SettingsModel model;
    return model;
}

Rn2SettingsModel::Config Rn2SettingsModel::defaults()
{
    return Config{};
}

Rn2SettingsModel::Rn2SettingsModel()
{
    load();
}

Rn2SettingsModel::Config Rn2SettingsModel::normalized(Config config)
{
    const Config fallback = defaults();
    config.version = kConfigVersion;
    config.rxDryMix = std::isfinite(config.rxDryMix)
        ? std::clamp(config.rxDryMix, 0.0f, kMaxRxDryMix)
        : fallback.rxDryMix;
    return config;
}

Rn2SettingsModel::Config Rn2SettingsModel::config() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void Rn2SettingsModel::load()
{
    AppSettings& settings = AppSettings::instance();
    const QString raw = settings.value(kRootKey, QString{}).toString();
    if (raw.isEmpty()) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(lcRn2Settings)
            << "Ignoring malformed RN2 settings object:" << error.errorString();
        settings.remove(kRootKey);
        settings.save();
        return;
    }

    const Config loaded = normalized(fromJson(document.object()));
    const QString normalizedJson = QString::fromUtf8(
        QJsonDocument(toJson(loaded)).toJson(QJsonDocument::Compact));
    if (normalizedJson != raw) {
        settings.setValue(kRootKey, normalizedJson);
        settings.save();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = loaded;
}

void Rn2SettingsModel::persist(const Config& config) const
{
    AppSettings& settings = AppSettings::instance();
    settings.setValue(
        kRootKey,
        QString::fromUtf8(QJsonDocument(toJson(config))
                              .toJson(QJsonDocument::Compact)));
    settings.save();
}

void Rn2SettingsModel::setConfig(const Config& requested)
{
    const Config next = normalized(requested);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_config == next) {
            return;
        }
        m_config = next;
    }
    persist(next);
    emit configChanged();
}

void Rn2SettingsModel::setRxDryMix(float value)
{
    Config next = config();
    next.rxDryMix = value;
    setConfig(next);
}

} // namespace AetherSDR
