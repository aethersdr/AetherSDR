#include "Kxpa100RadioAssociation.h"
#include "Kxpa100Model.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include <QLoggingCategory>
#include <cmath>

namespace AetherSDR {
namespace {
Q_LOGGING_CATEGORY(lcKxpa100Association, "aether.kxpa100")
}
int Kxpa100RadioAssociation::effectiveTxFrequencyKhz(RadioModel* radio)
{
    const SliceModel* slice = radio && radio->isConnected() ? radio->txSlice() : nullptr;
    if (!slice) { return 0; }
    const double mhz = slice->frequency()
        + ((slice->mode() == "FM" || slice->mode() == "NFM") ? slice->txOffsetFreq() : 0.0)
        + (slice->xitOn() ? slice->xitFreq() / 1.0e6 : 0.0);
    return std::isfinite(mhz) && mhz >= 0.001 && mhz < 100.0
        ? static_cast<int>(std::llround(mhz * 1000.0)) : 0;
}
Kxpa100RadioAssociation::Kxpa100RadioAssociation(RadioModel* radio, Kxpa100Model* amplifier, QObject* parent)
    : QObject(parent), m_radio(radio), m_tuner(amplifier)
{
    if (amplifier) {
        connect(amplifier, &Kxpa100Model::associationChanged, this, &Kxpa100RadioAssociation::loadAssociation);
        connect(amplifier, &Kxpa100Model::detectedChanged, this, &Kxpa100RadioAssociation::loadAssociation);
        connect(amplifier, &Kxpa100Model::statusUpdated, this, [this](const Kxpa100::Status& status) {
            if (status.serial != m_observedSerial) {
                m_observedSerial = status.serial;
                loadAssociation();
            }
        });
    }
    if (radio) {
        connect(radio, &RadioModel::connectionStateChanged, this, &Kxpa100RadioAssociation::loadAssociation);
        connect(radio, &RadioModel::infoChanged, this, &Kxpa100RadioAssociation::loadAssociation);
    }
    loadAssociation();
}
bool Kxpa100RadioAssociation::associated() const
{
    if (!m_radio || !m_tuner || !m_radio->isConnected() || m_boundSerial.isEmpty()) { return false; }
    const auto scope = m_radio->settingsScope();
    return scope.family() == m_boundFamily && scope.radioId() == m_boundRadio
        && m_tuner->status().serial == m_boundSerial;
}
bool Kxpa100RadioAssociation::associate(RadioModel* radio, Kxpa100Model* tuner)
{
    if (!radio || !radio->isConnected() || !tuner || !tuner->isDetected() || !tuner->isResponding()) { return false; }
    const auto scope = radio->settingsScope();
    if (!scope.isValid() || scope.radioId().isEmpty()) { return false; }
    int version = 0;
    AppSettings::FeatureReadStatus state;
    QJsonObject doc = scope.featureExact("kxpa100", &version, &state);
    if (version > 1 || state == AppSettings::FeatureReadStatus::Corrupt || state == AppSettings::FeatureReadStatus::Unavailable) { return false; }
    doc["serial"] = tuner->status().serial;
    doc.remove("useTxControls");
    if (!scope.setFeature("kxpa100", 1, doc)) {
        qCWarning(lcKxpa100Association) << "KXPA100: radio association could not be saved; existing association retained";
        return false;
    }
    emit tuner->associationChanged();
    return true;
}
void Kxpa100RadioAssociation::loadAssociation()
{
    m_boundFamily.clear(); m_boundRadio.clear(); m_boundSerial.clear();
    if (m_radio && m_tuner) {
        const auto scope = m_radio->settingsScope();
        int version = 0;
        const QJsonObject doc = scope.featureExact("kxpa100", &version);
        if (version == 1 && !scope.radioId().isEmpty() && doc.value("serial").toString() == m_tuner->status().serial) {
            m_boundFamily = scope.family(); m_boundRadio = scope.radioId(); m_boundSerial = m_tuner->status().serial;
        }
    }
    emit changed();
}
bool Kxpa100RadioAssociation::dissociate(RadioModel* radio, Kxpa100Model* tuner)
{
    if (!radio || !tuner) { return false; }
    const auto scope = radio->settingsScope();
    if (scope.radioId().isEmpty()) { return false; }
    int version = 0;
    QJsonObject doc = scope.featureExact("kxpa100", &version);
    if (version != 1) { return false; }
    doc.remove("serial");
    if (!scope.setFeature("kxpa100", 1, doc)) {
        qCWarning(lcKxpa100Association) << "KXPA100: radio association could not be saved; existing association retained";
        return false;
    }
    emit tuner->associationChanged();
    return true;
}
} // namespace AetherSDR
