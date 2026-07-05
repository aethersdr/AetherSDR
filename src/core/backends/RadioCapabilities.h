#pragma once

#include <QString>
#include <QVector>
#include <QVariantMap>

namespace AetherSDR {

// The honest, self-declared feature set of a connected radio, produced by an
// IRadioBackend and surfaced to clients (aetherd RFC §4.1 `welcome`). Clients
// render against what the radio *reports* — a control the radio lacks is
// disabled/absent — instead of hard-coding "the radio is a Flex". This is the
// structural replacement for the model-impersonation anti-pattern (RFC §1).
//
// Design (RFC §5.5 open-question Q1): a TYPED struct for the core profile —
// the surface every radio family has — plus a namespaced `extensions` bag for
// vendor-specific capability values that don't belong in the core. Typed where
// it's universal, open where it's vendor.
//
// NOT the same as models/ModelCapabilities: that is model-string-*derived*
// truth (a static FlexLib platform table keyed by the model name, Principle I);
// this is the radio's *reported* self-description produced by a backend and
// surfaced to clients. A FlexBackend may seed this FROM ModelCapabilities, but
// the two are distinct concepts (derived-from-name vs reported-by-backend).
struct RadioCapabilities {
    // Identity
    QString family;   // backend id: "flex", "kiwi", … (stable, lowercase)
    QString model;    // radio model string as reported by the hardware

    // Receive
    int maxSlices = 1;             // independent demod slices the radio supports
    int maxPanadapters = 1;        // simultaneous panadapters
    QVector<int> sampleRatesHz;    // supported per-receiver sample rates (Hz)

    // Transmit — the load-bearing capability for TX safety (RFC §6). A backend
    // that cannot key sets canTransmit=false; the engine guard then denies any
    // keying intent regardless of client requests.
    bool canTransmit = false;
    double txPowerMaxWatts = 0.0;  // 0 when RX-only

    // Peripherals / features every family may or may not have
    bool hasTuner = false;         // antenna tuner / ATU
    bool hasAmplifier = false;     // integrated or controllable PA
    bool hasExtendedDsp = false;   // extended firmware DSP filters (NRS/RNN/NRF)

    // Vendor-specific capabilities, keyed by extension namespace. Clients that
    // don't understand a namespace ignore it; a backend never puts core-profile
    // fields here. Example: {"flex": {"multiFlex": true, "guiClientId": "…"}}.
    QVariantMap extensions;

    // The vendor-extension namespaces this backend implements (for the
    // capability handshake). A client can pre-check before issuing
    // invokeExtension(ns, …).
    QVector<QString> extensionNamespaces;
};

}  // namespace AetherSDR
