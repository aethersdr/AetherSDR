// Unit tests for CwxModel::expandSpeedModifiers — the pure expansion function
// that parses inline +/- speed modifier prefixes from CW macro / send text.
// Run: ./build/cwx_speed_modifier_test

#include "models/CwxModel.h"
#include <QCoreApplication>
#include <cstdio>
#include <string>

using namespace AetherSDR;
using Segs = QVector<CwxModel::SpeedSegment>;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-64s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

void check(const char* name, const Segs& got, const Segs& want)
{
    if (got == want) { report(name, true); return; }
    std::string d = "got[";
    for (const auto& s : got)
        d += "(" + s.text.toStdString() + "@" + std::to_string(s.wpm) + ")";
    d += "] want[";
    for (const auto& s : want)
        d += "(" + s.text.toStdString() + "@" + std::to_string(s.wpm) + ")";
    d += "]";
    report(name, false, d);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("CWX speed modifier expansion tests\n\n");

    // ── No modifiers ──────────────────────────────────────────────────────
    check("no modifiers: passthrough as single segment",
          CwxModel::expandSpeedModifiers("CQ TEST", 20, 3),
          Segs{{"CQ TEST", 20}});

    check("empty string",
          CwxModel::expandSpeedModifiers("", 20, 3),
          Segs{});

    // ── Reporter's exact example ──────────────────────────────────────────
    check("reporter example: +ur 5nn 5nn -1234NH ++73",
          CwxModel::expandSpeedModifiers("+ur 5nn 5nn -1234NH ++73", 20, 3),
          Segs{{"ur", 23}, {" 5nn 5nn ", 20}, {"1234NH", 17}, {" ", 20}, {"73", 26}});

    // ── Single-word modifiers ─────────────────────────────────────────────
    check("single +word: sends word at base+step",
          CwxModel::expandSpeedModifiers("+CQ", 20, 3),
          Segs{{"CQ", 23}});

    check("single -word: sends word at base-step",
          CwxModel::expandSpeedModifiers("-CQ", 20, 3),
          Segs{{"CQ", 17}});

    check("double ++word: base + 2*step",
          CwxModel::expandSpeedModifiers("++CQ", 20, 3),
          Segs{{"CQ", 26}});

    check("double --word: base - 2*step",
          CwxModel::expandSpeedModifiers("--CQ", 20, 3),
          Segs{{"CQ", 14}});

    // ── Speed reset semantics ─────────────────────────────────────────────
    check("speed resets to base after modified word",
          CwxModel::expandSpeedModifiers("+CQ de", 20, 3),
          Segs{{"CQ", 23}, {" de", 20}});

    check("unmodified prefix, then modifier",
          CwxModel::expandSpeedModifiers("de W1AW +CQ", 20, 3),
          Segs{{"de W1AW ", 20}, {"CQ", 23}});

    // ── Prosign / hyphen disambiguation ───────────────────────────────────
    check("standalone + (AR prosign) — not a modifier",
          CwxModel::expandSpeedModifiers("+", 20, 3),
          Segs{{"+", 20}});

    check("trailing + (AR prosign) stays in word body",
          CwxModel::expandSpeedModifiers("73+", 20, 3),
          Segs{{"73+", 20}});

    check("+ followed by space = prosign, not modifier",
          CwxModel::expandSpeedModifiers("+ ur", 20, 3),
          Segs{{"+ ur", 20}});

    check("standalone - (hyphen) — not a modifier",
          CwxModel::expandSpeedModifiers("-", 20, 3),
          Segs{{"-", 20}});

    check("mid-word + stays as prosign",
          CwxModel::expandSpeedModifiers("de+w1aw", 20, 3),
          Segs{{"de+w1aw", 20}});

    // ── WPM clamping ─────────────────────────────────────────────────────
    check("wpm floor at 5",
          CwxModel::expandSpeedModifiers("-word", 5, 3),
          Segs{{"word", 5}});

    check("wpm ceiling at 100",
          CwxModel::expandSpeedModifiers("+word", 100, 3),
          Segs{{"word", 100}});

    check("large step: clamp at 100",
          CwxModel::expandSpeedModifiers("+word", 95, 10),
          Segs{{"word", 100}});

    check("large negative step: clamp at 5",
          CwxModel::expandSpeedModifiers("-word", 10, 10),
          Segs{{"word", 5}});

    // ── step=1 boundary ───────────────────────────────────────────────────
    check("step=1 single modifier",
          CwxModel::expandSpeedModifiers("+CQ", 20, 1),
          Segs{{"CQ", 21}});

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
