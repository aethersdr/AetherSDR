// The Customize Button Bar lists draw selection from the palette, not as a
// fixed colour on the theme's accent (follow-up to #6312).
#include "TestSettingsProfile.h"
#include "core/ThemeManager.h"
#include "gui/FavoritesPickerDialog.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QListWidget>

#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("favorites_picker_selection_test"));
    QApplication app(argc, argv);

    const QList<FavoritesPickerDialog::Entry> entries{
        {QStringLiteral("a"), QStringLiteral("AAA"), {}},
        {QStringLiteral("b"), QStringLiteral("BBB"), {}},
        {QStringLiteral("c"), QStringLiteral("CCC"), {}},
        {QStringLiteral("d"), QStringLiteral("DDD"), {}},
    };
    // Split after the last row so no divider is drawn in the sampled rows.
    FavoritesPickerDialog dlg(entries,
                              {QStringLiteral("a"), QStringLiteral("b"),
                               QStringLiteral("c"), QStringLiteral("d")},
                              {}, 4);
    dlg.show();
    QApplication::processEvents();

    QListWidget* active = nullptr;
    for (QListWidget* list : dlg.findChildren<QListWidget*>()) {
        if (list->count() == entries.size())
            active = list;
    }
    if (!active) {
        std::fprintf(stderr, "FAIL: Active list not found\n");
        return 1;
    }

    active->setCurrentRow(1);
    QApplication::processEvents();

    const QRect row = active->visualItemRect(active->item(1));
    const QColor selected = active->viewport()->grab().toImage().pixelColor(
        row.right() - 4, row.center().y());
    const QColor accent = ThemeManager::instance().color(QStringLiteral("color.accent"));

    int failures = 0;
    if (selected.rgb() == accent.rgb()) {
        std::fprintf(stderr, "FAIL: selected row is drawn in color.accent (%s)\n",
                     qPrintable(accent.name()));
        ++failures;
    }
    const QPalette pal = active->palette();
    if (selected.rgb() != pal.color(QPalette::Active, QPalette::Highlight).rgb()
        && selected.rgb() != pal.color(QPalette::Inactive, QPalette::Highlight).rgb()) {
        std::fprintf(stderr, "FAIL: selected row %s is not the palette highlight %s\n",
                     qPrintable(selected.name()),
                     qPrintable(pal.color(QPalette::Highlight).name()));
        ++failures;
    }

    if (failures == 0)
        std::printf("favorites_picker_selection_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
