#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {
class RadioModel;
class Kxpa100Model;

// Associates amplifier telemetry with one radio. Never sends device commands
// or changes radio tuner state, and does not require the optional KXAT100.
class Kxpa100RadioAssociation final : public QObject {
    Q_OBJECT
public:
    Kxpa100RadioAssociation(RadioModel* radio, Kxpa100Model* amplifier, QObject* parent = nullptr);
    static bool associate(RadioModel* radio, Kxpa100Model* amplifier);
    static bool dissociate(RadioModel* radio, Kxpa100Model* amplifier);
    static int effectiveTxFrequencyKhz(RadioModel* radio);
    bool associated() const;
signals:
    void changed();
private:
    void loadAssociation();
    QPointer<RadioModel> m_radio;
    QPointer<Kxpa100Model> m_tuner;
    QString m_boundFamily, m_boundRadio, m_boundSerial, m_observedSerial;
};
} // namespace AetherSDR
