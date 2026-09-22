#pragma once

#include "core/backends/hl2/MetisProtocol.h"

#include <cstdint>

namespace AetherSDR {

class RadioSettingsScope;

// What hardware is actually bolted to this Hermes-Lite 2.
//
// WHY THIS EXISTS. The HL2 is a BOARD, not a product: the same discovery reply
// comes back from a bare HL2, from an HL2 with the AK4951 companion board
// ("HL2+"), from a SquareSDR 2 — whose codec and low-pass filters are on the
// mainboard — and from an HL2 in a box with an N2ADR filter board bolted to
// J16. Protocol 1 gives the host NO way to tell them apart: there is no board
// ID, no capability word, and the gateware version is the same across all of
// them. The operator is the only party who knows, so these are settings.
//
// WHY PER RADIO AND NOT GLOBAL. An operator with an HL2 on the bench and a
// SquareSDR 2 in the shack must not have one's codec choice silently applied to
// the other — the dither bit means DIFFERENT THINGS on the two (see
// Codec below), so a shared setting would switch the wrong thing on the wrong
// radio. Stored as a radio-scoped feature document, the same mechanism
// Hl2FreqCal uses for the crystal's error, keyed by the radio's MAC.
//
// DEFAULTS ARE THE BARE BOARD, always. Every field's zero value is "a plain
// Hermes-Lite 2 with nothing attached", except the filter board — see
// FilterBoard. A radio that has never been configured therefore behaves exactly
// as it did before this document existed.
struct Hl2HardwareOptions {
    // ---- local audio codec ----
    //
    // THE DITHER BIT IS THE WHOLE REASON THIS IS A THREE-WAY CHOICE and not a
    // checkbox. Protocol 1's config register carries a "dither" bit at
    // 0x00[11], which on genuine openHPSDR hardware turns on the LT2208's
    // dither generator. The HL2 has no LT2208, and three different pieces of
    // hardware hijacked that one bit for three different purposes:
    //
    //   None        the bit drives the HL2's BAND VOLTAGE output (a DC level
    //               per band on the CL2 jack, softerhardware/Hermes-Lite2 wiki
    //               "Band-Volts"). The operator may want it either way.
    //   Ak4951      the HL2+ companion board's gateware reads the bit as
    //               "a codec is present" and it must be held HIGH for the
    //               codec to work at all. The operator does not get a choice.
    //   SquareSdr2  the SquareSDR 2's gateware uses the same bit to switch the
    //               INTERNAL LOUDSPEAKER on and off, so it must stay under the
    //               operator's control and must NOT be forced.
    //
    // Getting this wrong is not subtle: forcing the bit on a SquareSDR 2 nails
    // its speaker on, and clearing it on an HL2+ silently kills the codec.
    // Both were found by deskHPSDR the hard way (its SQUARE SDR 2 patch exists
    // for exactly this), and the three-way enum is what encodes the difference.
    enum class Codec : int {
        None       = 0,   // bare HL2: no codec, audio slot stays EADDR-safe zero
        Ak4951     = 1,   // HL2+ companion board; dither forced high
        SquareSdr2 = 2,   // SquareSDR 2, codec on the mainboard; dither is the speaker
    };

    // ---- companion filter board on J16 ----
    //
    // The HL2 has no switchable filters of its own; it has seven
    // open-collector outputs that the gateware forwards to I2C 0x20. What
    // decodes them is the board on the other end, and there may not be one.
    //
    //   None         nothing attached: release every relay and stop pretending.
    //   N2adrRxTx    the N2ADR board switching BOTH directions — the low-pass
    //                for the band plus the AM-broadcast HPF, on receive as well
    //                as transmit. What a boxed HL2 normally has.
    //   N2adrTxOnly  the low-pass is in the TRANSMIT path only; receive sees
    //                the bare front end, optionally through the 3 MHz HPF
    //                (see n2adrHpf). THIS IS THE SquareSDR 2's arrangement —
    //                its LPF bank is on the mainboard and wired for transmit —
    //                and it is also how an HL2 is wired when the filter board
    //                sits between the PA and the antenna rather than ahead of
    //                the ADC.
    //
    // DEFAULT IS N2adrRxTx, which is the ONE field whose default is not "bare
    // board". That is deliberate and it is not a new decision: this backend has
    // driven the N2ADR pattern unconditionally since it could tune, so
    // defaulting to anything else would silently change the front end of every
    // HL2 already on the air the moment this document appeared. Writing the
    // relays is inert when no board is listening, which is why it was safe to
    // do unconditionally and why it stays the default now.
    enum class FilterBoard : int {
        None        = 0,
        N2adrRxTx   = 1,
        N2adrTxOnly = 2,
    };

    Codec codec = Codec::None;

