#pragma once

#include <QObject>

#include <mutex>

namespace AetherSDR {

// Process-wide owner for client-side NR2 configuration. Constitution
// Principle V requires the feature to persist one versioned object rather
// than adding more loose AppSettings keys. UI surfaces edit this model and
// listen for configChanged() so they cannot drift from one another.
class Nr2SettingsModel final : public QObject {
    Q_OBJECT

public:
    struct Config {
        int version{1};
        bool enabled{false};
        int gainMethod{2};
        int npeMethod{0};
        bool aeFilter{true};
        float gainMax{1.0f};
        float gainFloor{0.0f};
        float gainSmooth{0.85f};
        float qspp{0.20f};
        bool legacyGeometryAndGainMapping{false};

        bool operator==(const Config&) const = default;
    };

    static Nr2SettingsModel& instance();
    static Config defaults();

    Config config() const;
    void setConfig(const Config& config);
    void setEnabled(bool enabled);
    void setGainMethod(int method);
    void setNpeMethod(int method);
    void setAeFilter(bool enabled);
    void setGainMax(float value);
    void setGainFloor(float value);
    void setGainSmooth(float value);
    void setQspp(float value);
    void setLegacyGeometryAndGainMapping(bool enabled);

signals:
    void configChanged();

private:
    Nr2SettingsModel();

    static Config normalized(Config config);
    void load();
    void persist(const Config& config) const;

    mutable std::mutex m_mutex;
    Config m_config;
};

} // namespace AetherSDR
