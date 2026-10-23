#pragma once

#include <QSet>

namespace AetherSDR {

// Capability metadata stays usable by Qt Core-only model and UI consumers.
enum class RadioSetupField {
    Callsign, ClientStation, PrivateIpPolicy, NetworkMtu,
    Dhcp, StaticNetwork, AccTxDelayMs, TxDelayMs, Tx1DelayMs,
    Tx2DelayMs, Tx3DelayMs, TimeoutMs, MaximumPowerPercent,
    ShowTxInWaterfall, MeterDuringReceive, SyncCwx, RttyMarkDefaultHz,
    CalibrationFrequencyMhz, FrequencyCorrectionPpb, ReferenceSource,
    MuteLocalWhenRemote, BinauralReceive, SharpnessLevel, SharpnessAuto,
    LowLatencyDigital, StartFrequencyCalibration, LineoutGain, LineoutMute,
    HeadphoneGain, HeadphoneMute, FrontSpeakerMute, Count
};

struct RadioSetupControl {
    QSet<RadioSetupField> fields;
    bool supports(RadioSetupField field) const { return fields.contains(field); }
};

} // namespace AetherSDR
