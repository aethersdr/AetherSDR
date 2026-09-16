#pragma once

// Payload builder for the MQTT `aethersdr/radio/state` topic (#5518).
//
// Lives here, apart from MainWindow, for one reason: the payload's shape is
// conditional — slice fields appear only with a slice up, power fields only once
// the radio has actually reported power — and four of those conditions are
// ordering/timing states (fresh connect, disconnect, no slice, mid-CWX) that a
// test cannot reach through MainWindow. A pure function over a plain input
// struct can be driven into every one of them directly, which is what
// tests/mqtt_radio_state_test.cpp does. MainWindow keeps the wiring; this keeps
// the contract.
//
// Subscribers key off FIELD PRESENCE, not sentinel values: an absent `drive` is
// "this radio has not told us its drive", never "drive is 0". That distinction is
// the whole point for the amplifier-interlock consumer the topic was asked for —
// a sentinel reads as a number and arms on it.

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

    // Power, radio-level. `haveTransmitStatus` gates both fields: false means the
    // model still holds its class default and there is nothing honest to publish.
    bool haveTransmitStatus = false;
    int drive = 0;                 // raw 0..100 RF-power setting, NOT watts
    int maxPowerLevel = 0;         // firmware max_power_level; watts = drive/100*this
    bool driveIsReadback = true;   // RadioCapabilities::driveIsReadback
};

// Build the `aethersdr/radio/state` JSON payload.
QJsonObject buildMqttRadioStatePayload(const MqttRadioStateInputs& in);

}  // namespace AetherSDR
