# Applet picker and pinned meter

The sidebar starts with one fixed **Add:** row: a grouped applet selector,
an explicit **+** button, and the existing **LOCK** control. Categories and
full names come from the shared applet catalog. Non-selectable category
headers use bold primary text on the existing raised-surface theme token,
rather than the muted appearance of disabled applet entries. Open applets remain listed
as **open**, and hardware-dependent applets as **unavailable** until their
existing capability/presence checks allow them. Neither can be added twice.
Selecting a name does nothing until **+** is pressed; adding reveals its
header in the scroll area. Each applet's **×** hides it without clearing
its settings. Aetherial DSP stages remain managed inside the existing CHAIN
surface rather than becoming independent picker entries.

Existing applet visibility, order, float state, and hardware gates are
preserved. Legacy favorites/drawer preferences are left untouched for older
builds but no longer control the new picker. The S-Meter stays pinned;
its horizontal margins now follow the actual scroll viewport on either
dock side, including scrollbar appearance/disappearance. A floating or
canvas-placed S-Meter leaves no empty pinned placeholder.

Local validation at `0de5a2b6` with working-tree changes: the macOS Qt 6.12
application build and all five selected offscreen tests passed (four
chrome/connection tests plus `applet_picker_test`). Native disconnected
bridge checks covered explicit add, no selection side effect, close/re-add,
duplicate prevention, unavailable hardware, S-Meter close/re-add and float/dock,
left/right/floating panel alignment, dark/light themes, and the scrollbar-free
layout. Separate native dropdown captures verified the category headings.
No live-radio/TX testing or Windows/Linux certification is implied.

## Independent PR scope

The picker and pinned-meter alignment are independent of the Qt 6.12 window
chrome migration. The source branch retains main's Qt 6.8 minimum and does not
change window decorations or the radio switcher. The earlier combined-build
evidence above is not a substitute for validation of the standalone PR.
