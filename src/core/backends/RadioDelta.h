#pragma once

#include <optional>

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Normalized, vendor-neutral radio-global status delta (aetherd RFC 2.3 —
// RadioModel residual). Same typed, compiler-checked, present-only contract as
// the sub-model deltas: FlexBackend::decodeRadioStatus populates only the fields
// the wire reported, and RadioModel::applyRadioChanges applies exactly those,
// keeping the model-side orchestration (slice-capacity bounding, propagating
// rtty_mark_default to slices, the TNF/DAX-IQ sub-models, the infoChanged /
// audioOutputChanged / callsign / autoSave emits).
//
// The universal fields (model, callsign, nickname, region, GPS/audio-out) map to
// any radio; the Flex-specific ones (MultiFlex, radio_options, freq calibration,
// rtty default) are simply absent for a backend that has no analog.
struct RadioDelta {
    // Identity / capability
    std::optional<QString> model;
    std::optional<int>     slicesAvailable;   // "slices=N" free-slot count
    std::optional<QString> callsign;
    std::optional<QString> nickname;
    std::optional<QString> region;
    std::optional<QString> radioOptions;
    std::optional<QString> bandsRaw;          // optional "bands=" declaration (validated model-side; see RadioModel::declaredBands())

    // Global flags
    std::optional<bool>    remoteOnEnabled;
    std::optional<bool>    multiFlexEnabled;      // mf_enable
    std::optional<bool>    enforcePrivateIp;      // enforce_private_ip_connections
    std::optional<bool>    binauralRx;
    std::optional<bool>    fullDuplex;            // full_duplex_enabled
    std::optional<bool>    muteLocalWhenRemote;   // mute_local_audio_when_remote
    std::optional<bool>    autoSave;
    std::optional<bool>    lowLatencyDigital;     // low_latency_digital_modes
    std::optional<bool>    tnfEnabled;

    // Calibration / defaults
    std::optional<int>     freqErrorPpb;
    std::optional<double>  calFreqMhz;            // cal_freq
    std::optional<int>     rttyMarkDefault;

    // Audio outputs
    std::optional<int>     lineoutGain;
    std::optional<bool>    lineoutMute;
    std::optional<int>     headphoneGain;
    std::optional<bool>    headphoneMute;
    std::optional<bool>    frontSpeakerMute;

    // DAX-IQ capacity
    std::optional<int>     daxiqCapacity;
    std::optional<int>     daxiqAvailable;
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::RadioDelta)
