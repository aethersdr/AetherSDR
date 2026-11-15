#pragma once

// One-way migration from legacy layout keys to a "Classic" workspace document
// (RFC #4887). Reads once and deletes nothing: owners keep dual-writing the
// legacy keys so a downgrade still works. Keys read:
//   Applet_<ID>               "True"/"False"     AppletPanel
//   AppletOrder               comma-joined ids   AppletPanel
//   ButtonBarLayout           {"order":[],"hidden":[]}         AppletPanel
//   AppletPanelDockedLeft     "True"/"False"     MainWindow
//   AppletPanelVisible        "True"/"False"     MainWindow
//   AppletPanelFloatGeometry  base64             MainWindow
//   PanadapterLayout          layout id          MainWindow_Shortcuts
//   FloatingPanIds            comma-joined ids   PanadapterStack
//   ContainerTree             {version,containers{}}           ContainerManager
//   ContainerGeometry_<id>    base64             ContainerManager
//   ContainerAlwaysOnTop_<id> bool               ContainerManager
// Floating pans/containers stay floating and are not placed (pop-out stays,
// RFC decision 1); importing them is an explicit opt-in.

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
