#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

// One transverter or USB cable as RadioModel knows it. The model mints one per
// entity and retires it when the entity is removed or replaced or its session
// ends; a write aimed at it is refused at the final worker-thread write after
// that.
struct SetupStatusLifetime {
    std::atomic_bool active{true};
    bool current() const { return active.load(std::memory_order_acquire); }
    void retire() { active.store(false, std::memory_order_release); }
};

// The radio's `expiration=` value: an ISO-8601 date-time, or a bare date, which
// FlexLib's DateTime.TryParse reads as local midnight. Anything else, trailing
// text included, is invalid.
inline QDateTime parseLicenseExpiration(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (const QDateTime dateTime = QDateTime::fromString(trimmed, Qt::ISODate); dateTime.isValid()) {
        return dateTime;
    }
    const QDate date = QDate::fromString(trimmed, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == trimmed ? date.startOfDay() : QDateTime();
}

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
    // Bytes a passthrough cable's device sent (`data:base64`, decoded). Data,
    // never a setting; textData marks `data:text`, which carries none we read.
    std::optional<QByteArray> passthroughData;
    bool textData{false};
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
    quint64 statusEpoch{0};   // the status session it arrived in
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SetupStatusObservation)
