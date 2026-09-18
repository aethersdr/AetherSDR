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
// a sentinel reads as a number and arms on it. `drive` and `max_power_level`
// appear and disappear independently: a radio can report its drive without ever
// reporting a ceiling, and computing watts from a missing ceiling is exactly the
// arithmetic this presence rule exists to refuse.

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
    // WHAT max_power_level MEANS, precisely (#5733 review): the best ceiling the
    // CLIENT has, which is not always a firmware answer.
    //   Flex — radio status `max_power_level=`, and the 500 W Aurora/PGXL ceiling
    //          from slice status `max_internal_pa_power` (#484). Firmware.
    //   Icom — the per-model rated output in IcomModels.cpp, via txPowerBands.
    //   HL2  — kHl2RatedOutputWatts, compiled in, via txPowerBands.
    // So presence means "we have a ceiling we stand behind", NOT "the radio
    // reported one". `watts = drive/100 * max_power_level` is the right
    // arithmetic in every case; only the provenance differs, and a per-model
    // rating is real data rather than the compiled-in 100 default this latch
    // exists to keep off the wire. There is deliberately no
    // `max_power_level_confirmed`: the value is trustworthy on every family, so a
    // second flag would suggest a doubt that does not exist.
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
