#pragma once

#include <QFrame>
#include <array>

namespace AetherSDR {
// One independently labeled palette per displayed source; no Z-R conversion.
class WeatherRadarLegend final : public QFrame {
public:
    explicit WeatherRadarLegend(QWidget* parent = nullptr);
    void setProviders(int providers);
    void setLegendVisible(bool visible);
private:
    std::array<QWidget*, 4> m_rows{};
    int m_providers{0};
    bool m_legendVisible{false};
};
}
