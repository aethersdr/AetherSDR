#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

// A transverter the CLIENT owns, for a backend whose radio keeps no transverter
// list of its own and declares RadioCapabilities::clientTransverters. A Flex
// holds its transverters as radio-side `xvtr` objects and is never routed
// through this.
//
// The radio only ever tunes the IF. Operating (RF) frequency = IF + LO, with a
// separate transmit LO for split converters such as a QO-100 station: one
// 28.5 MHz IF, 10461 MHz LO down, 2371.5 MHz LO up. Low-side injection only
// (LO below RF), so the spectrum is not mirrored.
struct ClientTransverter {
    QString name;
    double ifLowMhz{0.0};
    double ifHighMhz{0.0};
    double rxLoMhz{0.0};
    double txLoMhz{0.0};   // 0 = same as rxLoMhz
    bool   enabled{true};

    double txLo() const { return txLoMhz > 0.0 ? txLoMhz : rxLoMhz; }
    double rfLowMhz() const { return ifLowMhz + rxLoMhz; }
    double rfHighMhz() const { return ifHighMhz + rxLoMhz; }
    bool   isUsable() const;

    QJsonObject toJson() const;
    static ClientTransverter fromJson(const QJsonObject& o);
};

// The one RF<->IF translation point between RadioModel and a non-Flex backend.
// A transverter is ACTIVE once the operator tunes into its RF window (typed
// frequency, band button, spot click) and stays active while the radio's IF
// stays inside its IF window. Tuning outside every RF window, or the radio's
// own dial leaving the IF window, drops back to native frequencies, so the
// display can never show a converted frequency the hardware is not on.
class ClientTransverterSet {
public:
    void setTransverters(const QVector<ClientTransverter>& list);
    // The radio's own tuning range (0/0 = unknown). A request outside it is
    // not a native frequency, so while a transverter is active it is held at
    // the transverter's band edge rather than sent raw to a radio that would
    // refuse it (a VFO wheel step off the bottom of the QO-100 window).
    void setNativeRangeMhz(double minMhz, double maxMhz);
    const QVector<ClientTransverter>& transverters() const { return m_list; }

    // Outbound: the frequency the operator asked for (RF or native) to the
    // frequency the radio must tune. Activates or clears the transverter.
    double toRadioMhz(double requestedMhz);

    // Inbound: a receive frequency the radio reports, to what the operator sees.
    // Clears the active transverter when the IF leaves its window.
    double toDisplayMhz(double radioMhz);

    // Inbound without changing state, for pan centres and other geometry that
    // may legitimately sit outside the IF window (a scope half a span away).
    double shiftToDisplayMhz(double radioMhz) const;
    double shiftToRadioMhz(double displayMhz) const;

    // The transmit frequency for a radio IF (TX LO), native when none is active.
    double toDisplayTxMhz(double radioMhz) const;

    const ClientTransverter* active() const;
    int  activeIndex() const { return m_active; }
    void setActiveIndex(int index);

private:
    int indexForRf(double mhz) const;
    bool isNative(double mhz) const;

    QVector<ClientTransverter> m_list;
    double m_nativeMinMhz{0.0};
    double m_nativeMaxMhz{0.0};
    int m_active{-1};
};

} // namespace AetherSDR
