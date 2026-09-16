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
    if (in.haveTransmitStatus) {
        obj[QStringLiteral("drive")]           = in.drive;
        obj[QStringLiteral("max_power_level")] = in.maxPowerLevel;
        // Whether `drive` is what the RADIO reports or what the OPERATOR asked
        // for. Published alongside the value, not as separate knowledge a
        // subscriber has to look up per radio family.
        obj[QStringLiteral("drive_confirmed")] = in.driveIsReadback;
    }

    return obj;
}

}  // namespace AetherSDR
