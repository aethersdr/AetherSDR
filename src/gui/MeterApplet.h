#pragma once

#include <QWidget>

namespace MasterSDR {

class MeterModel;
class HGauge;

// Radio hardware telemetry applet — shows PA temperature, supply voltage,
// and fan speed. Uses hwTelemetryChanged for cached meters (PATEMP, +13.8A)
// and meterUpdated for additional RAD meters resolved by index (MAINFAN).
//
// Note: PACURRENT is intentionally omitted — on FLEX-8000 series the meter
// range is capped at 10A (declared max) while real PA draw exceeds this at
// full power, causing the reading to clip. See FlexRadio community thread
// "PA Current Meter for 6xxx" and bug SMART-11281.
class MeterApplet : public QWidget {
    Q_OBJECT
public:
    explicit MeterApplet(QWidget* parent = nullptr);

    void setMeterModel(MeterModel* model);

private:
    void resolveIndices();
    void onMeterUpdated(int index, float value);

    MeterModel* m_model{nullptr};

    HGauge* m_paTempGauge{nullptr};
    HGauge* m_supplyGauge{nullptr};
    HGauge* m_fanGauge{nullptr};

    // Lazy-resolved meter index (-1 = not yet found)
    int m_fanIdx{-1};
    bool m_resolved{false};
};

} // namespace MasterSDR
