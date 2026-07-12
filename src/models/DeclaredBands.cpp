#include "DeclaredBands.h"

#include "BandDefs.h"

namespace AetherSDR {

namespace {

// LF/MF (2200m / 630m) are deliberately NOT declarable. #4027's non-goals
// list the utility rows "WWV / GEN / 2200 / 630 … deliberately untouched —
// application features, not hardware band claims." WWV/GEN are already
// firewalled out because they live in the separate kWwvBand/kGenBand
// constants, but LF/MF are ordinary kBands entries, so a gateway sending
// bands=2200m,630m would otherwise get them rendered as hardware-band
// buttons. Exclude them here by their sub-1-MHz upper edge, which is the
// property that distinguishes LF/MF from every amateur HF/VHF/UHF band
// (#4191, follow-up #1 from the #4027 review).
constexpr bool isDeclarable(const BandDef& def)
{
    return def.highMhz >= 1.0;
}

} // namespace

QStringList parseDeclaredBands(const QString& csv)
{
    QStringList out;
    const QStringList parts = csv.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString name = part.trimmed();
        for (const auto& def : kBands) {
            if (!isDeclarable(def))
                continue;
            const QString canon = QString::fromLatin1(def.name);
            if (name.compare(canon, Qt::CaseInsensitive) == 0) {
                if (!out.contains(canon))
                    out.append(canon);
                break;
            }
        }
    }
    return out;
}

} // namespace AetherSDR
