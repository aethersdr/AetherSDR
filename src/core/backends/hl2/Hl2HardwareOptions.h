#pragma once

#include "core/backends/hl2/MetisProtocol.h"

#include <cstdint>

namespace AetherSDR {

class RadioSettingsScope;

// What hardware is attached to this Hermes-Lite 2. Protocol 1 has no board ID
// or capability word, so a bare HL2, HL2+ (AK4951), SquareSDR 2 and an HL2 with
// an N2ADR board on J16 look identical; the operator declares it. Stored per
// radio (feature document keyed by MAC, like Hl2FreqCal) because the dither
// bit means different things on different boards. Defaults are the bare board,
// except filterBoard (see FilterBoard).
struct Hl2HardwareOptions {
    // Which board is fitted:
    //   None        bare HL2. The EP2 audio slot is the extended address
    //               register and must stay zero.
    //   Ak4951      HL2+ companion board, codec over I2S.
    //   SquareSdr2  SquareSDR 2, codec on the mainboard.
    // Ak4951 and SquareSdr2 behave identically today but stay distinct: the
    // document persists 0/1/2 and the boards differ elsewhere (EP6 mic word
    // rate, docs/HERMES.md).
    // The dither bit 0x00[11] is the operator's on every variant: band-voltage
    // output on a bare HL2 (control.v `band_volts_enabled <= cmd_data[11]`),
    // loudspeaker on AK4951/SquareSDR 2 (i2c_bus2.v `ak4951_spon_next`).
    enum class Codec : int {
        None       = 0,   // bare HL2: no codec, audio slot stays EADDR-safe zero
        Ak4951     = 1,   // HL2+ companion board, codec over I2S
        SquareSdr2 = 2,   // SquareSDR 2, codec on the mainboard
    };
    // Used by clampCodec() and by the tests, which walk 0..kCodecCount-1.
    static constexpr int kCodecCount = 3;

    // Board on J16, which decodes the seven open-collector outputs the gateware
    // forwards to I2C 0x20:
    //   None         nothing attached: release every relay.
    //   N2adrRxTx    N2ADR switching LPF + AM-broadcast HPF on RX and TX.
    //   N2adrTxOnly  LPF in the TX path only; RX sees the bare front end,
    //                optionally through the 3 MHz HPF (n2adrHpf). The SquareSDR
    //                2's arrangement, or an N2ADR between PA and antenna.
    // Default N2adrRxTx: the backend has always driven this pattern, and relay
    // writes are inert with no board listening.
    enum class FilterBoard : int {
        None        = 0,
        N2adrRxTx   = 1,
        N2adrTxOnly = 2,
    };

    Codec codec = Codec::None;

    // Level of the radio's own speaker/headphone jack, 0..100 linear, unity at
    // 100 (same curve as Hl2Backend::setSliceAudioGain). Needed because the app
    // volume and mute are QAudioSink attenuations, not in the samples the codec
    // feed is taken from. Independent of app mute; meaningless without a codec.
    int speakerLevelPercent = 100;

    // The operator's dither-bit intent; read it via ditherBitOnWire(). Off by
    // default (band volts on a bare board); ditherBitOnCodecChange() seeds it.
    bool ditherBit = false;

    // The RANDOM bit, 0x00[12]. Same lineage as dither — an LT2208 control the
    // HL2 does not implement — but nothing has hijacked it, so it is offered
    // for parity with deskHPSDR and for whatever a future gateware does with
    // it. Off, and no known effect on stock hardware.
    bool randomBit = false;

    FilterBoard filterBoard = FilterBoard::N2adrRxTx;

    // The N2ADR board's 3 MHz high-pass on RECEIVE, in N2adrTxOnly only.
    // In N2adrRxTx the HPF rides the per-band pattern and this is ignored; with
    // no board it is meaningless. Off by default because the bare receive path
    // is the honest starting point — an operator near a broadcast transmitter
    // turns it on and hears why.
    bool n2adrHpf = false;

    // A 10 MHz reference (a GPSDO, typically) fed into the CL1 jack, with the
    // VersaClock reprogrammed to lock to it instead of the onboard crystal.
    //
    // THIS IS NOT A REGISTER, it is twenty-four of them: switching CL1 means
    // rewriting the VersaClock 5P49V5923's PLL configuration over I2C-1, and
    // switching back means rewriting all twenty-four again. See
    // versaClockCl1Banks() in MetisProtocol.h. It therefore takes effect on a
    // change rather than being re-asserted, and it does not survive a power
    // cycle of the radio — the HL2 boots on its crystal every time, which is
    // why this is sent on connect as well as on change.
    bool cl1RefClock = false;

    // The HL2 gateware's own ATU tune request, 0x09[20]. Raised only while
    // TUNE is running, and only for an ATU that the GATEWARE drives (the AH-4
    // protocol on the CL2/J16 pins). An ATU hanging off the N2ADR IO board is
    // driven over I2C instead and must NOT have this set — the two would both
    // try to start a tune.
    bool atuGateware = false;

    // ---- pure policy (what the tests pin) ----

    // The dither bit as it goes on the wire: the operator's choice on every
    // variant (see Codec). Identity today; the single named seam for what
    // 0x00[11] carries, also reported by hw.get.
    [[nodiscard]] constexpr bool ditherBitOnWire() const noexcept
    {
        return ditherBit;
    }

