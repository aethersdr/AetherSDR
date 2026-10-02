// #5347: capacity describes implemented scope streams, not hardware VFOs.
// Socket-free: select production model profiles without connecting a session.
#include "TestSettingsProfile.h"
#include "IcomReceiveContractTestAccess.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <cstdio>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::icom;
using namespace AetherSDR::control;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("icom-panadapter-capacity"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    int failures = 0;
    int dualScopeModels = 0;
    int noScopeModels = 0;
    int publications = 0;
    int publishedCapacity = -1;
    RadioModel radio;
    if (!radio.rebuildBackendForTest(QStringLiteral("icom"))) {
        return 1;
    }
    // Production factory + setupBackend signal wiring, not a test-only relay.
    auto& backend = static_cast<IcomCivBackend&>(*radio.backend());
    ControlResourceStore store;
    RadioResourceAdapter adapter(&radio, &store, QStringLiteral("radio-1"));
    QObject::connect(&radio, &RadioModel::capabilitiesChanged, &radio,
                     [&](bool, const RadioCapabilities& caps) {
        ++publications;
        publishedCapacity = caps.maxPanadapters;
    });
    std::vector<const IcomModel*> profiles{&unknownModel()};
    for (const IcomModel& model : knownModels()) {
        profiles.push_back(&model);
    }
    profiles.push_back(&unknownModel()); // Returning to unknown must clear scope capacity.
    for (const IcomModel* selected : profiles) {
        const IcomModel& model = *selected;
        const int before = publications;
        IcomCivBackendTestAccess::selectModel(backend, model);
        const RadioCapabilities caps = backend.capabilities();
        const int expected = model.hasScope ? 1 : 0;
        dualScopeModels += model.hasScope && model.receivers > 1;
        noScopeModels += !model.hasScope;
        const bool backendCorrect = caps.maxPanadapters == expected
            && caps.maxSlices == model.receivers;
        const auto snapshot = store.get({QStringLiteral("radioSession"), {}, QStringLiteral("radio-1")});
        const QJsonObject published = snapshot
            ? snapshot->value.value("capabilities").toObject() : QJsonObject{};
        const bool modelCorrect = radio.backendCapabilities().maxPanadapters == expected
            && publications > before && publishedCapacity == expected
            // A zero capability currently means "unknown" to this legacy
            // desktop getter; its fallback is not changed by #5347.
            && (expected == 0 || radio.maxPanadapters() == expected);
        const bool resourceCorrect = published.value("maxPanadapters").toInt(-1) == expected
            && published.value("maxSlices").toInt(-1) == model.receivers;
        const bool daemonUnchanged = published.value("receiveModeControl").isNull()
            && published.value("receiveFilterControl").isNull();
        const bool ok = backendCorrect && modelCorrect && resourceCorrect && daemonUnchanged;
        std::printf("[%s] %s: backend/model/resource capacity=%d, slices=%d\n",
                    ok ? "PASS" : "FAIL", qPrintable(caps.model), expected, model.receivers);
        failures += !ok;
    }
    // Protect the regression's discriminating rows against table erosion.
    if (dualScopeModels == 0 || noScopeModels == 0) {
        std::fprintf(stderr, "FAIL: need both multi-receiver scope and no-scope profiles\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
