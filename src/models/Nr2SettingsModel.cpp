#include "models/Nr2SettingsModel.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

Q_LOGGING_CATEGORY(lcNr2Settings, "aether.nr2.settings")

constexpr int kConfigVersion = 1;
const QString kRootKey = QStringLiteral("NR2");

const QString kLegacyEnabled = QStringLiteral("ClientNr2Enabled");
const QString kLegacyGainMethod = QStringLiteral("NR2GainMethod");
const QString kLegacyNpeMethod = QStringLiteral("NR2NpeMethod");
const QString kLegacyAeFilter = QStringLiteral("NR2AeFilter");
const QString kLegacyGainMax = QStringLiteral("NR2GainMax");
const QString kLegacyGainFloor = QStringLiteral("NR2GainFloor");
const QString kLegacyGainSmooth = QStringLiteral("NR2GainSmooth");
const QString kLegacyQspp = QStringLiteral("NR2Qspp");
const QString kLegacyGeometry = QStringLiteral("NR2UseOriginalGeometry");

const std::array<QString, 9> kLegacyKeys = {
    kLegacyEnabled,
    kLegacyGainMethod,
    kLegacyNpeMethod,
    kLegacyAeFilter,
    kLegacyGainMax,
    kLegacyGainFloor,
    kLegacyGainSmooth,
    kLegacyQspp,
    kLegacyGeometry,
};

bool settingIsTrue(const QVariant& value)
{
    return value.toString().compare(QLatin1String("True"),
                                    Qt::CaseInsensitive) == 0
        || value.toBool();
}

QJsonObject toJson(const Nr2SettingsModel::Config& config)
{
    return QJsonObject{
        {QStringLiteral("version"), config.version},
        {QStringLiteral("enabled"), config.enabled},
        {QStringLiteral("gainMethod"), config.gainMethod},
        {QStringLiteral("npeMethod"), config.npeMethod},
        {QStringLiteral("aeFilter"), config.aeFilter},
        {QStringLiteral("gainMax"), config.gainMax},
        {QStringLiteral("gainFloor"), config.gainFloor},
        {QStringLiteral("gainSmooth"), config.gainSmooth},
        {QStringLiteral("qspp"), config.qspp},
        {QStringLiteral("legacyGeometryAndGainMapping"),
         config.legacyGeometryAndGainMapping},
    };
}

Nr2SettingsModel::Config fromJson(const QJsonObject& object)
{
    Nr2SettingsModel::Config config = Nr2SettingsModel::defaults();
    config.version = object.value(QStringLiteral("version"))
                         .toInt(kConfigVersion);
    config.enabled = object.value(QStringLiteral("enabled"))
                         .toBool(config.enabled);
    config.gainMethod = object.value(QStringLiteral("gainMethod"))
                            .toInt(config.gainMethod);
    config.npeMethod = object.value(QStringLiteral("npeMethod"))
                           .toInt(config.npeMethod);
    config.aeFilter = object.value(QStringLiteral("aeFilter"))
                          .toBool(config.aeFilter);
    config.gainMax = static_cast<float>(
        object.value(QStringLiteral("gainMax")).toDouble(config.gainMax));
    config.gainFloor = static_cast<float>(
        object.value(QStringLiteral("gainFloor")).toDouble(config.gainFloor));
    config.gainSmooth = static_cast<float>(
        object.value(QStringLiteral("gainSmooth")).toDouble(config.gainSmooth));
    config.qspp = static_cast<float>(
        object.value(QStringLiteral("qspp")).toDouble(config.qspp));
    config.legacyGeometryAndGainMapping =
        object.value(QStringLiteral("legacyGeometryAndGainMapping"))
            .toBool(config.legacyGeometryAndGainMapping);
    return config;
}

} // namespace

Nr2SettingsModel& Nr2SettingsModel::instance()
{
    static Nr2SettingsModel model;
    return model;
}

Nr2SettingsModel::Config Nr2SettingsModel::defaults()
{
    return Config{};
}

Nr2SettingsModel::Nr2SettingsModel()
{
    load();
}

Nr2SettingsModel::Config Nr2SettingsModel::normalized(Config config)
{
    const Config fallback = defaults();
    config.version = kConfigVersion;
    config.gainMethod = std::clamp(config.gainMethod, 0, 3);
    config.npeMethod = std::clamp(config.npeMethod, 0, 2);
    config.gainMax = std::isfinite(config.gainMax)
        ? std::clamp(config.gainMax, 0.0f, 2.0f)
        : fallback.gainMax;
    config.gainFloor = std::isfinite(config.gainFloor)
        ? std::clamp(config.gainFloor, 0.0f, 1.0f)
        : fallback.gainFloor;
    config.gainSmooth = std::isfinite(config.gainSmooth)
        ? std::clamp(config.gainSmooth, 0.0f, 0.9999f)
        : fallback.gainSmooth;
    config.qspp = std::isfinite(config.qspp)
        ? std::clamp(config.qspp, 1.0e-4f, 1.0f - 1.0e-4f)
        : fallback.qspp;
    return config;
}

