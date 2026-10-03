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

    // Power is radio-level, so it survives the slice gate, but is omitted until the
    // radio has sent transmit status (TransmitModel's default 100 is
    // indistinguishable from real full drive) and while disconnected: the disconnect
    // publish runs before TransmitModel::resetState(), so a dead radio's drive must
    // never go out with drive_confirmed:true.
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
