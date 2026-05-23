#include "ModelCapabilities.h"

namespace MasterSDR {

namespace {

struct Entry {
    const char* model;       // FlexLib model-string key
    ModelCapabilities caps;
};

// Mirrors FlexLib/ModelInfo.cs Has4Meters / Has2Meters columns.  Keep
// this table in sync with FlexLib when Flex ships a new model — the
// tests/model_capabilities_test harness will fail loudly if a flag
// drifts away from the upstream source.
//
// Ordering matters: capabilitiesFor() returns the first substring
// match, so longer/suffix variants (FLEX-6400M, FLEX-6700R) must come
// before their base model (FLEX-6400, FLEX-6700) so that an exact
// "FLEX-6700R" status string doesn't match "FLEX-6700" first.
constexpr Entry kTable[] = {
    {"FLEX-6300",  {false, false}},
    {"FLEX-6400M", {false, false}},
    {"FLEX-6400",  {false, false}},
    {"FLEX-6500",  {true,  false}},  // Region 1 4m mod
    {"FLEX-6600M", {false, false}},
    {"FLEX-6600",  {false, false}},
    {"FLEX-6700R", {false, false}},  // Receive-only, per FlexLib
    {"FLEX-6700",  {true,  true }},  // Both built-in
    {"FLEX-8400M", {false, false}},
    {"FLEX-8400",  {false, false}},
    {"FLEX-8600M", {false, false}},
    {"FLEX-8600",  {false, false}},
    {"AU-520",     {false, false}},
    {"ML-380",     {false, false}},
    {"CL-200",     {false, false}},
    {"RT-100",     {false, false}},
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

} // namespace MasterSDR
