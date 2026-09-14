#pragma once

#include <QFrame>
#include <array>

namespace AetherSDR {
// One independently labeled palette per displayed source; no Z-R conversion.
class WeatherRadarLegend final : public QFrame {
public:
    explicit WeatherRadarLegend(QWidget* parent = nullptr);
    void setProviders(int providers);
private:
    std::array<QWidget*, 4> m_rows{};
};
}
