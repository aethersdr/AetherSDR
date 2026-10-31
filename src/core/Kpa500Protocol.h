#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {
namespace Kpa500 {

// ── Band codes (^BNnn;) ───────────────────────────────────────────────────────
// Source: KPA500 Programmer's Reference Rev. A2, §^BN.
// 00=160m, 01=80m, 02=60m, 03=40m, 04=30m, 05=20m,
// 06=17m,  07=15m, 08=12m, 09=10m, 10=6m.
QString bandName(int band);  // out-of-range → "?m"

// ── Status snapshot ───────────────────────────────────────────────────────────
// All fields optional — nullopt until the first valid reply for that command
// lands. Kpa500Connection owns the poll loop and emits this aggregate.

struct Status {
    // ^OS — 0=standby, 1=operate.
    std::optional<bool> operate;

    // ^ON — power status; a final ^ON0; may precede silence after power-off.
    std::optional<bool> powerOn;
    std::optional<bool> speakerOn;

    // ^BN — band index 0–10 (160m–6m), see bandName()
    std::optional<int> band;

    // ^WS — forward output power, watts (0–999). Zero when not transmitting.
    std::optional<float> forwardPowerW;

    // ^WS — SWR (1.0–99.0). Unmeasured (nullopt) when raw value is 000.
    // Source: implied decimal point after 2nd digit, so raw "015" → 1.5.
    std::optional<float> swr;

    // ^TM — PA temperature, 0–150 °C.
    std::optional<float> paTemperatureC;

    // ^VI — PA supply voltage (0.0–99.9 V).
    // Source: implied decimal after 2nd digit, so raw "120" → 12.0 V.
    std::optional<float> paVoltageV;

    // ^VI — PA supply current (0.0–99.9 A).
    // Source: implied decimal after 2nd digit, so raw "053" → 5.3 A.
    std::optional<float> paCurrentA;

    // ^FL — active fault code; 0 = no fault. Decimal, 2-digit.
    // Source: "^FLnn; where nn = current fault identifier; nn=00 = no faults."
    std::optional<int> faultCode;

    // ^FC — fan minimum speed, 0 (off) to 6 (high).
    // Source: KPA500 Programmer's Reference Rev. A2, §^FC.
    std::optional<int> fanMinSpeed;

    // ^RVM — firmware version string, e.g. "01.04" (one-shot on connect).
    std::optional<QString> firmwareVersion;

    // ^SN — serial number string (one-shot on connect).
    std::optional<QString> serialNumber;

    bool hasAnyField() const {
        return operate || powerOn || speakerOn || band || forwardPowerW || swr
            || paTemperatureC || paVoltageV || paCurrentA
            || faultCode || fanMinSpeed || firmwareVersion || serialNumber;
    }
};

// ── Frame parser ──────────────────────────────────────────────────────────────
// Accumulates raw bytes from a QSerialPort and emits one callback per
// complete, well-formed ^CMD[data]; frame. Resyncs on the next '^' after a
// corrupt or truncated frame. Zero networking dependency — directly testable
// against hand-built byte sequences without a live device.
//
// KPA500 wire format (source: Programmer's Reference Rev. A2):
//   ^CMDdata;
// where CMD is 2–3 letters and data is the payload (may be empty for a GET,
// may contain a space for ^WS and ^VI multi-field responses). The amp always
// responds upper-case. The null-command echo (lone ';') is silently discarded.
class FrameParser {
public:
    // Called with decoded cmd (e.g. "WS") and arg (e.g. "500 150") for each
    // well-formed frame. cmd is always upper-case.
    using Callback = std::function<void(const QString& cmd, const QString& arg)>;
    void setCallback(Callback cb) { m_cb = std::move(cb); }
    void setErrorCallback(std::function<void(const QString&)> cb) { m_error = std::move(cb); }

    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

private:
    // A 64-byte EEPROM reply uses 128 hex payload characters plus its header
    // and checksum. Keep enough room for that frame while bounding noisy input.
    static constexpr int kMaxFrameBytes = 160;
    QByteArray m_buf;
    Callback   m_cb;
    std::function<void(const QString&)> m_error;
};

// ── Message decode ────────────────────────────────────────────────────────────
// Applies one decoded (cmd, arg) pair to status, returning true if any field
// changed. All scaling constants verified against KPA500 Programmer's Reference
// Rev. A2.
// accepted distinguishes a valid unchanged reply from malformed/unknown input.
bool applyMessage(const QString& cmd, const QString& arg, Status& status,
                  bool* accepted = nullptr);

// ── Command builders ──────────────────────────────────────────────────────────
// All command text verified against KPA500 Programmer's Reference Rev. A2.

// Generic GET query: ^CMD;
QByteArray buildQuery(const char* cmd);

// ^OS1; — set operate mode
QByteArray buildOperate();

// ^OS0; — set standby mode
QByteArray buildStandby();

// ^FLC; — clear active fault.
// Source: §^FL "SET format: ^FLC; clears the current fault."
QByteArray buildClearFault();

// ^ON0; — power the amplifier off. No power-on SET is defined here.
QByteArray buildPowerOff();
// Bootloader wake: a single uppercase P, with no terminator (Rev. A2).
QByteArray buildPowerOn();
QByteArray buildSetBand(int band);
QByteArray buildSetSpeaker(bool on);

// ^FCn; — set fan minimum speed (0 = off, 6 = high).
// Source: KPA500 Programmer's Reference Rev. A2, §^FC.
QByteArray buildSetFanSpeed(int n);

// ';' — null command, echoes back as ';'. Used for connection health check.
QByteArray buildNullCommand();

// Poll schedules used by Kpa500Connection.
// Fast: one command per 200 ms, each field queried every 800 ms.
const QStringList& fastPollCommands();   // {"WS", "OS", "BN", "FL"}
// Slow (one command per 500 ms, rotated): config/status that rarely changes.
const QStringList& slowPollCommands();   // {"TM", "VI", "ON", "FC", "SP"}
// One-shot after first connect.
const QStringList& connectCommands();    // {"RVM", "SN"}

}  // namespace Kpa500
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Kpa500::Status)
