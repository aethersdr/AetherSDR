#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <array>
#include <optional>

namespace AetherSDR::Kxpa100 {
constexpr bool isSupportedFrequency(int khz) { return khz >= 1800 && khz <= 54000; }
enum class Mode { Auto, Manual, Bypass };
enum class CapacitorSide { Transmitter, Antenna };
struct Status {
    QString identity, firmware, serial;
    std::optional<bool> operate, tunerPresent, tuning, bypassed;
    std::optional<Mode> mode;
    std::optional<CapacitorSide> side;
    std::optional<int> antenna, band, capacitorMask, inductorMask, attenuator, frequencyKhz;
    std::optional<double> inputWatts, forwardWatts, reflectedWatts, dissipatedWatts;
    std::optional<double> drainAmps, supplyVolts, temperatureC, swr, bypassSwr;
    std::optional<char> faultCode, attenuatorReason;
    std::optional<double> faultDetail;
    std::optional<int> transceiverInterface;
    std::optional<bool> transceiverPolling;
};
// Bits 0/1 enable ANT1/ANT2. Missing configuration is represented by optional<Configuration>.
using Configuration = std::array<int, 11>;
QString bandName(int band);
int relayTotal(bool capacitor, int mask);
int relayMaskForValue(bool capacitor, int value);
QString currentFaultName(char code);
QString faultUnit(char code);
struct Backup {
    QByteArray internal, external;
    QString firmware, serial;
};
struct FaultRecord {
    int slot = 0;
    char code = 0;
    quint32 elapsedSeconds = 0;
    int frequencyKhz = 0;
    double value = 0;
    QString unit, description;
    QByteArray raw;
};
struct MemoryMatch {
    int antenna = 0;
    bool empty = true, bypassed = false;
    CapacitorSide side = CapacitorSide::Transmitter;
    int capacitorMask = 0, inductorMask = 0;
};
struct MemoryBin {
    int frequencyKhz = 0, band = 0, index = 0, firstKhz = 0, lastKhz = 0;
    QList<MemoryMatch> matches;
};
}
Q_DECLARE_METATYPE(AetherSDR::Kxpa100::Status)
Q_DECLARE_METATYPE(AetherSDR::Kxpa100::Configuration)
Q_DECLARE_METATYPE(AetherSDR::Kxpa100::Backup)
Q_DECLARE_METATYPE(AetherSDR::Kxpa100::MemoryBin)
Q_DECLARE_METATYPE(QList<AetherSDR::Kxpa100::FaultRecord>)
