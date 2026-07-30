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
    // setAutoSquelchActive() (the "second door" #1 could still leak
    // through: a radio-status echo of an Auto-computed level). While Auto
    // is active, the echoed level must not overwrite the manual memory;
    // once Auto ends, the operator's last manual choice must still be
    // there for RxApplet to restore.
    {
        SliceModel s(3);
        s.setManualSquelch(true, 45);
        EXPECT_EQ(s.manualSquelchLevel(), 45);

        s.setAutoSquelchActive(true);  // RxApplet transitions to Auto
        s.applyChanges(delta([](SliceDelta& d) {
            d.squelchOn = true; d.squelchLevel = 8;  // Auto's computed level, echoed back
        }));
        EXPECT_EQ(s.squelchLevel(), 8);
        EXPECT_EQ(s.manualSquelchLevel(), 45);  // must survive the Auto echo

        s.setAutoSquelchActive(false);  // RxApplet transitions back to Manual
        EXPECT_EQ(s.manualSquelchLevel(), 45);  // still the value to restore
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
    // level independently (m_externalReceiveSquelchLevel) — setManualSquelch
    // must not touch the Flex manual memory for them.
    {
        SliceModel s(5);
        s.setExternalReceiveAudioReplacementMute(true);
        const int before = s.manualSquelchLevel();
        s.setManualSquelch(true, 77);
        EXPECT_EQ(s.manualSquelchLevel(), before);
        EXPECT_EQ(s.receiveSquelchLevel(), 77);
    }

    if (g_failures == 0) {
        std::printf("slice_model_squelch_memory_test: all checks passed\n");
        return 0;
    }
    std::printf("slice_model_squelch_memory_test: %d failure(s)\n", g_failures);
    return 1;
}
