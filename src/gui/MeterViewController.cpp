#include "MeterViewController.h"
#include "DisplaySettings.h"

namespace AetherSDR {

MeterViewController& MeterViewController::instance()
{
    static MeterViewController s_instance;
    return s_instance;
}

MeterViewController::MeterViewController()
    : m_smartMtr(DisplaySettings::smartMtr())  // restore persisted choice
    , m_showExtremes(DisplaySettings::showExtremes())
    , m_extremesSpeed(DisplaySettings::extremesSpeed())
    , m_showValues(DisplaySettings::showValues())
    , m_txMeter(DisplaySettings::txMeter())
    , m_showTxMeterType(DisplaySettings::showTxMeterType())
{
}

void MeterViewController::setSmartMtr(bool on)
{
    if (m_smartMtr == on) {
        return;
    }
    m_smartMtr = on;
    DisplaySettings::setSmartMtr(on);  // persists via AppSettings
    emit changed(on);
}

void MeterViewController::setShowExtremes(bool on)
{
    if (m_showExtremes == on) {
        return;
    }
    m_showExtremes = on;
    DisplaySettings::setShowExtremes(on);
    emit extremesChanged();
}

void MeterViewController::setExtremesSpeed(DisplaySettings::ExtremesSpeed v)
{
    if (m_extremesSpeed == v) {
        return;
    }
    m_extremesSpeed = v;
    DisplaySettings::setExtremesSpeed(v);
    emit extremesChanged();
}

void MeterViewController::setShowValues(DisplaySettings::MeterValues v)
{
    if (m_showValues == v) {
        return;
    }
    m_showValues = v;
    DisplaySettings::setShowValues(v);
    emit extremesChanged();
}

void MeterViewController::setTxMeter(DisplaySettings::TxMeter v)
{
    if (m_txMeter == v) {
        return;
    }
    m_txMeter = v;
    DisplaySettings::setTxMeter(v);
    emit txMeterChanged();
}

void MeterViewController::setShowTxMeterType(bool on)
{
    if (m_showTxMeterType == on) {
        return;
    }
    m_showTxMeterType = on;
    DisplaySettings::setShowTxMeterType(on);
    // Shares the meter-overlay-options broadcast (like showValues): flags re-push
    // options to their SmartMtrWidget and re-seed their menu controls.
    emit extremesChanged();
}

} // namespace AetherSDR
