// Connect enumeration must never overwrite the radio's active slice selection.
// Principle II: The Radio Is Authoritative On Live State.
//
// When connecting to a FlexRadio with multiple existing slices, the radio
// enumerates slices in creation order (slice 0, slice 1, ...). The radio-active
// slice may be slice 1 (e.g. active=0 on slice 0, active=1 on slice 1).
//
// AetherSDR adopts the first enumerated slice (slice 0) to give the local UI
// an initial selection target before enumeration finishes. That bootstrap
// MUST use RadioSliceSelectionSource::InitialEnumeration (suppressActiveCommand=true).
// If it used TopologyFallback, it would issue "slice set 0 active=1" over the wire,
// clobbering the radio's live state and forcing slice 0 active before slice 1
// even arrived.
//
// This test verifies that:
// 1. Initial connect enumeration emits ZERO "active=1" commands to the wire.
// 2. The client correctly converges on the radio-reported active slice (slice 1).
// 3. Using TopologyFallback on connect would emit "slice set 0 active=1", proving
//    that this test catches call-site regressions.
// 4. Mid-session slice creation into an empty list uses TopologyFallback.

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "gui/BandRecallSliceSelectionPolicy.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct SliceWiringHarness {
    RadioModel* model{nullptr};
    int activeSliceId{-1};
    bool initialSliceEnumeration{true};
    QStringList emittedCommands;
    bool forceTopologyFallbackForTest{false};

    void attach(RadioModel* m) {
        model = m;
        QObject::connect(model, &RadioModel::sliceAdded, [this](SliceModel* s) {
            onSliceAdded(s);
        });
    }

    void onSliceAdded(SliceModel* s) {
        QObject::connect(s, &SliceModel::commandReady, [this](const QString& cmd) {
            emittedCommands.append(cmd);
        });

        const bool firstSlice = (activeSliceId < 0);
        if (firstSlice) {
            RadioSliceSelectionSource source = initialSliceEnumeration
                ? RadioSliceSelectionSource::InitialEnumeration
                : RadioSliceSelectionSource::TopologyFallback;

            if (forceTopologyFallbackForTest) {
                source = RadioSliceSelectionSource::TopologyFallback;
            }

            initialSliceEnumeration = false;

            const RadioSliceSelectionDecision decision =
                radioSliceSelectionDecision(false, source);

            if (!decision.suppressActiveCommand) {
                s->setActive(true);
            }
            activeSliceId = s->sliceId();
        }

        // Live status adoption: if a subsequent slice arrives with active=1 from radio
        if (s->isActive() && s->sliceId() != activeSliceId) {
            activeSliceId = s->sliceId();
        }
    }

    void onSliceRemoved(int id) {
        if (activeSliceId == id) {
            const auto slices = model->slices();
            if (slices.isEmpty()) {
                activeSliceId = -1;
            } else {
                activeSliceId = slices.first()->sliceId();
                const RadioSliceSelectionDecision decision =
                    radioSliceSelectionDecision(false, RadioSliceSelectionSource::TopologyFallback);
                if (!decision.suppressActiveCommand) {
                    slices.first()->setActive(true);
                }
            }
        }
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // =========================================================================
    // Test 1: Connect enumeration with Slice 0 (active=0) and Slice 1 (active=1)
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.attach(&model);

        // Simulate connect enumeration burst from FlexRadio:
        // Frame 1: Slice 0 status (inactive)
        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "0";
        model.handleSliceStatusForTest(0, s0_kvs, false);

        check(harness.activeSliceId == 0,
              "Precondition: Slice 0 adopted locally as UI bootstrap target");
        check(harness.emittedCommands.isEmpty(),
              "Initial enumeration of Slice 0 must NOT emit active=1 command");

        // Frame 2: Slice 1 status (active on radio)
        QMap<QString, QString> s1_kvs;
        s1_kvs["in_use"] = "1";
        s1_kvs["RF_frequency"] = "7.074000";
        s1_kvs["active"] = "1";
        model.handleSliceStatusForTest(1, s1_kvs, false);

        check(harness.activeSliceId == 1,
              "Client converges on radio-active Slice 1 upon enumeration");
        check(harness.emittedCommands.isEmpty(),
              "Complete connect enumeration burst emitted ZERO active=1 commands");

        check(model.slice(0) != nullptr && !model.slice(0)->isActive(),
              "Slice 0 model is inactive");
        check(model.slice(1) != nullptr && model.slice(1)->isActive(),
              "Slice 1 model is active");
    }

    // =========================================================================
    // Test 2: Regression check — TopologyFallback WOULD emit active=1
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.forceTopologyFallbackForTest = true;
        harness.attach(&model);

        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "0";
        model.handleSliceStatusForTest(0, s0_kvs, false);

        check(harness.emittedCommands.contains("slice set 0 active=1"),
              "Regression guard: TopologyFallback on connect improperly sends active=1");
    }

    // =========================================================================
    // Test 3: Mid-session creation into empty list uses TopologyFallback
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.attach(&model);

        // Connect enumeration
        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "1";
        model.handleSliceStatusForTest(0, s0_kvs, false);
        harness.emittedCommands.clear();

        // Operator closes Slice 0 mid-session
        model.handleSliceStatusForTest(0, QMap<QString, QString>{}, true);
        harness.onSliceRemoved(0);
        check(harness.activeSliceId == -1, "All slices removed: activeSliceId is -1");
        check(!harness.initialSliceEnumeration, "Initial connect enumeration is false");

        // Operator creates a new slice mid-session
        QMap<QString, QString> s1_kvs;
        s1_kvs["in_use"] = "1";
        s1_kvs["RF_frequency"] = "21.074000";
        s1_kvs["active"] = "0";
        model.handleSliceStatusForTest(1, s1_kvs, false);

        check(harness.emittedCommands.contains("slice set 1 active=1"),
              "Mid-session slice creation into empty list asserts active=1 via TopologyFallback");
    }

    if (g_failures == 0) {
        std::printf("PASS: all radiomodel_slice_connect_enumeration_test checks passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d check(s) FAILED.\n", g_failures);
    return 1;
}
