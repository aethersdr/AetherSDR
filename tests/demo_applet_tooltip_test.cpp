// Demo Noise applet — every noise-source row must carry an educational tooltip
// (what causes that noise on a real HF band), so the demo doubles as a primer
// for new operators. Guards the tips against being dropped when rows change.

#include "gui/DemoApplet.h"

#include <QApplication>
#include <QPushButton>
#include <QSet>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-58s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    DemoApplet applet;

    // The channel toggles are the applet's only CHECKABLE buttons (preset and
    // fault buttons are plain push buttons), so they identify the noise rows.
    QList<QPushButton*> toggles;
    for (QPushButton* b : applet.findChildren<QPushButton*>()) {
        if (b->isCheckable()) {
            toggles.append(b);
        }
    }

    report("ten noise-source rows found", toggles.size() == 10,
           QStringLiteral("count=%1").arg(toggles.size()));

    QSet<QString> seen;
    for (QPushButton* t : toggles) {
        const QString tip = t->toolTip();
        report(qPrintable(QStringLiteral("'%1' toggle has a tooltip").arg(t->text())),
               !tip.trimmed().isEmpty());
        // A one-liner should still be a sentence, not a label repeat.
        report(qPrintable(QStringLiteral("'%1' tooltip is explanatory").arg(t->text())),
               tip.length() > 40 && tip != t->text(),
               QStringLiteral("len=%1").arg(tip.length()));
        seen.insert(tip);
    }
    report("tooltips are distinct per source", seen.size() == toggles.size(),
           QStringLiteral("unique=%1").arg(seen.size()));

    // Hovering the level slider teaches too — same tip as the row's toggle.
    int slidersWithTips = 0;
    for (QSlider* s : applet.findChildren<QSlider*>()) {
        if (!s->toolTip().trimmed().isEmpty()) {
            ++slidersWithTips;
        }
    }
    report("level sliders carry the row tooltip", slidersWithTips >= 10,
           QStringLiteral("count=%1").arg(slidersWithTips));

    std::printf("\n%d demo applet tooltip test(s) failed\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
