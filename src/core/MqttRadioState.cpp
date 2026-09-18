#include "core/MqttRadioState.h"

namespace AetherSDR {

QJsonObject buildMqttRadioStatePayload(const MqttRadioStateInputs& in)
{
    QJsonObject obj;

    // Slice fields first, and only when there is a slice. Omitted rather than
    // nulled: a subscriber that reads `obj["slice"]` on a slice-less radio gets
    // an undefined value it must handle either way, and an explicit null adds a
    // second "absent" spelling for no benefit.
    if (in.haveSlice) {
        obj[QStringLiteral("slice")] = in.sliceLetter;
        obj[QStringLiteral("freq")]  = in.sliceFrequencyMhz;
        obj[QStringLiteral("mode")]  = in.sliceMode;
    }

    obj[QStringLiteral("tx")]        = in.transmitting;
    obj[QStringLiteral("connected")] = in.connected;

    // Power is RADIO-level, so it survives the slice gate above — but not the
    // "has the radio actually said anything" gate. Before the first transmit
    // status of a session, TransmitModel still holds its 100 default, which is
    // byte-identical to a radio genuinely running full drive; publishing it would
    // hand an interlock a phantom. Omission makes a consumer that requires the
    // field fail loudly instead.
    // AND `connected`, for the same reason the slice fields are gated: the
    // disconnect publish runs from radioTransmittingChanged(false), which
    // RadioModel::onDisconnected() emits TEN LINES BEFORE it calls
    // TransmitModel::resetState() — so the latches are still set and the dead
    // radio's drive would go out carrying drive_confirmed:true, which is the one
    // message an amplifier interlock must never see (#5733 review).
    if (in.connected && in.haveTransmitStatus) {
        obj[QStringLiteral("drive")] = in.drive;
        // Whether `drive` IN THIS MESSAGE is what the RADIO reports or what the
        // OPERATOR asked for. Published alongside the value, not as separate
        // knowledge a subscriber has to look up per radio family — and per
        // value, so a local drive change reads as unconfirmed until the radio
        // echoes it rather than being vouched for on the backend's reputation.
        obj[QStringLiteral("drive_confirmed")] = in.driveIsReadback;
    }

    // Gated apart from `drive` on purpose: only Flex reports a ceiling in
    // transmit status, so tying this to the drive latch published the model's
    // compiled-in 100 as firmware truth on every other family (#5733 review).
    if (in.connected && in.haveMaxPowerLevel) {
        obj[QStringLiteral("max_power_level")] = in.maxPowerLevel;
    }

    return obj;
}

}  // namespace AetherSDR
