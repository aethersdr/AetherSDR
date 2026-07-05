#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Parse a radio-declared band set ("bands=2m,440,23cm" from discovery/status)
// into a canonical band-name list. Names are validated against BandDefs,
// deduplicated, and case-folded, so a malformed or hostile declaration cannot
// inject junk into the band UI; unknown names are dropped (Principle VII —
// untrusted input validated at the boundary). An empty/absent value yields an
// empty list, which is the real-Flex path (band UI unchanged).
QStringList parseDeclaredBands(const QString& csv);

} // namespace AetherSDR
