#pragma once

// Joins a MicrohamTrioModel to the radio this application has connected: the
// radio's settings scope goes to the Trio model, and the Trio's antenna ports
// are the radio's station antennas (models/StationAntenna.h), offered in the
// RX ANT and TX ANT menus as one station device of RadioModel's registry.
// RadioModel knows only the station-antenna source, never the Trio.

#include "models/StationAntenna.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class MicrohamTrioModel;
class RadioModel;

class MicrohamTrioRadioLink : public QObject {
    Q_OBJECT

public:
    MicrohamTrioRadioLink(RadioModel* radio, MicrohamTrioModel* trio, QObject* parent = nullptr);

    // The id it registers under, and its tokens: this prefix and the Trio's
    // antenna key, "TRIO:ant6".
    static constexpr const char* kSourceId = "microham-trio";
    static constexpr const char* kTokenPrefix = "TRIO:";

    StationAntennaView antennaView() const;
    // Empty when the pick went out; otherwise the sentence for the operator.
    QString selectAntenna(const QString& token, bool tx);

private:
    void refreshRadio();

    QPointer<RadioModel>        m_radio;
    QPointer<MicrohamTrioModel> m_trio;
};

} // namespace AetherSDR
