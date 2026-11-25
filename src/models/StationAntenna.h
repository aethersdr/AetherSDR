#pragma once

// A station device outside the radio, an antenna switch, as the RX ANT and TX
// ANT menus offer it: one submenu per device beside the radio's own ports.
// The widgets see tokens, labels and sentences, never the device; a device
// registers through RadioModel::addStationAntennaSource(). Plain data.

#include <QCoreApplication>
#include <QString>
#include <QVector>

namespace AetherSDR {

struct StationAntennaChoice {
    QString token;    // opaque to the widgets; unique within its device
    QString label;    // the switch's own name for the port
    bool    enabled{false};
    QString hint;     // what a pick does, when enabled
    QString reason;   // why not, when not enabled
};

struct StationAntennaView {
    QString id;              // the registration id; a pick names it
    bool    active{false};   // the connected radio sits on this device
    QString heading;         // the device's name: "microHAM Trio"
    // Non-empty while the whole device cannot be used: offline, its session
    // held by another client, its focus on another radio. Its entries stay
    // listed, disabled, with this reason.
    QString unavailable;
    QVector<StationAntennaChoice> rx;
    QVector<StationAntennaChoice> tx;
    QString rxCurrent;       // token in use now, as the device reports it; empty when not known
    QString txCurrent;
    QString rxLabel;         // what each field shows
    QString txLabel;
    QString note;            // a sentence for the submenu, when not empty
};

// The shared refusal sentences for every station antenna switch (RFC #7238).
// A pick made while the radio may radiate:
inline QString stationAntennaTransmitRefusal()
{
    return QCoreApplication::translate("StationAntenna",
                                       "Antenna can't change while transmitting.");
}

// A key-on refused because a switch is moving. No caller here on purpose:
// nothing in this layer gates keying. It is kept beside its twin so the key
// objection of RFC #6237, when it lands, says the same thing every switch
// says; station_antenna_menu_test pins the text until then.
inline QString stationAntennaChangingKeyRefusal()
{
    return QCoreApplication::translate(
        "StationAntenna", "TX failed — Antenna is changing. Key again when it has settled.");
}

// The submenu title: a station switch, not a slice control.
inline QString stationAntennaMenuTitle(const QString& heading)
{
    return QCoreApplication::translate("StationAntenna", "%1 (station switch)").arg(heading);
}

} // namespace AetherSDR
