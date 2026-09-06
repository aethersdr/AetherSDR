#pragma once

namespace AetherSDR {

class RadioSettingsScope;

// Owned configuration for the "Hermes Lite 2" misc-options settings page
// (ticket #10). Radio-scoped state — these describe one physical HL2's
// firmware quirks, not a global preference — so it lives in the
// radio-scoped feature-document store (RFC #4603 proposal A, Constitution
// Principle V "realized in the store"; AGENTS.md "Radio-Scoped Feature
// Documents") behind a caller-supplied RadioSettingsScope, the same
// mechanism Hl2FreqCal uses, rather than a flat AppSettings key.
//
// Field names describe what each control actually DOES, not Thetis's control
// labels: two of Thetis's own labels ("Band Volts", "Disable PS Sync") don't
// match its code (SetADCDither / SetADCRandom), so they are not carried
// forward here.
class Hl2MiscOptionsSettings {
public:
    static constexpr const char* kFeature = "HermesLiteOptions";
    static constexpr int kSchemaVersion = 1;

    // ADC dither / randomization (MetisProtocol config-register bits). Both
    // default false, matching the wire default (see ccConfig()).
    static bool adcDither(const RadioSettingsScope& scope);
    static void setAdcDither(const RadioSettingsScope& scope, bool enabled);
    static bool adcRandom(const RadioSettingsScope& scope);
    static void setAdcRandom(const RadioSettingsScope& scope, bool enabled);

    // Reset the radio when this client disconnects. Defaults false — an
    // operator opts in rather than getting a surprise reset on their first
    // connect.
    static bool resetOnDisconnect(const RadioSettingsScope& scope);
    static void setResetOnDisconnect(const RadioSettingsScope& scope, bool enabled);

    // TX latency / PTT hang (register 0x17). Defaults (20, 12) match the
    // reference implementation's own initial values (Thetis
    // ChannelMaster/netInterface.c, create_rnet()).
    static int txLatency(const RadioSettingsScope& scope);
    static void setTxLatency(const RadioSettingsScope& scope, int value);
    static int pttHang(const RadioSettingsScope& scope);
    static void setPttHang(const RadioSettingsScope& scope, int value);

    // The two fields' widths on the wire (MetisProtocol.h's kTxLatencyMax /
    // kPttHangMax — a 7-bit and a 5-bit field respectively). Re-declared here,
    // rather than making the GUI's settings page import the wire-protocol
    // header directly for two range limits, since that header is
    // above the vendor-header seam for GUI-layer files (see
    // tools/check_engine_boundary.py's EB3). The .cpp static_asserts these
    // equal the real protocol constants, so they cannot silently drift.
    static constexpr int kTxLatencyMax = 127;
    static constexpr int kPttHangMax = 31;

    // Swap left/right on the outgoing transmit audio path. Defaults false.
    //
    // CURRENTLY A NO-OP ON THE WIRE: this backend's EP2 audio slot is always
    // zero (ep2WriteTxIq's EADDR-reuse invariant — see MetisProtocol.h), so
    // there is nothing on this backend's transmit path to swap today. Kept
    // and persisted anyway, for parity with the reference client and so a
    // future revision that routes real audio through this path (or a future
    // non-HL2 openHPSDR family that does) has a setting already in place —
    // the same "kept, harmless, not load-bearing" treatment MetisProtocol.h
    // already gives kConfigMercury/kConfigDuplex.
    static bool swapAudioChannels(const RadioSettingsScope& scope);
    static void setSwapAudioChannels(const RadioSettingsScope& scope, bool enabled);
};

}  // namespace AetherSDR
