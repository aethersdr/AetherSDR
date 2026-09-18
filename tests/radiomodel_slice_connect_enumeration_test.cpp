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
// 3. TopologyFallback outside connect window emits "slice set 0 active=1", proving
//    that the production firstSliceSelectionSource decision correctly gates wire writes.
// 4. Mid-session slice creation into an empty list (after connect window expires) uses TopologyFallback.
// 5. ConnectSliceEnumerationGuard does not latch across disconnect/reconnect cycles.

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "gui/BandRecallSliceSelectionPolicy.h"
#include "gui/ConnectSliceEnumerationGuard.h"

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
    ConnectSliceEnumerationGuard connectEnumerationGuard;
    qint64 nowMs{1000};
    QStringList emittedCommands;

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
            const RadioSliceSelectionSource source =
                firstSliceSelectionSource(connectEnumerationGuard.isActive(nowMs));

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
        harness.connectEnumerationGuard.arm(1000);
        harness.nowMs = 1050;
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
    // Test 2: Regression check — TopologyFallback when guard inactive emits active=1
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.nowMs = 10000;  // Guard un-armed / inactive
        harness.attach(&model);

        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "0";
        model.handleSliceStatusForTest(0, s0_kvs, false);

        check(harness.emittedCommands.contains("slice set 0 active=1"),
              "Regression guard: TopologyFallback when guard is inactive sends active=1");
    }

    // =========================================================================
    // Test 3: Mid-session creation into empty list uses TopologyFallback
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.connectEnumerationGuard.arm(1000);
        harness.nowMs = 1050;
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

        // Operator creates a new slice mid-session (nowMs past 3000ms window)
        harness.nowMs = 10000;
        check(!harness.connectEnumerationGuard.isActive(harness.nowMs),
              "Connect enumeration guard is inactive mid-session");

        QMap<QString, QString> s1_kvs;
        s1_kvs["in_use"] = "1";
        s1_kvs["RF_frequency"] = "21.074000";
        s1_kvs["active"] = "0";
        model.handleSliceStatusForTest(1, s1_kvs, false);

        check(harness.emittedCommands.contains("slice set 1 active=1"),
              "Mid-session slice creation into empty list asserts active=1 via TopologyFallback");
    }

    // =========================================================================
    // Test 4: Reconnect lifecycle — guard re-arms and does not latch
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        // Session 1: connect, enumerate, disconnect
        harness.connectEnumerationGuard.arm(1000);
        harness.nowMs = 1050;
        harness.attach(&model);

        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "1";
        model.handleSliceStatusForTest(0, s0_kvs, false);
        check(harness.emittedCommands.isEmpty(), "Session 1 connect emitted 0 commands");

        // Disconnect: cancelArm
        harness.connectEnumerationGuard.cancelArm();
        check(!harness.connectEnumerationGuard.isActive(1050), "Disconnected: guard inactive");

        // Session 2: Reconnect at t=20000
        harness.connectEnumerationGuard.arm(20000);
        check(harness.connectEnumerationGuard.isActive(20050), "Session 2: guard active during burst");
        check(!harness.connectEnumerationGuard.isActive(25000), "Session 2: guard expires cleanly");
    }

    // =========================================================================
    // Test 5: Slow enumeration — window expires before slice burst arrives
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        harness.connectEnumerationGuard.arm(1000);
        // Burst arrives at t=5000 (after 3000ms window expired at t=4000)
        harness.nowMs = 5000;
        harness.attach(&model);

        check(harness.connectEnumerationGuard.expiredUnused(harness.nowMs),
              "Guard reports expired-unused when burst arrives after timeout");

        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "0";
        model.handleSliceStatusForTest(0, s0_kvs, false);

        check(harness.emittedCommands.contains("slice set 0 active=1"),
              "Slow enumeration past guard window falls back to TopologyFallback");
    }

    // =========================================================================
    // Test 6: Event-driven finish — sliceConnectEnumerationFinished disarms guard
    // =========================================================================
    {
        RadioModel model;
        SliceWiringHarness harness;
        // Armed on connect client gui registration
        harness.connectEnumerationGuard.arm(1000);
        harness.nowMs = 1050;
        harness.attach(&model);

        // Enumeration completes (slice list reply)
        harness.connectEnumerationGuard.cancelArm();
        check(!harness.connectEnumerationGuard.isActive(harness.nowMs),
              "Guard immediately disarmed upon enumeration completion");

        // Subsequent slice created on empty radio adopts via TopologyFallback
        QMap<QString, QString> s0_kvs;
        s0_kvs["in_use"] = "1";
        s0_kvs["RF_frequency"] = "14.074000";
        s0_kvs["active"] = "0";
        model.handleSliceStatusForTest(0, s0_kvs, false);

        check(harness.emittedCommands.contains("slice set 0 active=1"),
              "Post-enumeration created slice on empty radio asserts active=1 via TopologyFallback");
    }

    if (g_failures == 0) {
        std::printf("PASS: all radiomodel_slice_connect_enumeration_test checks passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d check(s) FAILED.\n", g_failures);
    return 1;
}
