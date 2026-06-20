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

} // namespace AetherSDR
