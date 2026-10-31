#include "ModelCapabilities.h"

namespace AetherSDR {

namespace {

struct Entry {
    const char* model;       // FlexLib model-string key
    ModelCapabilities caps;
};

// Mirrors FlexLib/ModelInfo.cs Platform / Has4Meters / Has2Meters / HasLoopA /
// HasLoopB; keep in sync when Flex ships a model (tests/model_capabilities_test).
// capabilitiesFor() returns the first substring match, so suffix variants
// precede their base ("FLEX-6700R" before "FLEX-6700", "MLS-9601"/"CLS-9301"
// before "ML-9600"/"CL-9300"). Family entries (ML-9600, AU-510, AU-520)
// intentionally catch their W/X/M variants by substring.
constexpr Entry kTable[] = {
    // model key      platform                   4m     2m     LoopA  LoopB  Diversity NTP    Slices IQ max Hz
    {"FLEX-6300",  {RadioPlatform::Microburst, false, false, false, false, false,    false, 2, 96000}},
    {"FLEX-6400M", {RadioPlatform::DeepEddy,   false, false, false, false, false,    false, 2, 96000}},
    {"FLEX-6400",  {RadioPlatform::DeepEddy,   false, false, false, false, false,    false, 2, 96000}},
    {"FLEX-6500",  {RadioPlatform::Microburst, true,  false, true,  false, false,    false, 4, 192000}},  // Region 1 4m mod, LoopA only; single-SCU, no diversity
    {"FLEX-6600M", {RadioPlatform::DeepEddy,   false, false, false, false, true,     false, 4, 192000}},
    {"FLEX-6600",  {RadioPlatform::DeepEddy,   false, false, false, false, true,     false, 4, 192000}},
    {"FLEX-6700R", {RadioPlatform::Microburst, false, false, false, false, true,     false, 8, 192000}},  // Receive-only, per FlexLib
    {"FLEX-6700",  {RadioPlatform::Microburst, true,  true,  true,  true,  true,     false, 8, 192000}},  // Both built-in, LoopA + LoopB
    {"FLEX-8400M", {RadioPlatform::BigBend,    false, false, false, false, false,    true,  2, 96000}},
    {"FLEX-8400",  {RadioPlatform::BigBend,    false, false, false, false, false,    true,  2, 96000}},
    {"FLEX-8600M", {RadioPlatform::BigBend,    false, false, false, false, true,     true,  4, 192000}},
    {"FLEX-8600",  {RadioPlatform::BigBend,    false, false, false, false, true,     true,  4, 192000}},
    {"MLS-9601",   {RadioPlatform::BigBend,    false, false, false, false, true,     false, 4, 192000}},  // before ML-9600
    {"ML-9600",    {RadioPlatform::BigBend,    false, false, false, false, true,     false, 4, 192000}},  // catches ML-9600W / ML-9600X
    {"CLS-9301",   {RadioPlatform::BigBend,    false, false, false, false, true,     false, 4, 192000}},  // before CL-9300
    {"CL-9300",    {RadioPlatform::BigBend,    false, false, false, false, true,     false, 4, 192000}},
    {"AU-510",     {RadioPlatform::BigBend,    false, false, false, false, false,    false, 2, 96000}},  // catches AU-510M
    {"AU-520",     {RadioPlatform::BigBend,    false, false, false, false, true,     false, 4, 192000}},  // catches AU-520M
    {"RT-2122",    {RadioPlatform::DragonFire, false, false, false, false, false,    false, 2, 192000}},
};

} // namespace

QList<int> ModelCapabilities::daxIqSampleRates() const
{
    QList<int> rates;
    for (const int rate : {24000, 48000, 96000, 192000}) {
        if (rate <= maxDaxIqRateHz) {
            rates.append(rate);
        }
    }
    return rates;
}

ModelCapabilities capabilitiesFor(const QString& model)
{
    for (const auto& entry : kTable) {
        if (model.contains(QString::fromLatin1(entry.model),
                           Qt::CaseInsensitive)) {
            return entry.caps;
        }
    }
    return {};
}

} // namespace AetherSDR
