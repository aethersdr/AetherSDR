#pragma once

#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Persistence for the AetherModem APRS client. Per Constitution Principle V,
// the configuration lives as one nested JSON blob under a single AppSettings
// key ("AetherModemAprs") rather than a stack of flat entries. Mirrors the
// TncSettings pattern in Ax25HfPacketDecodeDialog.h.
class AprsSettings {
public:
    static constexpr int kDefaultBeaconIntervalMin = 30;

    static QString myCall();             // "" until the operator sets one
    static bool modemAutostart();        // enable the modem at app launch
    static bool beaconEnabled();
    static int beaconIntervalMinutes();  // clamped to [1, 1440]
    static QString beaconText();
    static QString symbol();             // two chars: table + code, e.g. "/-"
    static QString path();               // "WIDE1-1,WIDE2-1"
    static QString manualLat();          // decimal degrees as text, "" unset
    static QString manualLon();

    static void setMyCall(const QString& call);
    static void setModemAutostart(bool on);
    static void setBeaconEnabled(bool on);
    static void setBeaconIntervalMinutes(int minutes);
    static void setBeaconText(const QString& text);
    static void setSymbol(const QString& tableAndCode);
    static void setPath(const QString& path);
    static void setManualPosition(const QString& lat, const QString& lon);

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
    static void setString(const char* key, const QString& value);
};

} // namespace AetherSDR
