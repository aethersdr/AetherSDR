#include "core/backends/icom/IcomControls.h"

#include <array>

namespace AetherSDR::icom {
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
                "this backend does not yet model."},
    ControlSpec{"filter", 0x06, 0, false, "IF filter slot",
                Plane::Slice, Encoding::ModeFilter, Wiring::Both,
                1, 3, "slot", 1, 3,
                "setSliceFilter", "vfoFilterBtn", true,
                "THREE slots whose widths change with the mode: SSB 3.0/2.4/1.8k, "
                "CW 1.2k/500/250, RTTY 2.4k/500/250, AM+SAM 9/6/3k, FM 15/10/7k, "
                "WFM one fixed filter. Published per mode as rxFilterWidthsHz."},

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
                Plane::Slice, Encoding::Level255, Wiring::Declared,
                0, 255, "Hz", 300, 900,
                "", "", false,
                "NOT WANTED. AetherSDR decodes CW itself, so the radio's pitch is not "
                "ours to set; the CW passband we draw assumes the radio's default "
                "and that is the correct division of labour."},
    ControlSpec{"tx.power", 0x14, 0x0A, true, "RF power",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setTxPower", "txPowerSlider", true, ""},
    ControlSpec{"mic.gain", 0x14, 0x0B, true, "Mic gain",
                Plane::Transmit, Encoding::Level255, Wiring::Both,
                0, 255, "%", 0, 100,
                "setMicGain", "phoneMicSlider", true, ""},
    ControlSpec{"cw.speed", 0x14, 0x0C, true, "Keyer speed",
                Plane::Transmit, Encoding::Level255, Wiring::Declared,
                0, 255, "wpm", 6, 48,
                "", "", false,
                "NOT WANTED. The radio owns its keyer and AetherSDR has its own CW "
                "engine — operator decision, not a gap."},
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
                Plane::Transmit, Encoding::Enum, Wiring::Declared,
                0, 2, "step", 0, 2,
                "", "", false,
                "STUB: 00 off, 01 semi, 02 full. Declared, never used."},
    ControlSpec{"notch", 0x16, 0x48, true, "Manual notch",
                Plane::Slice, Encoding::OnOff, Wiring::Both,
                0, 1, "on/off", 0, 1,
                "setSliceManualNotch", "dspMNBtn", true, ""},
    ControlSpec{"dial.lock", 0x16, 0x50, true, "Dial lock",
                Plane::Radio, Encoding::OnOff, Wiring::Declared,
                0, 1, "on/off", 0, 1,
                "", "", false, "STUB: declared, never used."},
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
                "this session only; reconnect does not invent or replay state."},

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
                "attachment query, so capabilities().hasTuner follows canTransmit and the "
                "button is honest about the OUTCOME (00 none / 01 matched / 02 "
                "tuning) rather than about the hardware."},
    ControlSpec{"xfc", 0x1C, 0x02, true, "Transmit frequency monitor",
                Plane::Radio, Encoding::OnOff, Wiring::Declared,
                0, 1, "on/off", 0, 1,
                "", "", false, "STUB: declared, never used."},

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
                "selection and confirms it by readback."},
    ControlSpec{"mod.input.data", 0x1A, 0x05, true, "DATA MOD input",
                Plane::Radio, Encoding::Bcd4, Wiring::DecodeOnly,
                0, 5, "enum", 0, 5,
                "", "", true,
                "MODEL-SPECIFIC: item 0119 on IC-705 and 0085 on IC-7300MK2. "
                "Radio-authoritative and deliberately never written by PC Audio."},

    // ---- Scope (0x27) ----------------------------------------------------
    ControlSpec{"scope.onoff", 0x27, 0x10, true, "Scope on/off",
                Plane::Pan, Encoding::OnOff, Wiring::SendOnly,
                0, 1, "on/off", 0, 1,
                "", "", false,
                "Pushed at connect. BOTH this and scope.output are needed — "
                "enabling only this turns the scope on the radio's own screen and "
                "sends us nothing, the number-one black-panadapter cause."},
    ControlSpec{"scope.output", 0x27, 0x11, true, "Scope data output",
                Plane::Pan, Encoding::OnOff, Wiring::SendOnly,
                0, 1, "on/off", 0, 1,
                "", "", false, "Pushed at connect; see scope.onoff."},
    ControlSpec{"scope.span", 0x27, 0x15, true, "Scope span",
                Plane::Pan, Encoding::Bcd4, Wiring::Both,
                2500, 500000, "Hz", 5000, 1000000,
                "setPanBandwidth", "", false,
                "A HALF-width on the wire and a TOTAL width at the seam. Snaps to "
                "one of eight values; a zoom request that resolves to the current "
                "span moves one detent in the requested direction instead."},
    ControlSpec{"scope.reference", 0x27, 0x19, true, "Scope reference level",
                Plane::Pan, Encoding::Bcd4, Wiring::SendOnly,
                -20, 20, "dB", -20, 20,
                "invokeExtension icom/scope.reference", "", false,
                "Signed magnitude with a separate sign byte. Extension-only; no UI."},
    ControlSpec{"scope.fixededge", 0x27, 0x1E, true, "Scope fixed edges",
                Plane::Pan, Encoding::Bcd4, Wiring::Declared,
                1, 3, "preset", 1, 3,
                "", "", false,
                "STUB, and deliberately: FIXED mode's edges are three saved presets "
                "per band, so following a pan drag would overwrite the operator's "
                "own stored scope edges thirty times a second."},

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
