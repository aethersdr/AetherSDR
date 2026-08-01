#include "models/SliceModel.h"
#include "core/backends/SliceDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
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

// Mirrors slice_model_letter_test.cpp's helper for building a SliceDelta.
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

    // ── setManualSquelch() is the manual-intent entry point (#4592, fixing
    // the cross-slice leak #4461 left behind): it must move both the live
    // level and the remembered manual level together, so no caller can push
    // one without the other.
    {
        SliceModel s(1);
        QStringList commands;
        QObject::connect(&s, &SliceModel::commandReady,
                         [&commands](const QString& cmd) { commands.append(cmd); });
        s.setManualSquelch(true, 45);
        EXPECT_EQ(s.squelchLevel(), 45);
        EXPECT_EQ(s.manualSquelchLevel(), 45);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 1 squelch=1|slice set 1 squelch_level=45"));
    }

    // ── Plain setSquelch() (the shape Auto-mode call sites use) must NOT
    // touch the manual memory. This is the guard against reintroducing the
    // regression the issue's originally suggested fix would have caused:
    // writing m_manualSquelchLevel unconditionally inside setSquelch()
    // would let Auto-mode's per-tick computed levels overwrite the
    // operator's last manual choice.
    {
        SliceModel s(2);
        s.setManualSquelch(true, 45);
        EXPECT_EQ(s.manualSquelchLevel(), 45);
        s.setSquelch(true, 12);  // simulates an Auto-mode push
        EXPECT_EQ(s.squelchLevel(), 12);
        EXPECT_EQ(s.manualSquelchLevel(), 45);
    }

    // ── Manual -> Auto -> Manual round-trip via applyChanges(), gated by
    // setSquelchEchoIsManual() (the "second door" #1 could still leak
    // through: a radio-status echo of an Auto-computed level). While Auto
    // is active, the echoed level must not overwrite the manual memory;
    // once Auto ends, the operator's last manual choice must still be
    // there for RxApplet to restore.
    {
        SliceModel s(3);
        s.setManualSquelch(true, 45);
        EXPECT_EQ(s.manualSquelchLevel(), 45);

        s.setSquelchEchoIsManual(false);  // RxApplet transitions to Auto
        s.applyChanges(delta([](SliceDelta& d) {
            d.squelchOn = true; d.squelchLevel = 8;  // Auto's computed level, echoed back
        }));
        EXPECT_EQ(s.squelchLevel(), 8);
        EXPECT_EQ(s.manualSquelchLevel(), 45);  // must survive the Auto echo

        s.setSquelchEchoIsManual(true);  // RxApplet transitions back to Manual
        EXPECT_EQ(s.manualSquelchLevel(), 45);  // still the value to restore

        // Re-opening the gate must genuinely re-enable echo-driven updates,
        // not just leave the old value sitting there unchanged — otherwise
        // a slice stuck with the gate never re-opened (the bug review found
        // in RxApplet::disconnectSlice(), which now restores it) would look
        // identical to this test up to this point. A fresh echo at a new
        // level proves the gate is actually open again.
        s.applyChanges(delta([](SliceDelta& d) {
            d.squelchOn = true; d.squelchLevel = 60;
        }));
        EXPECT_EQ(s.manualSquelchLevel(), 60);
    }

    // ── The full SQL mode cycle, driven the way RxApplet drives it.
    //
    // The gate above is only as good as the mode machine that sets it, and
    // that machine is the part a SliceModel-only test can't see: an earlier
    // version of this fix gated on "is Auto active" at the Auto edges only,
    // which left the gate OPEN across Auto->Off — the leg every return from
    // Auto to Manual passes through, since cycleSqlMode() runs
    // Off->Manual->Auto->Off. The Off push then supplied a level the
    // operator never chose and the echo adopted it as the manual memory.
    //
    // SqlModeMachine mirrors the two lines of RxApplet::setSqlMode() that
    // matter (gate, then propagate) so that regression is pinned here rather
    // than only in a GUI target this test can't link. The manual level (45)
    // is deliberately NOT equal to the Auto margin (10): with the two equal,
    // a clobbered memory and a correct one are indistinguishable, which is
    // exactly why the original bug survived both review and a live radio test.
    {
        enum class SqlMode { Off, Manual, Auto };
        struct SqlModeMachine {
            SliceModel* slice;
            SqlMode     mode = SqlMode::Off;
            int         marginDb = 10;      // AppSettings "AutoSqlMarginDb"

            void setSqlMode(SqlMode m) {
                if (m == mode) return;
                mode = m;
                // RxApplet::setSqlMode() — re-gate on every mode change.
                slice->setSquelchEchoIsManual(m == SqlMode::Manual);
                // …then push to the radio; only Auto sends the dB margin.
                const bool on = (m != SqlMode::Off);
                const int level = (m == SqlMode::Auto)
                    ? marginDb
                    : slice->manualSquelchLevel();
                slice->setSquelch(on, level);
                // The radio echoes back what it now holds.
                SliceDelta d;
                d.squelchOn = on;
                d.squelchLevel = slice->squelchLevel();
                slice->applyChanges(d);
            }
            // RxApplet::cycleSqlMode(): Off -> Manual -> Auto -> Off.
            void cycle() {
                setSqlMode(mode == SqlMode::Off    ? SqlMode::Manual
                         : mode == SqlMode::Manual ? SqlMode::Auto
                                                   : SqlMode::Off);
            }
        };

        SliceModel s(6);
        SqlModeMachine rx{&s};

        rx.setSqlMode(SqlMode::Manual);
        s.setManualSquelch(true, 45);            // operator drags the slider
        EXPECT_EQ(s.manualSquelchLevel(), 45);

        rx.cycle();                              // Manual -> Auto
        s.setSquelch(true, 8);                   // an algorithm tick
        s.applyChanges(delta([](SliceDelta& d) { d.squelchOn = true; d.squelchLevel = 8; }));
        EXPECT_EQ(s.manualSquelchLevel(), 45);

        rx.cycle();                              // Auto -> Off
        EXPECT_EQ(s.manualSquelchLevel(), 45);   // the leg that used to clobber

        rx.cycle();                              // Off -> Manual
        EXPECT_EQ(s.manualSquelchLevel(), 45);
        EXPECT_EQ(s.squelchLevel(), 45);         // and the radio gets it back
    }

    // ── A genuine echo while NOT in Auto (the operator's own edit reaching
    // back through radio status, another Multi-Flex client, or session
    // restore) DOES update the manual memory — the other half of the #1
    // fix, for the path the issue's repro didn't cover but the bot's
    // review found in applyChanges().
    {
        SliceModel s(4);
        s.applyChanges(delta([](SliceDelta& d) {
            d.squelchOn = true; d.squelchLevel = 30;
        }));
        EXPECT_EQ(s.manualSquelchLevel(), 30);
    }

    // ── External-receive-replacement (KiwiSDR) slices manage their own
    // level independently (m_externalReceiveSquelchLevel) — neither
    // setManualSquelch() nor the applyChanges() echo path may touch the Flex
    // manual memory for them. Both halves of that exclusion are asserted:
    // they disagreed once, and only the setManualSquelch half was covered.
    {
        SliceModel s(5);
        s.setExternalReceiveAudioReplacementMute(true);
        const int before = s.manualSquelchLevel();
        s.setManualSquelch(true, 77);
        EXPECT_EQ(s.manualSquelchLevel(), before);
        EXPECT_EQ(s.receiveSquelchLevel(), 77);

        // The underlying Flex slice keeps reporting its own squelch_level
        // while Kiwi audio replaces its receive path; that echo is about a
        // level the operator isn't looking at on this surface.
        s.applyChanges(delta([](SliceDelta& d) {
            d.squelchOn = true; d.squelchLevel = 33;
        }));
        EXPECT_EQ(s.manualSquelchLevel(), before);
    }

    if (g_failures == 0) {
        std::printf("slice_model_squelch_memory_test: all checks passed\n");
        return 0;
    }
    std::printf("slice_model_squelch_memory_test: %d failure(s)\n", g_failures);
    return 1;
}
