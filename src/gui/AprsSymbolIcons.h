#pragma once

#include <QColor>
#include <QIcon>

namespace AetherSDR::aprsicons {

// Wireframe/outline icon for an APRS symbol (house, car, sailboat, ...).
// Drawn programmatically as 1.5 px line art on a transparent background at
// 2x for retina, so they stay crisp in the station table and combo boxes at
// 16 px. Codes without a dedicated drawing fall back to a rounded badge
// showing the symbol character itself. Results are cached per (code, color).
QIcon symbolIcon(char symbolTable, char symbolCode,
                 const QColor& color = QColor(0xae, 0xb9, 0xcc));

// Current-conditions icon for weather stations, same wireframe style:
// 0 = cloud, 1 = cloud with rain, 2 = cloud with wind streaks. Values match
// AprsStationList::Station::wxCondition.
QIcon weatherIcon(int condition,
                  const QColor& color = QColor(0xae, 0xb9, 0xcc));

} // namespace AetherSDR::aprsicons
