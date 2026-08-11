#pragma once

class QJsonObject;
class QString;

namespace AetherSDR {

// Owned configuration for the ColibriNANO backend, per Constitution
// Principle V: one nested JSON object under a single root key ("Colibri"),
// read and written atomically, with one place to default.
//
//   spanMhz      — the panadapter span the operator last chose, in MHz. On
//                  this radio the span IS the USB sample rate (48 kHz costs
//                  ~3 Mbit/s over USB, 3.072 MHz costs ~200), so it is opted
//                  into once and remembered, never imposed at connect.
//   dllPath      — optional explicit path to colibrinano_lib; empty means the
//                  default search (exe directory, then the OS loader path).
//   wireAnalytic — the IQ handedness the library delivers (design doc §IQ
//                  handedness). Persisted so live calibration against a known
//                  signal is one settings flip, not a rebuild.
class ColibriSettings {
public:
    // The operator's remembered span in MHz, or 0.0 when they have never
    // chosen one — distinct from any real span, so the backend can apply its
    // own conservative default (48 kHz).
    static double spanMhz();
    static void setSpanMhz(double mhz);

    static QString dllPath();

    static bool wireAnalytic();
    static void setWireAnalytic(bool analytic);

private:
    static QJsonObject readObj();
    static void writeObj(const QJsonObject& obj);
};

}  // namespace AetherSDR
