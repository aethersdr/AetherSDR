#pragma once

#include <QObject>

#include <mutex>

namespace AetherSDR {

// Process-wide owner for client-side RN2 configuration. Constitution
// Principle V requires the feature to persist one versioned object rather
// than adding more loose AppSettings keys. UI surfaces edit this model and
// listen for configChanged() so they cannot drift from one another.
class Rn2SettingsModel final : public QObject {
    Q_OBJECT

public:
    struct Config {
        int version{1};
        // Fraction of the original spectrum RNNoise keeps in each RX frame.
        // 0.0 is full suppression — RN2's behavior since it shipped, and the
        // default, so enabling the control is opt-in. Raising it leaves a
        // constant noise floor under the denoised audio, which some operators
        // prefer to RNNoise's near-silent gaps between phrases.
        float rxDryMix{0.0f};

        bool operator==(const Config&) const = default;
    };

    // Above this the denoiser stops being a denoiser; the UI slider shares
    // the bound so a stored value and a slider position cannot disagree.
    static constexpr float kMaxRxDryMix = 0.50f;

    static Rn2SettingsModel& instance();
    static Config defaults();

    Config config() const;
    void setConfig(const Config& config);
    void setRxDryMix(float value);

signals:
    void configChanged();

private:
    Rn2SettingsModel();

    static Config normalized(Config config);
    void load();
    void persist(const Config& config) const;

    mutable std::mutex m_mutex;
    Config m_config;
};

} // namespace AetherSDR
