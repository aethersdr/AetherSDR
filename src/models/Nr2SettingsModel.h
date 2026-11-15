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
    static constexpr int kConfigVersion = 2;

    struct Config {
        int version{kConfigVersion};
        bool enabled{false};
        int gainMethod{2};
        int npeMethod{0};
        bool aeFilter{true};
        float gainMax{1.0f};
        float gainFloor{0.0f};
        float gainSmooth{0.85f};
        float qspp{0.20f};

        // WDSP's psychoacoustic post-processing (emnr.c post2), off by
        // default exactly as upstream ships it — the Guide notes the default
        // is zero so the stage does nothing where a console has not
        // implemented its controls, which was AetherSDR until now (#5702).
        bool post2Run{false};
        float post2Factor{0.15f};
        float post2Nlevel{0.15f};
        // A FREQUENCY, not WDSP's bin fraction: 0.12 means 2871 Hz at WDSP's
        // geometry and 1435 Hz at this one. See SpectralNR.
        float post2TaperHz{2871.0f};
        float post2DecaySeconds{5.0f};

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
    // WDSP's post2 psychoacoustic stage (#5702).
    void setPost2Run(bool enabled);
    void setPost2Factor(float value);
    void setPost2Nlevel(float value);
    void setPost2TaperHz(float hz);
    void setPost2DecaySeconds(float seconds);

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
