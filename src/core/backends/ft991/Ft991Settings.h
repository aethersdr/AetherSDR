#pragma once

class QJsonObject;
class QString;

namespace AetherSDR {

// Owned configuration for the FT-991 backend, per Constitution Principle V:
// one nested JSON object under a single root key ("Ft991"), read and written
// atomically, with one place to default.
//
//   baudRate     — CAT serial rate. Default 38400; must match the radio's
//                  menu 031 CAT RATE. (Framing is fixed 8N2 per the manual.)
//   audioInHint  — case-insensitive substring matched against the capture
//                  device description. Default "usb audio codec", which is
//                  what the FT-991's built-in codec enumerates as.
//   audioOutHint — same, for the TX playback device.
//
// The COM port is NOT stored here: it is the radio's discovery identity
// (serial "ft991-<port>"), chosen in the picker like any other radio.
class Ft991Settings {
public:
    static int baudRate();
    static void setBaudRate(int baud);

    static QString audioInHint();
    static QString audioOutHint();

private:
    static QJsonObject readObj();
    static void writeObj(const QJsonObject& obj);
};

}  // namespace AetherSDR
