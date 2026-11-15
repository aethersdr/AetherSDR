#include "DeclaredBands.h"

#include "BandDefs.h"

#include <cstddef>
#include <string_view>

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

// Alternate spellings mapped to canonical kBands names (AE calls 70cm "440").
// Invariant: an alias maps only to an existing kBands name, never a new band,
// and the resolved name still passes isDeclarable + the allow-list. Keep it to
// mismatches actually observed from gateways.
struct BandAlias {
    const char* alias;
    const char* canonical;
};
constexpr BandAlias kBandAliases[] = {
    {"70cm", "440"},   // UHF: every ham + gateway spells it 70cm; AE names it 440
};

// Constexpr ASCII case-insensitive compare. Band tokens are ASCII, so this
// matches resolveAlias()'s Qt::CaseInsensitive comparison for the shadow check
// below while staying evaluable at compile time.
constexpr char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr bool ciEqualAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

// Compile-time alias invariants:
//   (1) canonical exists in kBands (else the allow-list drops it);
//   (2) canonical is declarable (resolveAlias() runs before isDeclarable);
//   (3) the alias key is not a real kBands name (resolveAlias() runs before the
//       allow-list match, so it would shadow that band).
constexpr bool aliasesAreValid()
{
    for (const auto& a : kBandAliases) {
        // (3) alias key must not shadow a real band (case-insensitive, matching
        //     resolveAlias's own comparison).
        for (const auto& def : kBands)
            if (ciEqualAscii(def.name, a.alias))
                return false;
        // (1) + (2) canonical must name an existing, declarable kBands entry.
        bool ok = false;
        for (const auto& def : kBands)
            if (std::string_view(def.name) == a.canonical && isDeclarable(def)) {
                ok = true;
                break;
            }
        if (!ok)
            return false;
    }
    return true;
}
static_assert(aliasesAreValid(),
              "every kBandAliases entry must (1) name an existing kBands "
              "canonical, (2) that is declarable, and (3) use an alias key that "
              "does not collide with any real kBands name");

// If `name` is a known alias, return its canonical kBands spelling; otherwise
// return `name` unchanged. Case-insensitive on the alias key.
QString resolveAlias(const QString& name)
{
    for (const auto& a : kBandAliases) {
        if (name.compare(QLatin1String(a.alias), Qt::CaseInsensitive) == 0)
            return QString::fromLatin1(a.canonical);
    }
    return name;
}

} // namespace

QStringList parseDeclaredBands(const QString& csv)
{
    QStringList out;
    const QStringList parts = csv.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        // Resolve a conventional spelling (e.g. "70cm") to its canonical kBands
        // name before matching. A non-alias token passes through unchanged, so
        // the allow-list below is still the sole gate on what gets rendered.
        const QString name = resolveAlias(part.trimmed());
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
