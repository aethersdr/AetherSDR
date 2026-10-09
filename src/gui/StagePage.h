#pragma once

#include <QWidget>

namespace AetherSDR {

// Holds one stage panel and shrinks its graphics until it fits the page.
//
// These panels were drawn for windows twice the size of the tabbed ones, and
// they spend that space on knobs, curves and meters rather than on text. So
// the page keeps every label at the size it was meant to be read at and takes
// the graphics down instead (CompactMetrics), rather than scaling the panel
// whole and shrinking the type with it.
//
// Nothing scrolls: whatever is on this page is all of it.
QWidget* makeStagePage(QWidget* panel);

// The embedded panels each ship their own window chrome — a title bar with a
// min/max/close trio, and a legacy band colour from when they were floating
// editors. Inside a host window the trio does nothing, so strip it.
//
// `keepTitle` decides whether the name plate goes too. A page showing one
// panel has already named it in the tab, and a second copy of "EQ" above the
// graph is a row of pixels saying nothing; a page stacking two panels keeps
// both plates, because there the titles are what tell them apart.
void tidyEmbeddedPanel(QWidget* panel, bool keepTitle = false);

// The canon header AetherRX and AetherTX carry in place of a title bar, as
// Network Diagnostics and Waveforms do: the title in canon ink over a
// hairline, with a right margin that keeps it clear of CanonWindow's corner
// close button. The empty part of it is a handle for moving the window.
QWidget* makeCanonHeader(const QString& title);

// The sheet AetherRX and AetherTX put on CanonWindow's body: every container
// that paints nothing of its own stays transparent over the canon ground,
// instead of taking MainWindow's window-wide background rule, and text
// defaults to canon ink. Panels that set their own background keep it, and
// popups (menus, combo lists) keep a raised panel.
QString canonBodyStyleSheet();

} // namespace AetherSDR
