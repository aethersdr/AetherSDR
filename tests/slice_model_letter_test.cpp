#include "models/SliceModel.h"
#include "core/backends/SliceDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (a_ != e_) { \
        const QString a_str = QString("%1").arg(a_); \
        const QString e_str = QString("%1").arg(e_); \
        std::fprintf(stderr, "FAIL %s:%d  expected %s, got %s\n", \
                     __FILE__, __LINE__, \
                     e_str.toUtf8().constData(), \
                     a_str.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

// aetherd RFC 2.3: SliceModel::applyChanges now takes a typed SliceDelta (the Flex
// wire decode lives in FlexBackend::decodeSliceStatus, covered by
// aetherd_slice_decode_test). This helper builds a delta from a field-setter.
template <class F>
static SliceDelta delta(F&& build)
{
    SliceDelta d;
    build(d);
    return d;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Fallback: letter() returns 'A' + sliceId when index_letter hasn't
    // arrived yet (older firmware, early in status sequence).
    {
        SliceModel s(2);
        EXPECT_EQ(s.letter(), QString("C"));  // 'A' + 2
    }

    // ── applyStatus("index_letter=A") populates the radio-given letter
    // and emits letterChanged with the resolved value.
    {
        SliceModel s(3);
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("A"); }));
        EXPECT_EQ(s.letter(), QString("A"));
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("A"));
    }

    // ── Re-applying the same letter does NOT re-emit letterChanged.
    {
        SliceModel s(1);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("B"); }));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("B"); }));
        EXPECT_EQ(spy.count(), 0);
    }

    // ── Letter change emits exactly once and the resolved value follows.
    {
        SliceModel s(2);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("A"); }));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("C"); }));
        EXPECT_EQ(s.letter(), QString("C"));
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("C"));
    }

    // ── emitLetterRefresh() forces a re-emission with the current value
    // (used when a display preference changes).
    {
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("A"); }));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.emitLetterRefresh();
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("A"));
    }

    // ── Status messages without index_letter don't disturb the stored
    // letter or emit on letterChanged.
    {
        SliceModel s(2);
        s.applyChanges(delta([](SliceDelta& d){ d.letter = QStringLiteral("A"); }));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyChanges(delta([](SliceDelta& d){ d.inUse = true; d.frequency = 14.250; }));
        EXPECT_EQ(s.letter(), QString("A"));
        EXPECT_EQ(spy.count(), 0);
    }

    // ── External receive replacement (KiwiSDR virtual antenna) owns visible
    // audio pan locally. It must not send audio_pan commands to the Flex radio
    // while Flex audio is silently muted for the same slice.
    {
        SliceModel s(4);
        QStringList commands;
        QObject::connect(&s, &SliceModel::commandReady,
                         [&commands](const QString& cmd) { commands.append(cmd); });
        s.applyChanges(delta([](SliceDelta& d){ d.audioPan = 25; }));
        EXPECT_EQ(s.audioPan(), 25);
        EXPECT_EQ(s.flexAudioPan(), 25);

        QSignalSpy panSpy(&s, &SliceModel::audioPanChanged);
        s.setExternalReceiveAudioReplacementMute(true);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_mute=1"));
        EXPECT_EQ(s.audioPan(), 25);
        EXPECT_EQ(s.flexAudioPan(), 25);
        EXPECT_EQ(panSpy.count(), 0);
        commands.clear();

        s.setAudioPan(80);
        EXPECT_EQ(s.audioPan(), 80);
        EXPECT_EQ(s.flexAudioPan(), 25);
        EXPECT_EQ(commands.join(QStringLiteral("|")), QString());
        EXPECT_EQ(panSpy.count(), 1);
        EXPECT_EQ(panSpy.takeFirst().at(0).toInt(), 80);

        s.applyChanges(delta([](SliceDelta& d){ d.audioPan = 10; }));
        EXPECT_EQ(s.audioPan(), 80);
        EXPECT_EQ(s.flexAudioPan(), 10);
        EXPECT_EQ(panSpy.count(), 0);

        s.setExternalReceiveAudioReplacementMute(false, false);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_mute=0"));
        EXPECT_EQ(s.audioPan(), 10);
        EXPECT_EQ(s.flexAudioPan(), 10);
        EXPECT_EQ(panSpy.count(), 1);
        EXPECT_EQ(panSpy.takeFirst().at(0).toInt(), 10);
        commands.clear();

        s.setAudioPan(60);
        EXPECT_EQ(s.audioPan(), 60);
        EXPECT_EQ(s.flexAudioPan(), 60);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_pan=60"));
    }

    // ── External receive replacement also owns visible AGC and SQL controls.
    // These controls should drive the replacement source, not hidden Flex state.
    {
        SliceModel s(5);
        QStringList commands;
        QObject::connect(&s, &SliceModel::commandReady,
                         [&commands](const QString& cmd) { commands.append(cmd); });
        s.applyChanges(delta([](SliceDelta& d){
            d.agcMode = QStringLiteral("slow"); d.agcThreshold = 40;
            d.agcOffLevel = 12; d.squelchOn = true; d.squelchLevel = 35; }));
        EXPECT_EQ(s.agcMode(), QString("slow"));
        EXPECT_EQ(s.agcThreshold(), 40);
        EXPECT_EQ(s.agcOffLevel(), 12);
        EXPECT_EQ(s.squelchOn(), true);
        EXPECT_EQ(s.squelchLevel(), 35);

        QSignalSpy agcModeSpy(&s, &SliceModel::agcModeChanged);
        QSignalSpy agcThresholdSpy(&s, &SliceModel::agcThresholdChanged);
        QSignalSpy agcOffLevelSpy(&s, &SliceModel::agcOffLevelChanged);
        QSignalSpy externalAgcModeSpy(
            &s, &SliceModel::externalReceiveAgcModeChanged);
        QSignalSpy externalAgcThresholdSpy(
            &s, &SliceModel::externalReceiveAgcThresholdChanged);
        QSignalSpy externalAgcOffLevelSpy(
            &s, &SliceModel::externalReceiveAgcOffLevelChanged);
        QSignalSpy externalAutoSquelchSpy(
            &s, &SliceModel::externalReceiveAutoSquelchChanged);
        QSignalSpy squelchSpy(&s, &SliceModel::squelchChanged);
        QSignalSpy externalSquelchSpy(
            &s, &SliceModel::externalReceiveSquelchChanged);
        s.setExternalReceiveAudioReplacementMute(true);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 5 audio_mute=1"));
        EXPECT_EQ(s.agcMode(), QString("slow"));
        EXPECT_EQ(s.receiveAgcMode(), QString("med"));
        EXPECT_EQ(s.flexAgcMode(), QString("slow"));
        EXPECT_EQ(s.agcThreshold(), 40);
        EXPECT_EQ(s.receiveAgcThreshold(), -100);
        EXPECT_EQ(s.flexAgcThreshold(), 40);
        EXPECT_EQ(s.agcOffLevel(), 12);
        EXPECT_EQ(s.receiveAgcOffLevel(), 50);
        EXPECT_EQ(s.flexAgcOffLevel(), 12);
        EXPECT_EQ(s.squelchOn(), true);
        EXPECT_EQ(s.receiveSquelchOn(), false);
        EXPECT_EQ(s.externalReceiveAutoSquelchOn(), false);
        EXPECT_EQ(s.flexSquelchOn(), true);
        EXPECT_EQ(s.squelchLevel(), 35);
        EXPECT_EQ(s.receiveSquelchLevel(), 0);
        EXPECT_EQ(s.flexSquelchLevel(), 35);
        EXPECT_EQ(agcModeSpy.count(), 0);
        EXPECT_EQ(agcThresholdSpy.count(), 0);
        EXPECT_EQ(agcOffLevelSpy.count(), 0);
        EXPECT_EQ(externalAgcModeSpy.count(), 1);
        EXPECT_EQ(externalAgcModeSpy.takeFirst().at(0).toString(), QString("med"));
        EXPECT_EQ(externalAgcThresholdSpy.count(), 1);
        EXPECT_EQ(externalAgcThresholdSpy.takeFirst().at(0).toInt(), -100);
        EXPECT_EQ(externalAgcOffLevelSpy.count(), 1);
        EXPECT_EQ(externalAgcOffLevelSpy.takeFirst().at(0).toInt(), 50);
        EXPECT_EQ(squelchSpy.count(), 0);
        EXPECT_EQ(externalSquelchSpy.count(), 1);
        EXPECT_EQ(externalSquelchSpy.takeFirst().at(0).toBool(), false);
        EXPECT_EQ(externalAutoSquelchSpy.count(), 0);
        commands.clear();

        s.setExternalReceiveAutoSquelch(true);
        EXPECT_EQ(s.externalReceiveAutoSquelchOn(), true);
        EXPECT_EQ(commands.join(QStringLiteral("|")), QString());
        EXPECT_EQ(externalAutoSquelchSpy.count(), 1);
        EXPECT_EQ(externalAutoSquelchSpy.takeFirst().at(0).toBool(), true);

        s.setAgcMode(QStringLiteral("off"));
        s.setAgcThreshold(-40);
        s.setAgcOffLevel(44);
        s.setSquelch(false, 66);
        EXPECT_EQ(s.agcMode(), QString("slow"));
        EXPECT_EQ(s.receiveAgcMode(), QString("off"));
        EXPECT_EQ(s.flexAgcMode(), QString("slow"));
        EXPECT_EQ(s.agcThreshold(), 40);
        EXPECT_EQ(s.receiveAgcThreshold(), -40);
        EXPECT_EQ(s.flexAgcThreshold(), 40);
        EXPECT_EQ(s.agcOffLevel(), 12);
        EXPECT_EQ(s.receiveAgcOffLevel(), 44);
        EXPECT_EQ(s.flexAgcOffLevel(), 12);
        EXPECT_EQ(s.squelchOn(), true);
        EXPECT_EQ(s.receiveSquelchOn(), false);
        EXPECT_EQ(s.externalReceiveAutoSquelchOn(), true);
        EXPECT_EQ(s.flexSquelchOn(), true);
        EXPECT_EQ(s.squelchLevel(), 35);
        EXPECT_EQ(s.receiveSquelchLevel(), 66);
        EXPECT_EQ(s.flexSquelchLevel(), 35);
        EXPECT_EQ(commands.join(QStringLiteral("|")), QString());
        EXPECT_EQ(agcModeSpy.count(), 0);
        EXPECT_EQ(agcThresholdSpy.count(), 0);
        EXPECT_EQ(agcOffLevelSpy.count(), 0);
        EXPECT_EQ(externalAgcModeSpy.count(), 1);
        EXPECT_EQ(externalAgcModeSpy.takeFirst().at(0).toString(), QString("off"));
        EXPECT_EQ(externalAgcThresholdSpy.count(), 1);
        EXPECT_EQ(externalAgcThresholdSpy.takeFirst().at(0).toInt(), -40);
        EXPECT_EQ(externalAgcOffLevelSpy.count(), 1);
        EXPECT_EQ(externalAgcOffLevelSpy.takeFirst().at(0).toInt(), 44);
        EXPECT_EQ(squelchSpy.count(), 0);
        EXPECT_EQ(externalSquelchSpy.count(), 1);
        EXPECT_EQ(externalSquelchSpy.takeFirst().at(0).toBool(), false);
        commands.clear();

        s.applyChanges(delta([](SliceDelta& d){
            d.agcMode = QStringLiteral("fast"); d.agcThreshold = 90;
            d.agcOffLevel = 8; d.squelchOn = true; d.squelchLevel = 12; }));
        EXPECT_EQ(s.agcMode(), QString("fast"));
        EXPECT_EQ(s.receiveAgcMode(), QString("off"));
        EXPECT_EQ(s.flexAgcMode(), QString("fast"));
        EXPECT_EQ(s.agcThreshold(), 90);
        EXPECT_EQ(s.receiveAgcThreshold(), -40);
        EXPECT_EQ(s.flexAgcThreshold(), 90);
        EXPECT_EQ(s.agcOffLevel(), 8);
        EXPECT_EQ(s.receiveAgcOffLevel(), 44);
        EXPECT_EQ(s.flexAgcOffLevel(), 8);
        EXPECT_EQ(s.squelchOn(), true);
        EXPECT_EQ(s.receiveSquelchOn(), false);
        EXPECT_EQ(s.externalReceiveAutoSquelchOn(), true);
        EXPECT_EQ(s.flexSquelchOn(), true);
        EXPECT_EQ(s.squelchLevel(), 12);
        EXPECT_EQ(s.receiveSquelchLevel(), 66);
        EXPECT_EQ(s.flexSquelchLevel(), 12);
        EXPECT_EQ(agcModeSpy.count(), 1);
        EXPECT_EQ(agcModeSpy.takeFirst().at(0).toString(), QString("fast"));
        EXPECT_EQ(agcThresholdSpy.count(), 1);
        EXPECT_EQ(agcThresholdSpy.takeFirst().at(0).toInt(), 90);
        EXPECT_EQ(agcOffLevelSpy.count(), 1);
        EXPECT_EQ(agcOffLevelSpy.takeFirst().at(0).toInt(), 8);
        EXPECT_EQ(squelchSpy.count(), 1);
        EXPECT_EQ(squelchSpy.takeFirst().at(0).toBool(), true);
        EXPECT_EQ(externalAgcModeSpy.count(), 0);
        EXPECT_EQ(externalAgcThresholdSpy.count(), 0);
        EXPECT_EQ(externalAgcOffLevelSpy.count(), 0);
        EXPECT_EQ(externalSquelchSpy.count(), 0);

        s.setExternalReceiveAudioReplacementMute(false, false);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 5 audio_mute=0"));
        EXPECT_EQ(s.agcMode(), QString("fast"));
        EXPECT_EQ(s.agcThreshold(), 90);
        EXPECT_EQ(s.agcOffLevel(), 8);
        EXPECT_EQ(s.squelchOn(), true);
        EXPECT_EQ(s.squelchLevel(), 12);
        EXPECT_EQ(s.externalReceiveAutoSquelchOn(), false);
        EXPECT_EQ(agcModeSpy.count(), 1);
        EXPECT_EQ(agcModeSpy.takeFirst().at(0).toString(), QString("fast"));
        EXPECT_EQ(agcThresholdSpy.count(), 1);
        EXPECT_EQ(agcThresholdSpy.takeFirst().at(0).toInt(), 90);
        EXPECT_EQ(agcOffLevelSpy.count(), 1);
        EXPECT_EQ(agcOffLevelSpy.takeFirst().at(0).toInt(), 8);
        EXPECT_EQ(squelchSpy.count(), 1);
        EXPECT_EQ(squelchSpy.takeFirst().at(0).toBool(), true);
        EXPECT_EQ(squelchSpy.count(), 0);
        EXPECT_EQ(externalSquelchSpy.count(), 0);
        EXPECT_EQ(externalAutoSquelchSpy.count(), 1);
        EXPECT_EQ(externalAutoSquelchSpy.takeFirst().at(0).toBool(), false);
        commands.clear();

        s.setAgcMode(QStringLiteral("med"));
        s.setAgcThreshold(20);
        s.setAgcOffLevel(30);
        s.setSquelch(false, 22);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 5 agc_mode=med|"
                                 "slice set 5 agc_threshold=20|"
                                 "slice set 5 agc_off_level=30|"
                                 "slice set 5 squelch=0|"
                                 "slice set 5 squelch_level=22"));
    }

    // ── step_list: a malformed token is dropped (fail-closed), not admitted as
    // a bogus 0-Hz step. (#4068 review — rfoust.)
    {
        SliceModel s(6);
        s.applyChanges(delta([](SliceDelta& d){ d.stepList = QStringLiteral("10,abc,1000"); }));
        EXPECT_EQ(s.stepList().size(), 2);
        if (s.stepList().size() == 2) {
            EXPECT_EQ(s.stepList()[0], 10);
            EXPECT_EQ(s.stepList()[1], 1000);
        }
    }

    // ── Filter polarity mirror (#3434). FlexLib reports FDV passbands as
    // USB-form (positive lo/hi) for BOTH sidebands; FDVL is lower-sideband and
    // must be mirrored to negative offsets so the overlay draws below the
    // carrier. The mirror is asymmetric-safe (lo,hi)→(-hi,-lo), so the FreeDV
    // low cut is preserved rather than collapsed (the regression #3092 hit).
    {
        // FDVL: asymmetric positive echo → mirrored, both edges kept.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
            d.filterLow = 95; d.filterHigh = 2000;
        }));
        EXPECT_EQ(s.filterLow(),  -2000);   // was 95 → -hi
        EXPECT_EQ(s.filterHigh(),  -95);    // was 2000 → -lo (low cut preserved)
    }
    {
        // FDVU: upper-sideband FreeDV stays positive (mode-aware).
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVU");
            d.filterLow = 95; d.filterHigh = 2000;
        }));
        EXPECT_EQ(s.filterLow(),  95);
        EXPECT_EQ(s.filterHigh(), 2000);
    }
    {
        // LSB: symmetric positive echo → historical (-2700,0) result unchanged.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("LSB");
            d.filterLow = 0; d.filterHigh = 2700;
        }));
        EXPECT_EQ(s.filterLow(),  -2700);
        EXPECT_EQ(s.filterHigh(), 0);
    }
    {
        // USB: wrong-polarity negative echo after restore → mirrored positive.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("USB");
            d.filterLow = -2700; d.filterHigh = 0;
        }));
        EXPECT_EQ(s.filterLow(),  0);
        EXPECT_EQ(s.filterHigh(), 2700);
    }
    {
        // FDVL already correct (negative) → left untouched, no double-flip.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
            d.filterLow = -2000; d.filterHigh = -95;
        }));
        EXPECT_EQ(s.filterLow(),  -2000);
        EXPECT_EQ(s.filterHigh(), -95);
    }
    {
        // FDVU wrong-polarity restore echo → mirrored positive. This is the
        // behavior FDVU actually GAINS from joining the USB family — the
        // stays-positive case above also passed pre-mirror (#3434 review).
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVU");
            d.filterLow = -2000; d.filterHigh = -95;
        }));
        EXPECT_EQ(s.filterLow(),  95);
        EXPECT_EQ(s.filterHigh(), 2000);
    }
    {
        // Plain FDV is USB-family too (FlexLib Slice.cs:543-546) — a
        // wrong-polarity echo is corrected, not skipped (#3434 review).
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDV");
            d.filterLow = -2000; d.filterHigh = -95;
        }));
        EXPECT_EQ(s.filterLow(),  95);
        EXPECT_EQ(s.filterHigh(), 2000);
    }
    {
        // Repeated positive echo is idempotent: the radio keeps reporting
        // USB-form for FDVL on every status; each apply must land on the same
        // canonical values, never oscillate.
        SliceModel s(0);
        auto d1 = delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
            d.filterLow = 95; d.filterHigh = 2000;
        });
        s.applyChanges(d1);
        s.applyChanges(delta([](SliceDelta& d){
            d.filterLow = 95; d.filterHigh = 2000;
        }));
        EXPECT_EQ(s.filterLow(),  -2000);
        EXPECT_EQ(s.filterHigh(), -95);
    }
    {
        // Single-edge echoes (#3434 review): under the mirror a wire edge in
        // the wrong-polarity form maps to the OPPOSITE stored edge — merging
        // it directly would build a carrier-straddling passband.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
            d.filterLow = 95; d.filterHigh = 2000;   // → stored (-2000,-95)
        }));
        // Wire reports only filter_hi=3000 (USB-form width change):
        // canonical stored low becomes -3000. NOT (lo=-2000, hi=3000).
        s.applyChanges(delta([](SliceDelta& d){ d.filterHigh = 3000; }));
        EXPECT_EQ(s.filterLow(),  -3000);
        EXPECT_EQ(s.filterHigh(), -95);
        // Wire reports only filter_lo=200 (USB-form low cut): canonical
        // stored high becomes -200.
        s.applyChanges(delta([](SliceDelta& d){ d.filterLow = 200; }));
        EXPECT_EQ(s.filterLow(),  -3000);
        EXPECT_EQ(s.filterHigh(), -200);
        // A canonical-form (negative) single edge merges directly.
        s.applyChanges(delta([](SliceDelta& d){ d.filterLow = -2500; }));
        EXPECT_EQ(s.filterLow(),  -2500);
        EXPECT_EQ(s.filterHigh(), -200);
    }
    {
        // USB-family single wrong-form edge: crosswise with negation.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("USB");
            d.filterLow = 100; d.filterHigh = 2800;
        }));
        s.applyChanges(delta([](SliceDelta& d){ d.filterLow = -2700; }));
        EXPECT_EQ(s.filterLow(),  100);
        EXPECT_EQ(s.filterHigh(), 2700);
    }
    {
        // Mode-only change (no filter keys in the delta): a second client
        // flips FDVU→FDVL mid-session — the reporter's MultiFlex scenario.
        // The stored positive passband must re-normalize immediately, not
        // wait for the next filter echo (#3434 review).
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVU");
            d.filterLow = 95; d.filterHigh = 2000;
        }));
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
        }));
        EXPECT_EQ(s.filterLow(),  -2000);
        EXPECT_EQ(s.filterHigh(), -95);
    }
    {
        // setFilterWidth boundary defense (#3434 review): client-side callers
        // can replay values captured under the pre-mirror convention (band
        // stack, snapshots, FilterPresets_FDVL, net presets) or pass
        // audio-domain positives (EQ drag) — and when the radio's filter
        // already matches, no echo arrives to heal the model.
        SliceModel s(0);
        s.applyChanges(delta([](SliceDelta& d){
            d.mode = QStringLiteral("FDVL");
            d.filterLow = -2000; d.filterHigh = -95;
        }));
        s.setFilterWidth(95, 2000);                  // stale positive replay
        EXPECT_EQ(s.filterLow(),  -2000);
        EXPECT_EQ(s.filterHigh(), -95);
        s.setFilterWidth(-2500, -95);                // canonical → untouched
        EXPECT_EQ(s.filterLow(),  -2500);
        EXPECT_EQ(s.filterHigh(), -95);
    }

    if (g_failures == 0) {
        std::printf("slice_model_letter_test: all checks passed\n");
        return 0;
    }
    std::printf("slice_model_letter_test: %d failure(s)\n", g_failures);
    return 1;
}
