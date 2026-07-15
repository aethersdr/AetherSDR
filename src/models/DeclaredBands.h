#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Parse a radio-declared band set ("bands=2m,440,23cm" from discovery/status)
// into a validated band-name list (input order preserved). Each name is
// validated against BandDefs, deduplicated, and case-folded to its BandDefs
// spelling, so a malformed or hostile declaration cannot inject junk into the
// band UI; unknown names are dropped (Principle VII — untrusted input
// validated at the boundary). The LF/MF utility rows (2200m / 630m) are also
// non-declarable per #4027's non-goals, even though they are kBands entries
// (see the isDeclarable() firewall in DeclaredBands.cpp). An empty/absent
// value yields an empty list, which is the real-Flex path (band UI unchanged).
//
// A short alias table also accepts conventional spellings (e.g. "70cm") and
// resolves them to the canonical kBands name ("440"), so gateways need not
// know AE's naming quirks. Aliases only ever map to names already in kBands,
// so the allow-list guarantee above is unchanged (see kBandAliases in the .cpp).
QStringList parseDeclaredBands(const QString& csv);

} // namespace AetherSDR
