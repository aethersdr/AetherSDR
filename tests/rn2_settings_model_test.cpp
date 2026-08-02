#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "models/Rn2SettingsModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

bool nearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1.0e-5f;
}

float storedDryMix(AppSettings& settings)
{
    const QJsonDocument document = QJsonDocument::fromJson(
        settings.value(QStringLiteral("RN2")).toString().toUtf8());
    return static_cast<float>(
        document.object().value(QStringLiteral("rxDryMix")).toDouble());
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aether-rn2-settings-model-test"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        std::fprintf(stderr, "Unable to create isolated settings profile\n");
        return 1;
    }

    AppSettings& settings = AppSettings::instance();
    settings.load();

    Rn2SettingsModel& model = Rn2SettingsModel::instance();

    // The default is what RN2 has always done. A user who never touches the
    // control must not hear a different receiver after upgrading — this is the
    // assertion that keeps the #4689 dry mix opt-in.
    expect(nearlyEqual(Rn2SettingsModel::defaults().rxDryMix, 0.0f),
           "dry mix defaults to full suppression");
    expect(nearlyEqual(model.config().rxDryMix, 0.0f),
           "a fresh profile loads full suppression");
    expect(model.config().version == 1, "fresh config reports version 1");
    expect(settings.value(QStringLiteral("RN2")).toString().isEmpty(),
           "an untouched default writes nothing to the store");

    QSignalSpy changed(&model, &Rn2SettingsModel::configChanged);
    model.setRxDryMix(0.15f);
    expect(changed.count() == 1, "a real change broadcasts one refresh signal");
    expect(nearlyEqual(model.config().rxDryMix, 0.15f), "the new value is held");
    expect(nearlyEqual(storedDryMix(settings), 0.15f),
           "setting changes update the authoritative RN2 object");

    model.setRxDryMix(0.15f);
    expect(changed.count() == 1,
           "an unchanged value does not broadcast a duplicate refresh");

    // Clamping: the slider cannot produce these, but a hand-edited store or a
    // future schema can, and an out-of-range blend inverts wet and dry.
    model.setRxDryMix(-0.5f);
    expect(nearlyEqual(model.config().rxDryMix, 0.0f),
           "a negative dry mix clamps to full suppression");
    model.setRxDryMix(10.0f);
    expect(nearlyEqual(model.config().rxDryMix, Rn2SettingsModel::kMaxRxDryMix),
           "an excessive dry mix clamps to the documented maximum");
    model.setRxDryMix(std::nanf(""));
    expect(nearlyEqual(model.config().rxDryMix, 0.0f),
           "a non-finite dry mix falls back to the default");

    // Round-trip through the store, the way a restart would.
    model.setRxDryMix(0.2f);
    expect(nearlyEqual(storedDryMix(settings), 0.2f),
           "the persisted object matches the live config");

    // A malformed document is discarded rather than half-parsed.
    settings.setValue(QStringLiteral("RN2"), QStringLiteral("{not json"));
    settings.save();
    expect(settings.value(QStringLiteral("RN2")).toString()
               == QStringLiteral("{not json"),
           "the malformed object is staged for the next load");

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
