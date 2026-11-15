#pragma once

// Payload builder for the MQTT `aethersdr/radio/state` topic (#5518). A pure
// function over a plain struct so tests/mqtt_radio_state_test.cpp can reach every
// conditional shape (fresh connect, disconnect, no slice, mid-CWX); MainWindow
// keeps the wiring. Subscribers key off FIELD PRESENCE, not sentinels: absent
// `drive` means "not reported", never 0, and `drive` and `max_power_level` come
// and go independently, so watts are never computed from a missing ceiling.

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Everything the payload is derived from, gathered as one object so the builder
// stays pure and every caller is forced to state each input explicitly. The
// defaults describe the most conservative reading: nothing connected, nothing
// reported, nothing transmitting.
struct MqttRadioStateInputs {
    // Radio-level. Truthful independently of drive: `tx` is the radio's own
    // transmit state and is never inferred from a power setting.
    bool connected = false;
    bool transmitting = false;

    // Slice-level. Present only when the client has an active slice; a radio can
    // be up with no slice open yet, and the power fields below must still
    // publish in that window (the Mission requirement on #5518).
    bool haveSlice = false;
    QString sliceLetter;
    double sliceFrequencyMhz = 0.0;
    QString sliceMode;

    // Power, radio-level. The two fields gate INDEPENDENTLY (#5733 review): only
    // FlexBackend populates TransmitDelta::maxPowerLevel, so on an Icom or an HL2
    // the drive latch says nothing about whether the ceiling has been reported,
    // and one gate published a compiled-in 100 W default as a firmware answer.
    bool haveTransmitStatus = false;   // gates `drive` and `drive_confirmed`
    int drive = 0;                     // raw 0..100 RF-power setting, NOT watts
    // max_power_level is the best ceiling the CLIENT has:
    //   Flex - radio status `max_power_level=`, and the 500 W Aurora/PGXL ceiling
    //          from slice `max_internal_pa_power` (#484).
    //   Icom - per-model rated output (IcomModels.cpp, via txPowerBands).
    //   HL2  - kHl2RatedOutputWatts, via txPowerBands.
    // Presence means "a ceiling we stand behind"; watts = drive/100 * this in every
    // case. No separate _confirmed flag: the value is trustworthy on every family.
    bool haveMaxPowerLevel = false;    // gates `max_power_level` on its own
    int maxPowerLevel = 0;             // watts = drive/100 * this

    // Whether `drive` in THIS message is confirmed radio state: the backend
    // reads drive back (RadioCapabilities::transmitDriveControl authority Radio)
    // AND the current value arrived from the radio rather than from a local set
    // (TransmitModel::rfPowerIsFromRadio). Defaults false — the conservative
    // reading, and the one a safety consumer should get when nobody has said
    // otherwise.
    bool driveIsReadback = false;
};

// Build the `aethersdr/radio/state` JSON payload.
QJsonObject buildMqttRadioStatePayload(const MqttRadioStateInputs& in);

}  // namespace AetherSDR
