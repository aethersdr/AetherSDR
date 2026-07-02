#include "ModelCapabilities.h"

namespace AetherSDR {

namespace {

struct Entry {
    const char* model;       // FlexLib model-string key
    ModelCapabilities caps;
};

// Mirrors FlexLib/ModelInfo.cs Platform / Has4Meters / Has2Meters /
// HasLoopA / HasLoopB columns (Principle I — FlexLib is the model
// authority).  Keep this table in sync with FlexLib when Flex ships a
// new model — the tests/model_capabilities_test harness will fail loudly
// if a flag drifts away from the upstream source.
//
// Ordering matters: capabilitiesFor() returns the first substring match,
// so longer/suffix variants must come before their base model — an exact
// "FLEX-6700R" status string must not match "FLEX-6700" first, and the
// "S" server variants ("MLS-9601", "CLS-9301") must precede the base
// "ML-9600"/"CL-9300" families they'd otherwise be mistaken for.  Family
// entries (ML-9600, AU-510, AU-520) intentionally catch their W/X/M
// variants (ML-9600W, AU-510M, ...) by substring.
constexpr Entry kTable[] = {
    // model key      platform                   4m     2m     LoopA  LoopB  Diversity  Slices
    {"FLEX-6300",  {RadioPlatform::Microburst, false, false, false, false, false,     2}},
    {"FLEX-6400M", {RadioPlatform::DeepEddy,   false, false, false, false, false,     2}},
    {"FLEX-6400",  {RadioPlatform::DeepEddy,   false, false, false, false, false,     2}},
    {"FLEX-6500",  {RadioPlatform::Microburst, true,  false, true,  false, false,     4}},  // Region 1 4m mod, LoopA only; single-SCU, no diversity
    {"FLEX-6600M", {RadioPlatform::DeepEddy,   false, false, false, false, true,      4}},
    {"FLEX-6600",  {RadioPlatform::DeepEddy,   false, false, false, false, true,      4}},
    {"FLEX-6700R", {RadioPlatform::Microburst, false, false, false, false, true,      8}},  // Receive-only, per FlexLib
    {"FLEX-6700",  {RadioPlatform::Microburst, true,  true,  true,  true,  true,      8}},  // Both built-in, LoopA + LoopB
    {"FLEX-8400M", {RadioPlatform::BigBend,    false, false, false, false, false,     2}},
    {"FLEX-8400",  {RadioPlatform::BigBend,    false, false, false, false, false,     2}},
    {"FLEX-8600M", {RadioPlatform::BigBend,    false, false, false, false, true,      4}},
    {"FLEX-8600",  {RadioPlatform::BigBend,    false, false, false, false, true,      4}},
    {"MLS-9601",   {RadioPlatform::BigBend,    false, false, false, false, true,      4}},  // before ML-9600
    {"ML-9600",    {RadioPlatform::BigBend,    false, false, false, false, true,      4}},  // catches ML-9600W / ML-9600X
    {"CLS-9301",   {RadioPlatform::BigBend,    false, false, false, false, true,      4}},  // before CL-9300
    {"CL-9300",    {RadioPlatform::BigBend,    false, false, false, false, true,      4}},
    {"AU-510",     {RadioPlatform::BigBend,    false, false, false, false, false,     2}},  // catches AU-510M
    {"AU-520",     {RadioPlatform::BigBend,    false, false, false, false, true,      4}},  // catches AU-520M
    {"RT-2122",    {RadioPlatform::DragonFire, false, false, false, false, false,     2}},
};

} // namespace

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
