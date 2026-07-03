// Standalone test harness guarding the "New Panadapter" layout picker
// (PanLayoutDialog) against the Cancel button overlapping the thumbnail grid.
//
// The dialog used to pin its body to a fixed 560x502, but the 3-column grid of
// twelve 115 px-tall layout buttons plus the title and Cancel button needs far
// more than 502 px of vertical room. The fixed height clipped the layout, so
// the bottom-anchored Cancel button was drawn up over the last row of
// thumbnails. The fix lets the body height follow its content.
//
// Invariant under test: the Cancel button sits entirely below every layout
// thumbnail button (no vertical overlap), and stays within the dialog bounds.
//
// Build: CMake target `pan_layout_dialog_size_test`. Exit 0 = pass.

#include "gui/PanLayoutDialog.h"

#include <QApplication>
#include <QPushButton>
#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

// Top-left y of a descendant widget expressed in the dialog's own coordinates.
int topInRoot(QWidget* w, QWidget* root)
{
    return w->mapTo(root, QPoint(0, 0)).y();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("PanLayoutDialog Cancel-button overlap test harness\n\n");

    // maxPans = 8 enables every layout (the tallest grid — worst case).
    PanLayoutDialog dialog(/*maxPans*/ 8, /*currentLayout*/ "1");
    dialog.show();
    QApplication::processEvents();

    QPushButton* cancel = nullptr;
    std::vector<QPushButton*> thumbs;
    for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("Cancel")) {
            cancel = b;
        } else if (b->text().isEmpty() && b->height() >= 100) {
            // The layout thumbnails are empty-text buttons fixed at 130x115;
            // the frameless title bar's window buttons are shorter, so the
            // height filter keeps them out of the grid set.
            thumbs.push_back(b);
        }
    }

    report("Cancel button exists", cancel != nullptr);
    report("twelve layout thumbnail buttons present", thumbs.size() == 12,
           "count=" + std::to_string(thumbs.size()));
    if (!cancel || thumbs.empty()) {
        dialog.hide();
        return 1;
    }

    // Bottom edge of the lowest thumbnail button, in dialog coordinates.
    int gridBottom = 0;
    for (QPushButton* b : thumbs)
        gridBottom = std::max(gridBottom, topInRoot(b, &dialog) + b->height());

    const int cancelTop = topInRoot(cancel, &dialog);
    const int cancelBottom = cancelTop + cancel->height();

    report("Cancel button starts below the thumbnail grid (no overlap)",
           cancelTop >= gridBottom,
           "cancelTop=" + std::to_string(cancelTop)
               + " gridBottom=" + std::to_string(gridBottom));

    report("Cancel button stays within the dialog bounds",
           cancelBottom <= dialog.height(),
           "cancelBottom=" + std::to_string(cancelBottom)
               + " dialogH=" + std::to_string(dialog.height()));

    dialog.hide();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
