#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Parse a radio-declared band set ("bands=2m,440,23cm") into validated,
// deduplicated kBands names (input order kept, case-folded); unknown names are
// dropped because the input is untrusted. 2200m / 630m are kBands entries but
// not declarable (#4027; isDeclarable() in the .cpp). Empty input -> empty list
// (the real-Flex path). Aliases like "70cm" -> "440" map only to kBands names.
QStringList parseDeclaredBands(const QString& csv);

} // namespace AetherSDR
