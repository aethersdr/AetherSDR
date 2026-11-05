#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

// Shared cancellation evidence may be checked at the final worker-thread write.
struct SetupStatusLifetime {
    std::atomic_bool active{true};
    bool current() const { return active.load(std::memory_order_acquire); }
    void retire() { active.store(false, std::memory_order_release); }
};

struct TransverterStatusValues {
    int id{-1};
    bool removed{false};
    std::optional<int> order;
    std::optional<QString> name;
    std::optional<double> rfFreq, ifFreq, loError, rxGain, maxPower;
    std::optional<bool> rxOnly, isValid;
};

struct UsbCableStatusValues {
    QString serial;
    bool removed{false};
    int bit{-1};
    std::optional<QString> type, name, source, sourceRxAnt, sourceTxAnt, sourceSlice;
    std::optional<QString> parity, flowControl, band, output;
    std::optional<bool> enabled, present, loggingEnabled, autoReport, activeHigh, preamp, pttDependent;
    std::optional<int> speed, dataBits, stopBits, pttDelayMs, txDelayMs;
    std::optional<double> lowFreqMhz, highFreqMhz;
};

struct FilterSharpnessStatusValues {
    enum class Mode { Voice, Cw, Digital };
    Mode mode{Mode::Voice};
    std::optional<int> level;
    std::optional<bool> automatic;
};

struct LicenseStatusValues {
    enum class Kind { Base, Subscription, Feature };
    Kind kind{Kind::Base};
    std::optional<QString> radioId, maximumVersion;
    QString name;
    QDateTime expiration;
    bool enabled{false};
    QString reason;
};

struct SetupStatusObservation {
    enum class Kind { Transverter, UsbCable, FilterSharpness, License };
    Kind kind{Kind::Transverter};
    TransverterStatusValues transverter;
    UsbCableStatusValues usbCable;
    FilterSharpnessStatusValues sharpness;
    LicenseStatusValues license;
    std::shared_ptr<SetupStatusLifetime> lifetime;
    std::shared_ptr<SetupStatusLifetime> entityLifetime;
    bool current() const { return lifetime && lifetime->current(); }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SetupStatusObservation)
