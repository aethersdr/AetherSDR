#include "core/backends/icom/IcomControls.h"

#include <algorithm>
#include <array>

namespace AetherSDR::icom {

int speechProcessorRawLevel(int maximum, int level) noexcept
{
    const int bounded = std::clamp(level, 0, maximum);
    if (maximum > 2) {
        return (bounded * 255 + 99) / 100;
    }
    static constexpr std::array<int, 3> kProcLevels{3, 6, 9};
    return kProcLevels[static_cast<std::size_t>(
        std::clamp(bounded, 0, 2))] * 255 / 10;
}

namespace {

// EVERY CI-V MESSAGE THIS BACKEND NAMES, wired or not.
//
// The bar for inclusion is that a constant exists in CivCodec.h. A row whose
// `wiring` is Declared is a constant with no call sites — the radio has the
// feature, we have written down its address, and nothing reaches it. Those rows
// are the point of the table; deleting them would restore exactly the blindness
// it exists to remove.
//
// Ranges are from the IC-705 CI-V Reference Guide (2020). Where a row's real
// behaviour is more complicated than four integers, `note` says so rather than
// the numbers quietly lying.
constexpr std::array kSpecs = {
    // ---- Tuning and mode ------------------------------------------------
    ControlSpec{"freq", 0x05, 0, false, "Operating frequency",
                Plane::Slice, Encoding::BcdFreq, Wiring::Both,
                0, 0, "Hz", 30000, 470000000,
                "setSliceFrequency", "vfoFreqLabel", true,
                "Read with 0x03; the radio also reports it unsolicited as 0x00 "
                "when CI-V Transceive is on."},
    ControlSpec{"mode", 0x06, 0, false, "Operating mode + filter slot",
                Plane::Slice, Encoding::ModeFilter, Wiring::Both,
                0, 3, "enum", 0, 0,
                "setSliceMode", "vfoModeCombo", true,
                "ORDINARY mode only — DATA on/off is NOT in this frame, which is why "
                "writes go out as 0x26 (the data.mode row) on every model that has "
                "it and fall back to 0x06 only on an unrecognised radio. The radio "
                "still READS and REPORTS here (0x04, and 0x01 unsolicited). The "
                "SECOND payload byte is the filter slot (1..3) and carries the "
                "passband: an IC-705 cannot report a passband in Hz, so the slot is "
                "the only source. SAM/DRM/DSB have no equivalent and are refused."},
    ControlSpec{"data.mode", 0x26, 0x00, true, "Mode + DATA on/off + filter slot",
                Plane::Slice, Encoding::Enum, Wiring::Both,
                0, 3, "enum", 0, 0,
                "setSliceMode", "vfoModeCombo", true,
                "WHAT MAKES DIGU DIFFERENT FROM USB. Commands 01/04/06 carry only "
                "the mode byte, and USB and USB-D share it — 26 is the only command "
                "that tells them apart, in either direction. It states mode, DATA "
                "and filter slot for the selected VFO in ONE frame, so a mode "
                "change cannot clear DATA and a filter change cannot leave DATA "
                "behind. Written on every mode and filter change, confirmed by a "
                "read, and adopted from the radio at connect and after every "
                "front-panel mode change. Unselected VFO (26 01) is split, which "
                "this backend does not yet model.", IcomFeature::VfoMode},
    ControlSpec{"filter", 0x06, 0, false, "IF filter slot",
                Plane::Slice, Encoding::ModeFilter, Wiring::Both,
                1, 3, "slot", 1, 3,
                "setSliceFilter", "vfoFilterBtn", true,
                "THE SLOT, NOT THE WIDTH — see if.width for the difference. Three "
                "slots whose FACTORY widths change with the mode: SSB 3.0/2.4/1.8k, "
                "CW 1.2k/500/250, RTTY 2.4k/500/250, AM+SAM 9/6/3k, FM 15/10/7k, "
                "WFM one fixed filter. Published per mode as rxFilterWidthsHz, and "
                "those labels are DEFAULTS: an operator who redefined a slot gets a "
                "button whose label is stale, which is why the drawn passband comes "
                "from 1A 03 instead. A seam request that matches a published width "
                "exactly is taken as a slot pick; anything else is a width change."},
    ControlSpec{"if.width", 0x1A, 0x03, true, "IF filter width (actual)",
                Plane::Slice, Encoding::BcdByte, Wiring::Both,
                0, 49, "Hz", 50, 10000,
                "setSliceFilter", "spectrumPassbandEdge", true,
                "THE HZ THE SELECTED SLOT IS ACTUALLY DEFINED AS, which the slot "
                "number cannot tell you. Mode-dependent code table, identical on "
                "IC-705 and IC-7300MK2: SSB/CW 00-09 = 50-500 Hz in 50 Hz and "
                "10-40 = 600-3600 Hz in 100 Hz; RTTY the same but capped at code 31 "
                "(2.7 kHz); AM 00-49 = 200 Hz-10 kHz in 200 Hz. NOTE THE GAP — 550 Hz "
                "does not exist. FM/DV/WFM have NO settable width and this command "
                "does not apply there. Re-read after every mode, DATA and slot change, "
                "because the radio stores a separate width for each combination and "
                "announces none of them. A write REDEFINES the selected slot, which "
                "is exactly what the radio's own FILTER knob does."},
    ControlSpec{"pbt.inner", 0x14, 0x07, true, "Twin PBT inner",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "Hz", -3600, 3600,
                "setSliceFilter", "spectrumPassbandEdge", true,
                "A SIGNED POSITION about 0128, not a magnitude — do NOT put it "
                "through the 0-255-to-percent conversion every other level uses, or "
                "the centre quantises away and the passband walks one step per "
                "round trip. Hz per step SCALES WITH THE WIDTH in circuit: full "
                "deflection is one whole filter width, so the same code is 3.6 kHz in "
                "wide SSB and 250 Hz in narrow CW. Written together with pbt.outer to "
                "SLIDE the passband; the pair moving apart is what narrows it from "
                "the inside, and is the only way an Icom produces an asymmetric "
                "response."},
    ControlSpec{"pbt.outer", 0x14, 0x08, true, "Twin PBT outer",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "Hz", -3600, 3600,
                "setSliceFilter", "spectrumPassbandEdge", true,
                "The other end of the pair — see pbt.inner. setSliceFilter writes "
                "both to the SAME code, because 1A 03 has already set the width and "
                "separating them would subtract from a window that is already the "
                "right size."},
    ControlSpec{"repeater.shift", 0x0F, 0, false, "Repeater shift direction",
                Plane::Slice, Encoding::Enum, Wiring::Both,
                0x10, 0x12, "enum", 0, 2,
                "setSliceRepeaterOffsetDir", "vfoFmDuplexContainer", true,
                "10 simplex, 11 DUP-, 12 DUP+. Polled because CI-V Transceive "
                "does not reliably announce every front-panel change.",
                IcomFeature::FmRepeaterBasic},
    ControlSpec{"repeater.offset", 0x0D, 0, false, "Repeater offset",
                Plane::Slice, Encoding::Bcd6, Wiring::Both,
                0, 999999, "Hz", 0, 99999900,
                "setSliceFmRepeaterOffset", "vfoFmDuplexContainer", true,
                "Read with 0x0C and written with 0x0D. Three little-endian BCD "
                "bytes in 100 Hz units.", IcomFeature::FmRepeaterBasic},
    ControlSpec{"repeater.tone.frequency", 0x1B, 0x00, true,
                "Repeater CTCSS frequency",
                Plane::Slice, Encoding::Bcd6, Wiring::Both,
                0, 2999, "Hz", 0, 299,
                "setSliceFmToneValue", "vfoFmToneContainer", true,
                "Three big-endian BCD bytes in tenths of a hertz. This is the "
                "tone parameter; 16 42 is the independent enable.",
                IcomFeature::FmRepeaterBasic},
    ControlSpec{"repeater.tone.rx", 0x1B, 0x01, true,
                "Receive CTCSS frequency",
                Plane::Slice, Encoding::Bcd6, Wiring::DecodeOnly,
                0, 2999, "Hz", 0, 299,
                "", "", true,
                "IC-9700 extended readback only. Three BCD bytes in tenths of "
                "a hertz; the reserved polarity byte must be zero.",
                IcomFeature::FmRepeaterExtendedReadback},
    ControlSpec{"repeater.dtcs", 0x1B, 0x02, true,
                "DTCS code and polarity",
                Plane::Slice, Encoding::Dtcs, Wiring::Both,
                0, 999, "code", 0, 999,
                "setSliceFmDtcs", "vfoFmToneContainer", true,
                "IC-9700 extended control. Payload bit 4 is TX reverse "
                "and bit 0 is RX reverse; all other polarity bits are rejected.",
                IcomFeature::FmRepeaterExtendedReadback},
    ControlSpec{"repeater.access.ctcss", 0x16, 0x5D, true,
                "FM repeater access mode", Plane::Slice, Encoding::Enum, Wiring::Both,
                0, 9, "enum", 0, 7, "setSliceFmToneMode", "vfoFmToneContainer", true,
                "IC-9700 exposes the complete documented CTCSS, DTCS, and mixed "
                "access vocabulary through the capability-gated FM tone UI.",
                IcomFeature::FmRepeaterCtcssRx},
    ControlSpec{"repeater.tone.rx", 0x1B, 0x01, true,
                "Receive CTCSS frequency", Plane::Slice, Encoding::Bcd6, Wiring::Both,
                0, 2999, "Hz", 0, 299, "setSliceFmToneRxValue", "vfoFmToneContainer", true,
                "Three big-endian BCD bytes in tenths of a hertz. The wire encoding "
                "spans 000.0-299.9; IC-9700 writes accept only the canonical CTCSS list.",
                IcomFeature::FmRepeaterCtcssRx},

    // ---- Levels (0x14) --------------------------------------------------
    ControlSpec{"af.gain", 0x14, 0x01, true, "AF gain",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSliceAudioGain", "sliceAudioGainSlider", true,
                "Was decode-only for the whole first bring-up: read and published, "
                "with no setSliceAudioGain override, so the slider moved and the "
                "radio's volume did not."},
    ControlSpec{"rf.gain", 0x14, 0x02, true, "RF gain",
                Plane::Pan, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setPanRfGain", "panRfGainSlider", true,
                "PERCENT, not dB — the register has no published decibel mapping. "
                "This slider used to drive the preamp (16 02) and label its three "
                "positions '0/1/2 dB'."},
    ControlSpec{"squelch", 0x14, 0x03, true, "Squelch threshold",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSliceSquelch", "sliceSquelchSlider", true,
                "NO separate enable exists on this radio — the threshold IS the "
                "control, and squelch is off at zero."},
    ControlSpec{"nr.level", 0x14, 0x06, true, "Noise reduction level",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSliceNoiseReduction", "dspNRBtn", true,
                "Only pushed while NR is enabled: the register survives the "
                "function being switched off."},
    ControlSpec{"cw.pitch", 0x14, 0x09, true, "CW pitch",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "Hz", 300, 900,
                "setCwPitch", "cwPitchSlider", true,
                "Shared by the Icom text keyer and the existing CW sidebar."},
    ControlSpec{"tx.power", 0x14, 0x0A, true, "RF power",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setTxPower", "txPowerSlider", true, ""},
    ControlSpec{"mic.gain", 0x14, 0x0B, true, "Mic gain",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setMicGain", "phoneMicSlider", true,
                "MODEL-CONDITIONAL: normally physical MIC gain 14 0B; on the "
                "IC-9700 while LAN is the active MOD input, the same normalized "
                "Phone control reads and writes model-owned SET 0114."},
    ControlSpec{"cw.speed", 0x14, 0x0C, true, "Keyer speed",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "wpm", 6, 48,
                "setCwSpeed", "cwSpeedSlider", true,
                "Shared by the Icom text keyer and the existing CW sidebar."},
    ControlSpec{"notch.pos", 0x14, 0x0D, true, "Manual notch position",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSliceManualNotch", "dspMNBtn", true,
                "A POSITION across the passband, not a depth. Pushed even while "
                "the notch is off, so the marker and the notch agree on enable."},
    ControlSpec{"comp.level", 0x14, 0x0E, true, "Speech compressor level",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSpeechProcessor", "txProcBtn", true,
                "Two registers make one operator control: 16 44 is the enable, "
                "this is how hard."},
    ControlSpec{"nb.level", 0x14, 0x12, true, "Noise blanker level",
                Plane::Slice, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setSliceNoiseBlanker", "dspNBBtn", true, ""},
    ControlSpec{"monitor.level", 0x14, 0x15, true, "Monitor level",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setTxMonitor", "phoneMonitorSlider", true,
                "14 15 carries the level and 16 45 carries enable; both are read "
                "from the radio and operator intent crosses one neutral seam verb."},

    // ---- Functions (0x16) ------------------------------------------------
    ControlSpec{"preamp", 0x16, 0x02, true, "Preamp",
                Plane::Pan, Encoding::Enum, Wiring::Both,
                0, 2, "step", 0, 2,
                "setPanPreamp", "panPreampBtn", true,
                "00 OFF, 01 P.AMP1, 02 P.AMP2 on HF; only 00/01 above 50 MHz. No "
                "published gain figures, so the positions are NAMED, never given "
                "decibels. A set is answered with a bare FB and never echoed."},
    ControlSpec{"agc", 0x16, 0x12, true, "AGC time constant",
                Plane::Slice, Encoding::Enum, Wiring::Both,
                1, 3, "step", 1, 3,
                "setSliceAgc", "sliceAgcCombo", true,
                "01 FAST, 02 MID, 03 SLOW. The seam also carries an AGC threshold "
                "and this radio has none — that half is a documented no-op."},
    ControlSpec{"nb", 0x16, 0x22, true, "Noise blanker",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceNoiseBlanker", "dspNBBtn", true, ""},
    ControlSpec{"nr", 0x16, 0x40, true, "Noise reduction",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceNoiseReduction", "dspNRBtn", true, ""},
    ControlSpec{"anf", 0x16, 0x41, true, "Auto notch",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceAutoNotch", "dspANFBtn", true,
                "A REAL Icom feature, not a Flex one — it finds its own tone, "
                "unlike the manual notch."},
    ControlSpec{"repeater.tone", 0x16, 0x42, true, "Repeater tone enable",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceFmToneMode", "vfoFmToneContainer", true,
                "CTCSS transmit tone only. Written last during memory recall "
                "because an IC-705 frequency change can clear the enable.",
                IcomFeature::FmRepeaterBasic},
    ControlSpec{"repeater.access", 0x16, 0x5D, true,
                "FM repeater access selector",
                Plane::Slice, Encoding::Enum, Wiring::DecodeOnly,
                0, 9, "mode", 0, 9,
                "", "", true,
                "IC-9700 extended readback only. Values 00/01/02/03/06/07/08/09 "
                "map to the active model profile's normalized access modes.",
                IcomFeature::FmRepeaterExtendedReadback},
    ControlSpec{"comp", 0x16, 0x44, true, "Speech compressor",
                Plane::Transmit, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSpeechProcessor", "txProcBtn", true, ""},
    ControlSpec{"monitor", 0x16, 0x45, true, "TX monitor",
                Plane::Transmit, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setTxMonitor", "txMonitorBtn", true,
                "The reply used to fall through the 0x16 switch's default and be "
                "dropped, so the button opened at OUR default on a radio that may "
                "have had the monitor on."},
    ControlSpec{"vox", 0x16, 0x46, true, "VOX",
                Plane::Transmit, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setVox", "phoneVoxBtn", true,
                "Enable only. The trigger threshold is a separate level (14 16) "
                "and the DELAY is a SET-menu item (1A 05 0359, in 0.1 s steps) — "
                "not 14 17, which is the ANTI-vox gain."},
    ControlSpec{"vox.gain", 0x14, 0x16, true, "VOX gain",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setVox", "phoneVoxSlider", true,
                "The trigger threshold. An operator slider change is pushed even "
                "while VOX is off because the register defines the next enable."},
    ControlSpec{"break.in", 0x16, 0x47, true, "Break-in",
                Plane::Transmit, Encoding::Enum, Wiring::Both,
                0, 2, "step", 0, 2,
                "setCwBreakIn", "cwBreakInBtn", true,
                "The existing boolean control selects OFF or semi break-in. "
                "Full break-in needs a future three-state UI."},
    ControlSpec{"notch", 0x16, 0x48, true, "Manual notch",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceManualNotch", "dspMNBtn", true, ""},
    ControlSpec{"dial.lock", 0x16, 0x50, true, "Dial lock",
                Plane::Radio, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setRadioDialLock", "sliceLockButtons", true,
                "IC-705, IC-7300MK2, and IC-9700 profile-gated; radio-global "
                "readback is mirrored to every slice lock surface.",
                IcomFeature::DialLock},
    ControlSpec{"notch.width", 0x16, 0x57, true, "Manual notch width",
                Plane::Slice, Encoding::Enum, Wiring::Declared,
                0, 2, "step", 0, 2,
                "", "", false,
                "STUB: 00 wide, 01 mid, 02 narrow. No seam verb carries a notch "
                "width, so the operator's own choice on the radio is left alone."},

    // ---- Attenuator (0x11) ----------------------------------------------
    ControlSpec{"atten", 0x11, 0, false, "Attenuator",
                Plane::Pan, Encoding::BcdByte, Wiring::Both,
                0, 0x20, "step", 0, 1,
                "setPanAttenuator", "panAttenuatorBtn", true,
                "NOT sub-addressed: the single data byte IS the setting, in BCD dB "
                "(0x00 off, 0x20 = 20 dB). HF and 50 MHz only — the radio ignores "
                "it above and reports OFF."},

    // ---- Receive antenna (0x12) ----------------------------------------
    ControlSpec{"rx.antenna", 0x12, 0x00, true, "Receive-only antenna",
                Plane::Slice, Encoding::OnOff, Wiring::SendOnly,
                0, 1, "main/rx", 0, 1,
                "setSliceRxAntenna", "sliceRxAntennaBtn", true,
                "IC-7300MK2-specific: 00 uses ANT1 for receive; 01 selects the "
                "RX-ANT input. Live B6 firmware returns bare FB to the official "
                "read form. The operator command is therefore optimistic for "
                "this session only; reconnect does not invent or replay state.",
                IcomFeature::RxAntenna},

    // ---- Control (0x1C) --------------------------------------------------
    ControlSpec{"ptt", 0x1C, 0x00, true, "PTT",
                Plane::Transmit, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setKeying", "moxBtn", false,
                "Polled continuously, which is how the transmit indicator follows "
                "the radio's own front-panel PTT."},
    ControlSpec{"tuner", 0x1C, 0x01, true, "Antenna tuner",
                Plane::Transmit, Encoding::Enum, Wiring::Both,
                0, 2, "step", 0, 2,
                "setAtu", "txAtuBtn", true,
                "NOT a tune carrier — it runs the model's internal or external "
                "antenna-tuner matching cycle and it KEYS. There is no universal "
                "attachment query, so only exact model profiles with documented tuner "
                "paths publish capabilities().hasTuner and send this command. The "
                "shared button remains visible but unavailable otherwise.",
                IcomFeature::AntennaTuner},
    ControlSpec{"xfc", 0x1C, 0x02, true, "Transmit frequency monitor",
                Plane::Radio, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setTransmitFrequencyCheck",
                "vfoFmReverseButton / rxFmReverseButton", false,
                "Momentary press-and-hold, not persistent REV. The active model "
                "profile attests 1C 02 01 while held and 00 on release; "
                "polled so front-panel XFC updates both repeater-control surfaces. "
                "Excluded from scrub because asserting it changes the receive "
                "frequency during the check.", IcomFeature::TxFrequencyCheck},
    ControlSpec{"repeater.tx.frequency", 0x1C, 0x03, true,
                "Transmit frequency readback",
                Plane::Slice, Encoding::BcdFreq, Wiring::DecodeOnly,
                0, 0, "Hz", 0, 0,
                "", "", true,
                "IC-9700 extended readback only. Refreshed on confirmed PTT "
                "edges; it does not alter the shared RX frequency presentation.",
                IcomFeature::FmRepeaterExtendedReadback},

    // ---- RIT / XIT (0x21) ------------------------------------------------
    ControlSpec{"rit.offset", 0x21, 0x00, true, "RIT / XIT offset",
                Plane::Slice, Encoding::Bcd4, Wiring::Both,
                -9999, 9999, "Hz", -9999, 9999,
                "setRitOffset", "vfoRitSpin", true,
                "ONE register shared by both: 21 01 and 21 02 choose whether it "
                "applies to receive, transmit or both, so the decoded offset is "
                "published to each. A signed magnitude — the sign is a separate "
                "byte, and folding it into the magnitude tunes the wrong way."},
    ControlSpec{"rit.enable", 0x21, 0x01, true, "RIT enable",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setRitEnabled", "vfoRitBtn", true, ""},
    ControlSpec{"xit.enable", 0x21, 0x02, true, "XIT (dTX) enable",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setXitEnabled", "vfoXitBtn", true, ""},

    // ---- SET menu (0x1A 05) ----------------------------------------------
    ControlSpec{"mod.input.dataoff", 0x1A, 0x05, true, "DATA OFF MOD input",
                Plane::Radio, Encoding::Bcd4, Wiring::Both,
                0, 5, "enum", 0, 5,
                "invokeExtension icom/audio.pc", "pcAudioBtn", true,
                "MODEL-SPECIFIC: item 0118 and WLAN=03 on IC-705; item 0084 "
                "and LAN=05 on IC-7300MK2. PC Audio writes only this voice-mode "
                "selection and confirms it by readback.", IcomFeature::ModulationInput},
    ControlSpec{"mod.input.data", 0x1A, 0x05, true, "DATA MOD input",
                Plane::Radio, Encoding::Bcd4, Wiring::DecodeOnly,
                0, 5, "enum", 0, 5,
                "", "", true,
                "MODEL-SPECIFIC: item 0119 on IC-705 and 0085 on IC-7300MK2. "
                "Radio-authoritative and deliberately never written by PC Audio.",
                IcomFeature::ModulationInput},

    // ---- Scope (0x27) ----------------------------------------------------
    ControlSpec{"scope.onoff", 0x27, 0x10, true, "Scope on/off",
                Plane::Pan, Encoding::OnOff, Wiring::SendOnly,
                0, 1, "on/off", 0, 1,
                "", "", false,
                "Pushed at connect. BOTH this and scope.output are needed — "
                "enabling only this turns the scope on the radio's own screen and "
                "sends us nothing, the number-one black-panadapter cause.",
                IcomFeature::Scope},
    ControlSpec{"scope.output", 0x27, 0x11, true, "Scope data output",
                Plane::Pan, Encoding::OnOff, Wiring::SendOnly,
                0, 1, "on/off", 0, 1,
                "", "", false, "Pushed at connect; see scope.onoff.", IcomFeature::Scope},
    ControlSpec{"scope.span", 0x27, 0x15, true, "Scope span",
                Plane::Pan, Encoding::Bcd4, Wiring::Both,
                2500, 500000, "Hz", 5000, 1000000,
                "setPanBandwidth", "", false,
                "A HALF-width on the wire and a TOTAL width at the seam. Snaps to "
                "one of eight values; a zoom request that resolves to the current "
                "span moves one detent in the requested direction instead.", IcomFeature::Scope},
    ControlSpec{"scope.reference", 0x27, 0x19, true, "Scope reference level",
                Plane::Pan, Encoding::Bcd4, Wiring::SendOnly,
                -20, 20, "dB", -20, 20,
                "invokeExtension icom/scope.reference", "", false,
                "Signed magnitude with a separate sign byte. Extension-only; no UI.",
                IcomFeature::Scope},
    ControlSpec{"scope.fixededge", 0x27, 0x1E, true, "Scope fixed edges",
                Plane::Pan, Encoding::Bcd4, Wiring::Declared,
                1, 3, "preset", 1, 3,
                "", "", false,
                "STUB, and deliberately: FIXED mode's edges are three saved presets "
                "per band, so following a pan drag would overwrite the operator's "
                "own stored scope edges thirty times a second.", IcomFeature::Scope},

    // ---- Transmit passband ------------------------------------------------
    ControlSpec{"tx.bandwidth.slot", 0x16, 0x58, true, "SSB TX bandwidth slot",
                Plane::Transmit, Encoding::Enum, Wiring::DecodeOnly,
                0, 2, "enum", 0, 2,
                "", "", true,
                "00 WIDE, 01 MID, 02 NAR. NAMES A SLOT, IS NOT A PASSBAND: the edges "
                "live in the SET item the slot points at (tx.bandwidth.edges), and "
                "the radio also swaps slots on its own with the speech compressor. "
                "DECODE-ONLY deliberately — AetherSDR's seam carries Hz, not a preset "
                "name, so a write here would be a control with no operator intent "
                "behind it. Read at connect and used to route the edge read/write to "
                "the slot actually in circuit.", IcomFeature::TxBandwidth},
    ControlSpec{"tx.bandwidth.edges", 0x1A, 0x05, true, "SSB TX passband edges",
                Plane::Transmit, Encoding::Enum, Wiring::Both,
                0, 0x53, "Hz", 100, 2900,
                "setTxFilter", "TX low cut frequency", true,
                "MODEL-SPECIFIC ITEM NUMBERS: IC-7300MK2 00 14/15/16/17 and IC-705 "
                "0019/0020/0021/0022 for WIDE/MID/NAR/SSB-D. ONE PACKED BCD BYTE — "
                "high digit indexes the low-edge table, low digit the high-edge "
                "table. The tables differ: IC-7300MK2 low edges are 100/120/150/200/"
                "300/500 Hz and the IC-705's are 100/200/300/500; both share high "
                "edges 2500/2700/2800/2900. NOTHING BETWEEN THEM EXISTS, so a seam "
                "request SNAPS and the applet must show the read-back, never the "
                "request. UNVERIFIED ON THE IC-705: its own guide cites 0017/0018/0019 "
                "in the 16 58 note, which collides with the SSB TX Tone levels — the "
                "0019-0022 run is the consistent reading and the read-back is what "
                "settles it. A model with no profile gets NO write and an empty "
                "txFilterLowEdgesHz, so the UI and the backend decline together.",
                IcomFeature::TxBandwidth},

    // ---- Identity / power ------------------------------------------------
    ControlSpec{"id", 0x19, 0x00, true, "Transceiver ID",
                Plane::Radio, Encoding::None, Wiring::Both,
                0, 255, "civ-addr", 0, 255,
                "", "", true,
                "Model discovery. A backend that hardcodes 0xA4 mis-decodes an "
                "IC-9700 (0xA2) or IC-7610 (0x98) and the failure looks like "
                "corrupt spectrum rather than a wrong model."},
    ControlSpec{"power", 0x18, 0, false, "Power off / on",
                Plane::Radio, Encoding::OnOff, Wiring::Declared,
                0, 1, "on/off", 0, 1,
                "", "", false,
                "NOT WIRED ON PURPOSE. Over WiFi 18 00 drops the WLAN interface, "
                "so the 18 01 that would bring it back has no path — a one-way "
                "trip. capabilities().canReboot is false for this reason."},
};

}  // namespace

std::span<const ControlSpec> controlSpecs() { return kSpecs; }

bool controlSupported(const IcomModel& model, const IcomModelProfile& profile,
                      const ControlSpec& spec) noexcept
{
    if (spec.requiredFeature == IcomFeature::Scope) {
        // Scope startup and status handling have always followed identity
        // geometry. Keep the registry aligned with that real wire behavior;
        // profile evidence remains a separate diagnostic field.
        return model.hasScope;
    }
    return profile.supports(spec.requiredFeature);
}

std::string_view encodingName(Encoding e)
{
    switch (e) {
    case Encoding::None:       return "none";
    case Encoding::Level255:   return "level255";
    case Encoding::OnOff:      return "onoff";
    case Encoding::Enum:       return "enum";
    case Encoding::BcdByte:    return "bcd-byte";
    case Encoding::BcdFreq:    return "bcd-freq";
    case Encoding::ModeFilter: return "mode+filter";
    case Encoding::Bcd4:       return "bcd4";
    case Encoding::Bcd6:       return "bcd6";
    case Encoding::Dtcs:       return "dtcs";
    }
    return "?";
}

std::string_view planeName(Plane p)
{
    switch (p) {
    case Plane::Slice:    return "slice";
    case Plane::Transmit: return "transmit";
    case Plane::Pan:      return "pan";
    case Plane::Radio:    return "radio";
    case Plane::Meter:    return "meter";
    case Plane::None:     return "none";
    }
    return "?";
}

std::string_view wiringName(Wiring w)
{
    switch (w) {
    case Wiring::Both:       return "both";
    case Wiring::SendOnly:   return "send-only";
    case Wiring::DecodeOnly: return "decode-only";
    case Wiring::Declared:   return "declared-only";
    }
    return "?";
}

}  // namespace AetherSDR::icom
