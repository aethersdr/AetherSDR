#include "gui/SplitQsySettings.h"

#include <QJsonDocument>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (condition) {
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

QJsonObject parseObject(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

void testDefaults()
{
    const AetherSDR::SplitQsySettings settings;
    check(settings.closeSplitOnQsy, "QSY split closure defaults on");
    check(settings.thresholdHz == 20, "threshold defaults to 20 Hz");

    const auto empty = AetherSDR::SplitQsySettings::fromJson({});
    check(empty.closeSplitOnQsy, "empty settings keep QSY split closure on");
    check(empty.thresholdHz == 20, "empty settings keep the 20 Hz threshold");
}

void testRoundTrip()
{
    AetherSDR::SplitQsySettings settings;
    settings.closeSplitOnQsy = false;
    settings.thresholdHz = 5000;
    const auto restored =
        AetherSDR::SplitQsySettings::fromJson(settings.toJson());
    check(!restored.closeSplitOnQsy, "disabled setting round-trips");
    check(restored.thresholdHz == 5000, "threshold round-trips");
}

void testClampAndMalformedValues()
{
    const auto low = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"thresholdHz":0})"));
    check(low.thresholdHz == 1, "threshold clamps to the 1 Hz minimum");

    const auto high = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"thresholdHz":1e100})"));
    check(high.thresholdHz == 200000,
          "large threshold clamps to the 200000 Hz maximum before narrowing");

    const auto malformed = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":1,"closeSplitOnQsy":"yes","thresholdHz":"20"})"));
    check(malformed.closeSplitOnQsy,
          "wrong-typed enable setting falls back to enabled");
    check(malformed.thresholdHz == 20,
          "wrong-typed threshold falls back to 20 Hz");

    const auto future = AetherSDR::SplitQsySettings::fromJson(
        parseObject(R"({"v":2,"closeSplitOnQsy":false,"thresholdHz":100})"));
    check(future.closeSplitOnQsy,
          "unknown version is not partially read for same-named fields");
    check(future.thresholdHz == 20, "unknown version uses current defaults");
}

void testQsyClosePolicy()
{
    const AetherSDR::SplitQsySettings settings;
    check(!AetherSDR::shouldCloseSplitOnQsy(
              settings, true, true, false, 14.000020, 14.000000),
          "frequency change at the threshold keeps split active");
    check(AetherSDR::shouldCloseSplitOnQsy(
              settings, true, true, false, 14.000021, 14.000000),
          "RX QSY beyond the threshold closes split");
    check(!AetherSDR::shouldCloseSplitOnQsy(
              settings, true, true, true, 14.005000, 14.000000),
          "intentional swap retune does not close split");
    check(!AetherSDR::shouldCloseSplitOnQsy(
              settings, true, false, false, 14.005000, 14.000000),
          "TX slice QSY does not close split");

    auto disabled = settings;
    disabled.closeSplitOnQsy = false;
    check(!AetherSDR::shouldCloseSplitOnQsy(
              disabled, true, true, false, 14.005000, 14.000000),
          "disabled option leaves split active on RX QSY");
}

} // namespace

int main()
{
    testDefaults();
    testRoundTrip();
    testClampAndMalformedValues();
    testQsyClosePolicy();
    return g_failures == 0 ? 0 : 1;
}