    // The dither bit to adopt when the operator declares a different board,
    // since 0x00[11] changes meaning across boards (#5867):
    //   None        false: the bit is band volts on CL2; never via a codec pick.
    //   Ak4951      true: the gateware's init already turned the speaker on
    //               (i2c.v STATE_AK4951S8 writes 0x02 = 0xae) and only writes on
    //               a change, so seeding low would leave the checkbox wrong.
    //   SquareSdr2  current value: the bit is that board's speaker too, and its
    //               power-on state is not established.
    [[nodiscard]] static constexpr bool ditherBitOnCodecChange(Codec next,
                                                               bool current) noexcept
    {
        switch (next) {
        case Codec::None:
            return false;
        case Codec::Ak4951:
            return true;
        case Codec::SquareSdr2:
            return current;
        }
        // No default: above, so a new enumerator is a -Wswitch warning at every
        // build rather than a silent fall-through to somebody's guess.
        return current;
    }

    // True when the host must put real audio in the EP2 audio slot. On a bare
    // HL2 that slot is NOT AUDIO — its first word per frame is the extended
    // address register — so writing to it is a protocol violation, not merely
    // useless. See ep2WriteTxAudio() in MetisProtocol.h.
    [[nodiscard]] constexpr bool hasLocalCodec() const noexcept
    {
        return codec != Codec::None;
    }

    // The speaker level as a multiplier, already clamped. 0.0 when there is no
    // codec, so a caller that forgets to check hasLocalCodec() produces silence
    // rather than sending samples to a radio whose audio slot is EADDR.
    [[nodiscard]] constexpr float speakerGain() const noexcept
    {
        if (!hasLocalCodec())
            return 0.0f;
        return static_cast<float>(clampSpeakerLevel(speakerLevelPercent)) / 100.0f;
    }

    static constexpr int clampSpeakerLevel(int raw) noexcept
    {
        return raw < 0 ? 0 : (raw > 100 ? 100 : raw);
    }

    // The open-collector byte for RECEIVE at this frequency.
    [[nodiscard]] std::uint8_t ocReceiveByteForHz(double hz) const noexcept
    {
        switch (filterBoard) {
        case FilterBoard::None:
            return hl2::kOcNone;
        case FilterBoard::N2adrRxTx:
            return hl2::ocFilterByteForHz(hz);
        case FilterBoard::N2adrTxOnly:
            if (!n2adrHpf)
                return hl2::kOcNone;
            // Masked from the per-band pattern so the HPF exclusions (below
            // 1.6 MHz, and 160 m where the HL2's switching supply couples
            // spurs) live only in ocFilterByteForHz().
            return static_cast<std::uint8_t>(hl2::ocFilterByteForHz(hz)
                                             & hl2::kOcHpfAmBc);
        }
        return hl2::kOcNone;
    }

    // The open-collector byte for TRANSMIT at this frequency.
    // TX gets the full per-band LPF pattern in both N2ADR modes (it keeps
    // harmonics off the air); only FilterBoard::None releases it.
    [[nodiscard]] std::uint8_t ocTransmitByteForHz(double hz) const noexcept
    {
        return filterBoard == FilterBoard::None ? hl2::kOcNone
                                                : hl2::ocFilterByteForHz(hz);
    }

    [[nodiscard]] bool operator==(const Hl2HardwareOptions&) const = default;

    // Per-radio feature document like Hl2FreqCal, read-modify-written whole so
    // a later field is not dropped by an older field's write.
    static constexpr const char* kFeature = "Hardware";
    static constexpr int kSchemaVersion = 1;

    // Defaults for every field the document does not carry — so a document
    // written by an older build gains new fields at their defaults rather than
    // at zero, which for filterBoard would be "no board" and would silently
    // release the relays.
    static Hl2HardwareOptions load(const RadioSettingsScope& scope);
    static void save(const RadioSettingsScope& scope, const Hl2HardwareOptions& opts);

    // Clamp a round-tripped enum from the settings store. The fallbacks differ:
    // unknown codec -> None (bare board), unknown filter board -> N2adrRxTx,
    // because None would release the relays on a radio that has a board.
    static constexpr Codec clampCodec(int raw) noexcept
    {
        switch (raw) {
        case static_cast<int>(Codec::Ak4951):     return Codec::Ak4951;
        case static_cast<int>(Codec::SquareSdr2): return Codec::SquareSdr2;
        default:                                  return Codec::None;
        }
    }
    static constexpr FilterBoard clampFilterBoard(int raw) noexcept
    {
        switch (raw) {
        case static_cast<int>(FilterBoard::None):        return FilterBoard::None;
        case static_cast<int>(FilterBoard::N2adrTxOnly): return FilterBoard::N2adrTxOnly;
        default:                                         return FilterBoard::N2adrRxTx;
        }
    }
};

// Fires until kCodecCount matches the enumerators clampCodec() maps;
// hl2_hardware_options_test walks 0..kCodecCount-1. The dither decision for a
// new enumerator is forced separately by the default-less switch in
// ditherBitOnCodecChange() (-Wswitch, a warning). Out of the class because a
// class-scope static_assert cannot call a member before the class is complete.
static_assert(Hl2HardwareOptions::clampCodec(Hl2HardwareOptions::kCodecCount)
                  == Hl2HardwareOptions::Codec::None,
              "a new Codec enumerator needs a dither-bit decision: add it to "
              "ditherBitOnCodecChange() and clampCodec(), then bump kCodecCount");

}  // namespace AetherSDR
