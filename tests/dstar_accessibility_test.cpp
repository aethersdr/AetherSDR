#include "gui/DStarAccessibility.h"

#include <QAccessible>
#include <QApplication>
#include <QLabel>

#include <cstdio>

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QLabel label;

    AetherSDR::updateDStarSliceStateLabel(
        &label, QStringLiteral("No DSTR slice"));
    QAccessibleInterface* accessible =
        QAccessible::queryAccessibleInterface(&label);
    report("D-STAR slice label has an accessible interface", accessible != nullptr);
    if (!accessible) {
        return 1;
    }
    report("empty slice state is exposed in the accessible name",
           accessible->text(QAccessible::Name)
               == QStringLiteral("D-STAR slice: No DSTR slice"));

    AetherSDR::updateDStarSliceStateLabel(
        &label, QStringLiteral("Slice A  14.100.000 MHz"));
    report("controlled slice state updates the visible label",
           label.text() == QStringLiteral("Slice A  14.100.000 MHz"));
    report("controlled slice state updates the accessible name",
           accessible->text(QAccessible::Name)
               == QStringLiteral("D-STAR slice: Slice A  14.100.000 MHz"));

    return g_failed == 0 ? 0 : 1;
}
