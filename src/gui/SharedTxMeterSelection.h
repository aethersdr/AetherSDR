#pragma once

namespace AetherSDR {

enum class SharedTxMeterSource { Radio, Amplifier, RadioKeyBlocked };

struct SharedTxMeterSelection {
    SharedTxMeterSource source{SharedTxMeterSource::Radio};
    int maxWatts{100};
    bool highPowerAmplifier{false};
};

// The KAT500 key-line override applies to the KPA500 path. A separately
// selected radio-integrated amplifier retains its existing meter priority.
inline SharedTxMeterSelection selectSharedTxMeters(int radioMaxWatts, bool amplifierActive,
                                                   bool kpa500Active, bool keyBlockConfirmed)
{
    if (amplifierActive) {
        return {SharedTxMeterSource::Amplifier, radioMaxWatts, true};
    }
    if (kpa500Active) {
        if (keyBlockConfirmed) {
            return {SharedTxMeterSource::RadioKeyBlocked, radioMaxWatts, false};
        }
        return {SharedTxMeterSource::Amplifier, 500, false};
    }
    return {SharedTxMeterSource::Radio, radioMaxWatts, false};
}

} // namespace AetherSDR