Nr2SettingsModel::Config Nr2SettingsModel::config() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void Nr2SettingsModel::load()
{
    AppSettings& settings = AppSettings::instance();
    const QString raw = settings.value(kRootKey, QString{}).toString();
    if (!raw.isEmpty()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(
            raw.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            const Config loaded = normalized(fromJson(document.object()));
            const QString normalizedJson = QString::fromUtf8(
                QJsonDocument(toJson(loaded)).toJson(QJsonDocument::Compact));
            bool changed = normalizedJson != raw;
            for (const QString& key : kLegacyKeys) {
                if (settings.contains(key)) {
                    settings.remove(key);
                    changed = true;
                }
            }
            if (changed) {
                settings.setValue(kRootKey, normalizedJson);
                settings.save();
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = loaded;
            return;
        } else {
            qCWarning(lcNr2Settings)
                << "Ignoring malformed NR2 settings object:" << error.errorString();
            settings.remove(kRootKey);
            settings.save();
        }
    }

    const bool hasLegacy = std::any_of(
        kLegacyKeys.cbegin(), kLegacyKeys.cend(),
        [&settings](const QString& key) { return settings.contains(key); });
    if (!hasLegacy) {
        return;
    }

    Config migrated = defaults();
    migrated.enabled = settingIsTrue(
        settings.value(kLegacyEnabled, QStringLiteral("False")));
    migrated.gainMethod = settings.value(kLegacyGainMethod, 2).toInt();
    migrated.npeMethod = settings.value(kLegacyNpeMethod, 0).toInt();
    migrated.aeFilter = settingIsTrue(
        settings.value(kLegacyAeFilter, QStringLiteral("True")));
    migrated.gainMax = settings.value(kLegacyGainMax, 1.0f).toFloat();
    migrated.gainFloor = settings.value(kLegacyGainFloor, 0.0f).toFloat();
    migrated.gainSmooth = settings.value(kLegacyGainSmooth, 0.85f).toFloat();
    migrated.qspp = settings.value(kLegacyQspp, 0.20f).toFloat();
    migrated.legacyGeometryAndGainMapping = settingIsTrue(
        settings.value(kLegacyGeometry, QStringLiteral("False")));
    migrated = normalized(migrated);

    settings.setValue(
        kRootKey,
        QString::fromUtf8(QJsonDocument(toJson(migrated))
                              .toJson(QJsonDocument::Compact)));
    for (const QString& key : kLegacyKeys) {
        settings.remove(key);
    }
    settings.save();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = migrated;
}

void Nr2SettingsModel::persist(const Config& config) const
{
    AppSettings& settings = AppSettings::instance();
    settings.setValue(
        kRootKey,
        QString::fromUtf8(QJsonDocument(toJson(config))
                              .toJson(QJsonDocument::Compact)));
    settings.save();
}

void Nr2SettingsModel::setConfig(const Config& requested)
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

void Nr2SettingsModel::setEnabled(bool enabled)
{
    Config next = config();
    next.enabled = enabled;
    setConfig(next);
}

void Nr2SettingsModel::setGainMethod(int method)
{
    Config next = config();
    next.gainMethod = method;
    setConfig(next);
}

void Nr2SettingsModel::setNpeMethod(int method)
{
    Config next = config();
    next.npeMethod = method;
    setConfig(next);
}

void Nr2SettingsModel::setAeFilter(bool enabled)
{
    Config next = config();
    next.aeFilter = enabled;
    setConfig(next);
}

void Nr2SettingsModel::setGainMax(float value)
{
    Config next = config();
    next.gainMax = value;
    setConfig(next);
}

void Nr2SettingsModel::setGainFloor(float value)
{
    Config next = config();
    next.gainFloor = value;
    setConfig(next);
}

void Nr2SettingsModel::setGainSmooth(float value)
{
    Config next = config();
    next.gainSmooth = value;
    setConfig(next);
}

void Nr2SettingsModel::setQspp(float value)
{
    Config next = config();
    next.qspp = value;
    setConfig(next);
}

void Nr2SettingsModel::setLegacyGeometryAndGainMapping(bool enabled)
{
    Config next = config();
    next.legacyGeometryAndGainMapping = enabled;
    setConfig(next);
}

} // namespace AetherSDR
