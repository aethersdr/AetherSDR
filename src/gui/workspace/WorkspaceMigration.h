#pragma once

// One-way migration from the legacy layout keys to a workspace document
// (RFC #4887, phase 2).
//
// Today's arrangement is spread across a dozen independent AppSettings keys
// with three different schemas and no single writer.  This reads them once,
// builds the "Classic" workspace, and hands it back.  It does not delete
// anything: the legacy keys keep being written by their current owners for a
// release (dual-write), so a downgrade still finds the state it expects.
//
// THE KEYS, as they actually exist in the code today — the RFC's table was
// written from a grep and is looser than this:
//
//   Applet_<ID>               "True"/"False"     AppletPanel   (26 of them)
//   AppletOrder               comma-joined ids   AppletPanel
//   ButtonBarLayout           {"order":[],"hidden":[]}         AppletPanel
//   AppletPanelDockedLeft     "True"/"False"     MainWindow
//   AppletPanelVisible        "True"/"False"     MainWindow
//   AppletPanelFloatGeometry  base64             MainWindow
//   PanadapterLayout          layout id          MainWindow_Shortcuts
//   FloatingPanIds            comma-joined ids   PanadapterStack
//   ContainerTree             {version,containers{}}           ContainerManager
//   ContainerGeometry_<id>    base64             ContainerManager (per container)
//   ContainerAlwaysOnTop_<id> bool               ContainerManager (per container)
//
// WHAT MIGRATION DELIBERATELY DOES NOT DO: it does not pull floating things
// onto the canvas.  Pop-out stays (RFC decision 1), so a pan or container the
// operator floated is left floating and is not placed in Classic.  Dragging
// someone's pop-outs into a canvas they have not opted into would be a
// user-visible change in a phase whose whole contract is that there is none.
// Phase 6's "import current pop-out arrangement" is the opt-in that does it.

#include "gui/workspace/WorkspaceDocument.h"

#include <QString>
#include <QStringList>

namespace AetherSDR {

// What the legacy keys said, gathered in one place so the reading and the
// composing can be tested apart.
struct LegacyLayoutState {
    QString     panLayoutId{QStringLiteral("1")};
    QStringList floatingPanIds;     // left floating, not placed
    QStringList openAppletIds;      // in column order, floats excluded
    QStringList floatingAppletIds;  // left floating, not placed
    bool        appletsLeft{false};
    bool        appletPanelVisible{true};
};

// Read the legacy keys out of AppSettings.
//
// `knownAppletIds` is the applet id set to consider — AppletPanel owns the
// canonical list, and this stays out of that business rather than duplicating
// 26 string literals that would rot the first time an applet is added.
LegacyLayoutState readLegacyLayoutState(const QStringList& knownAppletIds);

// Build the Classic workspace document from gathered legacy state.  Pure.
//
// `panIds` are the pans that exist right now; migration runs at startup
// before a radio is connected, so this is usually empty and Classic carries
// only the applet column until phase 3 places pans as they arrive.
WorkspaceDocument buildClassicDocument(const LegacyLayoutState& legacy,
                                       const QStringList& panIds);

// The workspace id and label the migration produces.
QString classicWorkspaceId();

}  // namespace AetherSDR
