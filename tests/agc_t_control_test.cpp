// #5384 -- the AGC-T knob on the controller surfaces (the MIDI/StreamDeck/
// Ulanzi parameter registry, the FlexControl/TMate2 wheel funnel, the keyboard
// steps) must follow the slice's AGC mode: agc_off_level while AGC is off,
// agc_threshold otherwise -- the split the GUI slider has honoured since #1183.
// SliceModel's agcTKnob* members own that decision; this pins them against a
// real SliceModel. Pure model: no socket, no radio.
//
// Mode strings are the wire vocabulary ("off" / "slow" / "med" / "fast" --
// the docs/agc-t-calibration-design.md property table, and what RxApplet /
// VfoWidget / the agc_cycle shortcut compare against). Threshold / off-level numbers are CONSTRUCTED: they exercise
// routing only and stand for no captured radio state.
#include "core/KiwiSdrProtocol.h"
#include "core/backends/SliceDelta.h"
#include "models/SliceModel.h"

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

#define EXPECT_TRUE(cond, what) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        ++g_failures; \
    } \
} while (0)

template <class F>
static SliceDelta delta(F&& build)
{
    SliceDelta d;
    build(d);
    return d;
}

static QString lastCommand(const QSignalSpy& spy)
{
    return spy.isEmpty() ? QString() : spy.last().at(0).toString();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. AGC off: the knob is agc_off_level. Writing it must emit the
    //    agc_off_level command and leave agc_threshold untouched.
    {
        SliceModel s(1);
        s.applyChanges(delta([](SliceDelta& d) {
            d.agcMode = QStringLiteral("off");
            d.agcThreshold = 50;
            d.agcOffLevel = 10;
        }));
        QSignalSpy cmds(&s, &SliceModel::commandReady);

        EXPECT_TRUE(s.agcTKnobUsesOffLevel(), "AGC off selects the off level");
        EXPECT_EQ(s.agcTKnobLevel(), 10);
        EXPECT_EQ(s.agcTKnobMinimum(), 0);
        EXPECT_EQ(s.agcTKnobMaximum(), 100);

        s.setAgcTKnobLevel(40);
        EXPECT_EQ(cmds.count(), 1);
        EXPECT_EQ(lastCommand(cmds), QStringLiteral("slice set 1 agc_off_level=40"));
        EXPECT_EQ(s.agcOffLevel(), 40);
        EXPECT_EQ(s.agcThreshold(), 50);
        EXPECT_EQ(s.agcTKnobLevel(), 40);
    }

    // 2. AGC slow / med / fast: the knob is agc_threshold. Writing it must
    //    emit the agc_threshold command and leave agc_off_level untouched.
    for (const char* mode : {"slow", "med", "fast"}) {
        SliceModel s(2);
        s.applyChanges(delta([mode](SliceDelta& d) {
            d.agcMode = QString::fromLatin1(mode);
            d.agcThreshold = 50;
            d.agcOffLevel = 10;
        }));
        QSignalSpy cmds(&s, &SliceModel::commandReady);

        EXPECT_TRUE(!s.agcTKnobUsesOffLevel(), "AGC on selects the threshold");
        EXPECT_EQ(s.agcTKnobLevel(), 50);
        EXPECT_EQ(s.agcTKnobMinimum(), 0);
        EXPECT_EQ(s.agcTKnobMaximum(), 100);

        s.setAgcTKnobLevel(30);
        EXPECT_EQ(cmds.count(), 1);
        EXPECT_EQ(lastCommand(cmds), QStringLiteral("slice set 2 agc_threshold=30"));
        EXPECT_EQ(s.agcThreshold(), 30);
        EXPECT_EQ(s.agcOffLevel(), 10);
        EXPECT_EQ(s.agcTKnobLevel(), 30);
    }

    // 3. Mode flips re-route the same knob; each property keeps its own value
    //    across the flip (the radio reports the mode change; the knob follows).
    {
        SliceModel s(3);
        s.applyChanges(delta([](SliceDelta& d) {
            d.agcMode = QStringLiteral("off");
            d.agcThreshold = 50;
            d.agcOffLevel = 10;
        }));
        QSignalSpy cmds(&s, &SliceModel::commandReady);

        s.setAgcTKnobLevel(40);                       // off  -> off level 40
        s.applyChanges(delta([](SliceDelta& d) { d.agcMode = QStringLiteral("slow"); }));
        EXPECT_EQ(s.agcTKnobLevel(), 50);               // knob now reads the threshold
        s.setAgcTKnobLevel(30);                       // slow -> threshold 30
        s.applyChanges(delta([](SliceDelta& d) { d.agcMode = QStringLiteral("off"); }));
        EXPECT_EQ(s.agcTKnobLevel(), 40);               // off level survived the detour

        EXPECT_EQ(cmds.count(), 2);
        EXPECT_EQ(cmds.at(0).at(0).toString(), QStringLiteral("slice set 3 agc_off_level=40"));
        EXPECT_EQ(cmds.at(1).at(0).toString(), QStringLiteral("slice set 3 agc_threshold=30"));
        EXPECT_EQ(s.agcOffLevel(), 40);
        EXPECT_EQ(s.agcThreshold(), 30);
    }

    // 4. External receiver replacing the slice audio (KiwiSDR): the threshold
    //    keeps the protocol's dB span, the off level keeps 0..100, and neither
    //    write reaches the Flex command stream.
    {
        SliceModel s(4);
        s.applyChanges(delta([](SliceDelta& d) {
            d.agcMode = QStringLiteral("slow");
            d.agcThreshold = 50;
            d.agcOffLevel = 10;
        }));
        s.setExternalReceiveAudioReplacementMute(true);
        EXPECT_TRUE(s.externalReceiveReplacementActive(), "external replacement is active");
        QSignalSpy cmds(&s, &SliceModel::commandReady);

        s.setAgcMode(QStringLiteral("off"));
        EXPECT_TRUE(s.agcTKnobUsesOffLevel(), "external AGC off selects the off level");
        EXPECT_EQ(s.agcTKnobMinimum(), 0);
        EXPECT_EQ(s.agcTKnobMaximum(), 100);
        const int before = cmds.count();
        s.setAgcTKnobLevel(40);
        EXPECT_EQ(cmds.count(), before);                  // external: no Flex command
        EXPECT_EQ(s.receiveAgcOffLevel(), 40);
        EXPECT_EQ(s.agcTKnobLevel(), 40);
        EXPECT_EQ(s.agcOffLevel(), 10);                   // the Flex value is untouched

        s.setAgcMode(QStringLiteral("slow"));
        EXPECT_TRUE(!s.agcTKnobUsesOffLevel(), "external AGC on selects the threshold");
        EXPECT_EQ(s.agcTKnobMinimum(), KiwiSdrProtocol::kAgcThresholdMinDb);
        EXPECT_EQ(s.agcTKnobMaximum(), KiwiSdrProtocol::kAgcThresholdMaxDb);
        const int before2 = cmds.count();
        s.setAgcTKnobLevel(-20);
        EXPECT_EQ(cmds.count(), before2);
        EXPECT_EQ(s.receiveAgcThreshold(), -20);
        EXPECT_EQ(s.agcTKnobLevel(), -20);
        EXPECT_EQ(s.agcThreshold(), 50);                  // the Flex value is untouched
    }

    if (g_failures) {
        std::fprintf(stderr, "agc_t_control_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("agc_t_control_test: all checks passed\n");
    return 0;
}
