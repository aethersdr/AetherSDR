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
    if (DisplaySettings::showExtremes() == on) {
        return;
    }
    DisplaySettings::setShowExtremes(on);
    emit extremesChanged();
}

void MeterViewController::setExtremesSpeed(DisplaySettings::ExtremesSpeed v)
{
    if (DisplaySettings::extremesSpeed() == v) {
        return;
    }
    DisplaySettings::setExtremesSpeed(v);
    emit extremesChanged();
}

void MeterViewController::setShowValues(DisplaySettings::MeterValues v)
{
    if (DisplaySettings::showValues() == v) {
        return;
    }
    DisplaySettings::setShowValues(v);
    emit extremesChanged();
}

void MeterViewController::setTxMeter(DisplaySettings::TxMeter v)
{
    if (DisplaySettings::txMeter() == v) {
        return;
    }
    DisplaySettings::setTxMeter(v);
    emit txMeterChanged();
}

} // namespace AetherSDR
