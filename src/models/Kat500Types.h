#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <optional>
#include <tuple>

namespace AetherSDR::Kat500 {
// KAT500 Owner's Manual: continuous operating coverage from 1.8 through 54 MHz.
constexpr bool isSupportedFrequency(int khz) { return khz >= 1800 && khz <= 54000; }
enum class Mode { Auto, Manual, Bypass };
enum class CapacitorSide { Transmitter, Antenna };
struct Status {
    QString identity;
    QString firmware;
    QString serial;
    std::optional<bool> powerOn, tuning, bypassed, ampKeyInterrupted, attenuator;
    std::optional<Mode> mode;
    std::optional<CapacitorSide> side;
    std::optional<int> antenna, band, capacitorMask, inductorMask, faultCode;
    std::optional<int> frequencyKhz, countedFrequencyKhz, catFrequencyKhz;
    std::optional<int> forwardAdc, reflectedAdc;
    std::optional<double> swr, bypassSwr;
    QString frequencyBin;
};

enum class Setting {
    AmpKeyPower, PowerOnDc, SleepIdle, EnabledAntenna, PreferredAntenna,
    SlotsPerBin, AutoFineTune, AutoSwr, BypassSwr, KeySwr, RetuneDistance
};
struct SettingKey {
    Setting setting;
    int band = -1;
    int antenna = 0;
    bool operator<(const SettingKey& other) const {
        return std::tie(setting, band, antenna) < std::tie(other.setting, other.band, other.antenna);
    }
    bool operator==(const SettingKey&) const = default;
};
// Missing keys are unknown, never factory defaults. A patch contains only edits.
using Configuration = QMap<SettingKey, double>;
QList<SettingKey> configurationKeys();
QString settingName(Setting setting);
QString bandName(int band);
int relayTotal(bool capacitor, int mask);
int relayMaskForValue(bool capacitor, int value);
QString currentFaultName(int code);

struct Backup {
    QByteArray internal;
    QByteArray external;
    QString firmware;
    QString serial;
};
struct FaultRecord {
    int slot = 0;
    int code = 0;
    quint32 elapsedSeconds = 0;
    int frequencyOrCount = 0;
    int forwardAdc = 0;
    int reflectedAdc = 0;
    double swr = 0;
    double bypassSwr = 0;
    QString description;
    QByteArray raw;
};
struct MemoryMatch {
    int antenna = 0;
    bool bypassed = false;
    std::optional<CapacitorSide> side;
    std::optional<int> capacitorMask, inductorMask;
    double bypassSwr = 0;
};
struct MemoryBin {
    int firstKhz = 0;
    int lastKhz = 0;
    QList<MemoryMatch> matches;
    int unused = 0;
};
} // namespace AetherSDR::Kat500
Q_DECLARE_METATYPE(AetherSDR::Kat500::Status)
Q_DECLARE_METATYPE(AetherSDR::Kat500::Configuration)
Q_DECLARE_METATYPE(AetherSDR::Kat500::Backup)
Q_DECLARE_METATYPE(AetherSDR::Kat500::MemoryBin)
Q_DECLARE_METATYPE(QList<AetherSDR::Kat500::FaultRecord>)