    // Level of the radio's OWN loudspeaker or headphone jack, 0..100, unity at
    // 100. Meaningless without a codec.
    //
    // WHY THIS EXISTS AT ALL, when the application already has a volume
    // control: that control is not in the samples. AudioEngine applies it as a
    // device attenuation on the host's QAudioSink, so the mixed audio this
    // backend produces has never seen it and no amount of tapping further down
    // would find it. The codec feed is taken from the mix, so without a level
    // of its own the radio's speaker would be stuck at whatever the slice
    // faders happen to sum to.
    //
    // AND IT SHOULD BE SEPARATE ANYWAY. deskHPSDR ties the two together — it
    // scales in WDSP before the buffer that feeds both the sound card and the
    // codec — but that is a consequence of having one knob, not a decision.
    // The speaker in the radio and the speakers on the desk are different
    // transducers in different places, and wanting them at different levels is
    // the ordinary case, not the exotic one.
    //
    // LINEAR, NOT dB, and unity at 100 — the same curve and the same reference
    // point as the per-slice audio gain (Hl2Backend::setSliceAudioGain), which
    // in turn matches what a Flex does with audio_level. One kind of fader in
    // this application, not two.
    //
    // NOT COUPLED TO THE APPLICATION'S MUTE. Mute is the same QAudioSink
    // attenuation as the volume and is equally invisible here; following it
    // would mean this backend reaching into the audio engine, which is a
    // dependency the HL2 backend does not have and should not grow for a
    // fader. Muting the radio's speaker is done with this control.
    int speakerLevelPercent = 100;

    // The operator's dither-bit intent. MEANS NOTHING on its own — read it
    // through ditherBitOnWire() below, which is where the codec's override
    // lives. Held separately from the wire value so that turning the codec
    // setting off again returns the operator's own choice rather than whatever
    // the codec forced.
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

    // The HL2 gateware's own ATU tune request, 0x09[20]. Raised only while
    // TUNE is running, and only for an ATU that the GATEWARE drives (the AH-4
    // protocol on the CL2/J16 pins). An ATU hanging off the N2ADR IO board is
    // driven over I2C instead and must NOT have this set — the two would both
    // try to start a tune.
    bool atuGateware = false;

    // ---- pure policy (what the tests pin) ----

    // The dither bit AS IT MUST GO ON THE WIRE, which is not always what the
    // operator asked for. See Codec: the HL2+ needs it held high whatever the
    // operator thinks, and everything else passes the operator's choice
    // through — including the SquareSDR 2, where the choice IS the speaker.
    [[nodiscard]] constexpr bool ditherBitOnWire() const noexcept
    {
        return codec == Codec::Ak4951 ? true : ditherBit;
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
    //
    // INLINE, like ditherBitOnWire() above and for the same reason: this is
    // pure policy over a table that already lives in a header, it has no
    // dependency on Qt or on the settings store, and keeping it out of the .cpp
    // is what lets it be exercised without standing an application up.
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
            // MASKED OUT OF THE PER-BAND PATTERN rather than decided again
            // here. ocFilterByteForHz() already knows the two frequencies where
            // the HPF must stay out — below 1.6 MHz it would remove what is
            // being listened to, and on 160 m the HL2's own switching supply
            // couples spurs into it — and restating those edges in a second
            // place is how the two drift apart.
            return static_cast<std::uint8_t>(hl2::ocFilterByteForHz(hz)
                                             & hl2::kOcHpfAmBc);
        }
        return hl2::kOcNone;
    }

    // The open-collector byte for TRANSMIT at this frequency.
    //
    // Separate from the receive byte because the two genuinely differ in
    // N2adrTxOnly, and because they differ for a REASON that is not symmetric:
    // the receive filter is a convenience the operator may decline, and the
    // transmit low-pass is what keeps harmonics off the air. Transmit therefore
    // gets the full per-band pattern in both N2ADR modes, and only a declared
    // absence of a board (FilterBoard::None) releases it.
    [[nodiscard]] std::uint8_t ocTransmitByteForHz(double hz) const noexcept
    {
        return filterBoard == FilterBoard::None ? hl2::kOcNone
                                                : hl2::ocFilterByteForHz(hz);
    }

    [[nodiscard]] bool operator==(const Hl2HardwareOptions&) const = default;

    // ---- per-radio persistence ----
    //
    // Same store and same shape as Hl2FreqCal: one feature document per radio,
    // read-modify-written whole so a field added later is not dropped by an
    // older field's write (Principle XIV).
    static constexpr const char* kFeature = "Hardware";
    static constexpr int kSchemaVersion = 1;

    // Defaults for every field the document does not carry — so a document
    // written by an older build gains new fields at their defaults rather than
    // at zero, which for filterBoard would be "no board" and would silently
    // release the relays.
    static Hl2HardwareOptions load(const RadioSettingsScope& scope);
    static void save(const RadioSettingsScope& scope, const Hl2HardwareOptions& opts);

    // Clamp a round-tripped enum. A hand-edited or truncated settings file must
    // not command a codec that does not exist (Principle VII — validate at the
    // boundary rather than trusting the store).
    //
    // NOTE THE TWO DIFFERENT FALLBACKS, which is the whole reason these are not
    // one templated helper: an unrecognised codec falls back to None, the safe
    // "bare board" answer, while an unrecognised filter board falls back to
    // N2adrRxTx — because None would RELEASE the relays on a radio that has a
    // board, and a receiver suddenly hearing the whole HF spectrum through a
    // bypassed front end is not a safe default. See FilterBoard.
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

}  // namespace AetherSDR
