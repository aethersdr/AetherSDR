#pragma once

#include <QWidget>

class QPushButton;

namespace AetherSDR {

class MeterModel;
class HGauge;

// Radio hardware telemetry applet. Faces are capability-shaped: PACURRENT is
// offered only when a backend supplies a verified scale (not on Flex, whose
// published range clips below real draw; SMART-11281).
class MeterApplet : public QWidget {
    Q_OBJECT
public:
    explicit MeterApplet(QWidget* parent = nullptr);

    void setMeterModel(MeterModel* model);
    void setTelemetryVisibility(bool paTemperature, bool supplyVoltage,
                                bool mainFan, double paCurrentMaxAmps);

private:
    void resolveIndices();
    void onMeterUpdated(int index, float value);
    void updatePaTempDisplay();

    MeterModel* m_model{nullptr};

    HGauge* m_paTempGauge{nullptr};
    HGauge* m_supplyGauge{nullptr};
    HGauge* m_fanGauge{nullptr};
    HGauge* m_paCurrentGauge{nullptr};

    QPushButton* m_tempUnitBtn{nullptr};

    float m_paTemp{0.0f};
    bool  m_hasPaTemp{false};
    bool  m_tempFahrenheit{false};

    // Lazy-resolved meter index (-1 = not yet found)
    int m_fanIdx{-1};
    int m_paCurrentIdx{-1};
};

} // namespace AetherSDR
