#pragma once

// One placed thing on a workspace canvas (RFC #4887, phase 1).
//
// Pure data, deliberately: the canvas model has to be reasoned about and
// tested without a QWidget in sight, and the same struct is what phase 2
// serialises into the workspace document.  There is no .cpp — this carries no
// behaviour beyond its members, matching the other pure-logic headers in
// src/gui (SpectrumPreviewLogic.h, CenterLockRebindTracker.h).

#include "gui/workspace/WorkspaceGeometry.h"

#include <QSize>
#include <QString>

namespace AetherSDR {

struct CanvasItem {
    // Stable identity, unique within one CanvasLayout.  The canvas never
    // invents these: they come from whatever is being placed, so a pan is
    // "pan:0x40000000" and an applet is "applet:RX" (phase 3+).
    QString id;

    // ContainerManager content-factory key, carried so phase 2 can rehydrate
    // this item's widget on restore.  Phase 1 stores it and does nothing with
    // it — the factory lookup arrives with the applets.
    QString contentType;

    // Placement, as a fraction of the canvas.  Always kept inside the surface
    // by CanvasLayout; see clampToCanvas().
    NormRect rect;

    // Stacking order.  CanvasLayout keeps these dense and contiguous over
    // [0, count-1], so "z + 1" is always the item directly above.
    int z = 0;

    // Smallest the item may be drawn, in pixels.  Enforced on every geometry
    // change so a panadapter cannot be squeezed to unreadability, and so an
    // item can never round down to a zero-size widget that still answers hit
    // tests.  The default is a floor, not a design opinion; real content
    // overrides it.
    QSize minimumSize{160, 90};

    // Collapsed to its title bar.  Phase 1 records the flag and leaves the
    // rect alone; the collapse animation and the height override belong with
    // the container work in phase 3.
    bool collapsed = false;

    // The item belongs to this workspace but its applet is CLOSED (phase 6,
    // full-recall ruling): switching to the workspace does not open it, and
    // close-keeps-home still works within a workspace.  Absent-when-false in
    // the stored form, so phase-3/4/5 documents parse unchanged and a
    // downgraded build merely drops a flag whose recall it does not do.
    // Meaningful for applet items only; pans are the surface and never close
    // this way.
    bool closed = false;
};

}  // namespace AetherSDR
