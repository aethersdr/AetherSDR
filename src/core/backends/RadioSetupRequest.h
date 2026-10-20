#pragma once

#include "RadioSetupControl.h"
#include "core/backends/RadioControlTypes.h"
#include "core/backends/SetupTextPolicy.h"
#include "core/backends/UsbCableRequest.h"
#include "core/NetworkMtuPolicy.h"

#include <QHostAddress>
#include <QAbstractSocket>
#include <QMetaType>
#include <QString>
#include <cmath>
#include <functional>
#include <variant>

namespace AetherSDR {

// Desktop configuration intents, not additions to the daemon's core profile.
// A field describes an operation, not a promise that every family supports it.
enum class ReferenceSource { Auto, External, Gpsdo, Tcxo };
enum class SharpnessMode { Voice, Cw, Digital };
struct SharpnessLevel { SharpnessMode mode; int level; };
struct SharpnessAuto { SharpnessMode mode; bool enabled; };
struct StaticNetwork {
    QHostAddress address;
    QHostAddress gateway;
    QHostAddress netmask;
    bool valid() const
    {
        if (address.protocol() != QAbstractSocket::IPv4Protocol
            || gateway.protocol() != QAbstractSocket::IPv4Protocol
            || netmask.protocol() != QAbstractSocket::IPv4Protocol) { return false; }
        const quint32 mask = netmask.toIPv4Address();
        const quint32 host = ~mask;
        return mask != 0 && (host & (host + 1)) == 0;
    }
};

struct RadioSetupRequest {
    using Field = RadioSetupField;
    Field field;
    std::variant<std::monostate, bool, int, double, QString,
                 AetherSDR::StaticNetwork, AetherSDR::ReferenceSource,
                 AetherSDR::SharpnessLevel, AetherSDR::SharpnessAuto> value;

    bool valid() const
    {
        switch (field) {
        case Field::Callsign: {
            const QString* text = std::get_if<QString>(&value);
            if (!text || !setupTextValid(*text)) { return false; }
            // A station identity needs an ASCII letter or digit, not only
            // symbols that a radio display may discard entirely.
            for (const QChar ch : *text) {
                if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
                    || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                    || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
                    return true;
                }
            }
            return false;
        }
        case Field::ClientStation: {
            const QString* text = std::get_if<QString>(&value);
            return text && setupTextValid(*text);
        }
        case Field::PrivateIpPolicy: case Field::ShowTxInWaterfall:
        case Field::MeterDuringReceive: case Field::SyncCwx:
        case Field::MuteLocalWhenRemote: case Field::BinauralReceive:
        case Field::LowLatencyDigital: case Field::LineoutMute:
        case Field::HeadphoneMute: case Field::FrontSpeakerMute:
            return std::holds_alternative<bool>(value);
        case Field::NetworkMtu: {
            const int* mtu = std::get_if<int>(&value);
            return mtu && *mtu >= kMinNetworkMtu && *mtu <= kMaxNetworkMtu;
        }
        case Field::Dhcp: return std::holds_alternative<std::monostate>(value);
        case Field::StaticNetwork: {
            const AetherSDR::StaticNetwork* network = std::get_if<AetherSDR::StaticNetwork>(&value);
            return network && network->valid();
        }
        case Field::AccTxDelayMs: case Field::TxDelayMs: case Field::Tx1DelayMs:
        case Field::Tx2DelayMs: case Field::Tx3DelayMs: case Field::TimeoutMs: {
            const int* milliseconds = std::get_if<int>(&value);
            return milliseconds && *milliseconds >= 0;
        }
        case Field::MaximumPowerPercent: case Field::LineoutGain: case Field::HeadphoneGain: {
            const int* percent = std::get_if<int>(&value);
            return percent && *percent >= 0 && *percent <= 100;
        }
        case Field::RttyMarkDefaultHz: case Field::FrequencyCorrectionPpb:
            return std::holds_alternative<int>(value);
        case Field::CalibrationFrequencyMhz: case Field::StartFrequencyCalibration: {
            const double* mhz = std::get_if<double>(&value);
            return mhz && std::isfinite(*mhz) && *mhz > 0;
        }
        case Field::ReferenceSource: {
            const AetherSDR::ReferenceSource* source = std::get_if<AetherSDR::ReferenceSource>(&value);
            return source && *source >= AetherSDR::ReferenceSource::Auto
                && *source <= AetherSDR::ReferenceSource::Tcxo;
        }
        case Field::SharpnessLevel: {
            const AetherSDR::SharpnessLevel* setting = std::get_if<AetherSDR::SharpnessLevel>(&value);
            return setting && setting->mode >= SharpnessMode::Voice
                && setting->mode <= SharpnessMode::Digital
                && setting->level >= 0 && setting->level <= 3;
        }
        case Field::SharpnessAuto: {
            const AetherSDR::SharpnessAuto* setting = std::get_if<AetherSDR::SharpnessAuto>(&value);
            return setting && setting->mode >= SharpnessMode::Voice && setting->mode <= SharpnessMode::Digital;
        }
        case Field::Count: return false;
        }
        return false;
    }
};

struct TxBandRequest {
    enum class Field {
        RfPowerPercent, TunePowerPercent, Inhibit, AccessoryTx,
        RcaTxRequest, AccessoryTxRequest, Tx1, Tx2, Tx3, HardwareAlc, Count
    };
    int bandId;
    Field field;
    std::variant<int, bool> value;
    bool valid() const
    {
        if (bandId < 0 || field < Field::RfPowerPercent || field >= Field::Count) { return false; }
        if (field == Field::RfPowerPercent || field == Field::TunePowerPercent) {
            const int* percent = std::get_if<int>(&value);
            return percent && *percent >= 0 && *percent <= 100;
        }
        return std::holds_alternative<bool>(value);
    }
};

struct TransverterRequest {
    enum class Field { Create, Remove, Name, RfFrequencyMhz, IfFrequencyMhz,
                       LoErrorMhz, RxGainDb, MaximumPowerDbm, RxOnly, Count };
    int id;
    Field field;
    std::variant<std::monostate, bool, double, QString> value;
    // The editor's IF setpoint accompanies a power edit, including the immediate
    // clamp after an IF edit. The radio's old status may not have echoed yet.
    double powerIfMhz{0.0};
    bool valid() const
    {
        if (field == Field::Create) {
            return id == -1 && std::holds_alternative<std::monostate>(value);
        }
        if (id < 0) { return false; }
        switch (field) {
        case Field::Remove: return std::holds_alternative<std::monostate>(value);
        case Field::Name: {
            const QString* text = std::get_if<QString>(&value);
            return text && setupTextValid(*text);
        }
        case Field::RxOnly: return std::holds_alternative<bool>(value);
        case Field::RfFrequencyMhz: case Field::IfFrequencyMhz:
        case Field::LoErrorMhz: case Field::RxGainDb: case Field::MaximumPowerDbm: {
            const double* number = std::get_if<double>(&value);
            return number && std::isfinite(*number)
                && (field != Field::MaximumPowerDbm || (std::isfinite(powerIfMhz) && powerIfMhz >= 0))
                && ((field != Field::RfFrequencyMhz && field != Field::IfFrequencyMhz) || *number >= 0);
        }
        case Field::Create: case Field::Count: return false;
        }
        return false;
    }
};

using RadioSetupReply = RadioControlReply;
using RadioSetupCompletion = RadioControlCompletion;

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::RadioSetupRequest)
Q_DECLARE_METATYPE(AetherSDR::TxBandRequest)
Q_DECLARE_METATYPE(AetherSDR::TransverterRequest)
