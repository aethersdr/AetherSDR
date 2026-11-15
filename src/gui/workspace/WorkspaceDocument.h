#pragma once

// The workspace document: all workspace profiles, the active selection and the
// radio-profile bindings as one versioned object, written atomically by one
// owner (RFC #4887), so a partial shutdown can't restore a mix of two layouts.
// Item rects are authoritative (surface fractions, restored exactly);
// `windowGeometry` on extra windows is only a hint (Wayland ignores position).
// Parsing is strict because the store is user-editable on disk and may hold a
// newer schema. Widget-free; see tests/workspace_document_test.cpp.

#include "gui/workspace/CanvasItem.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace AetherSDR {

// One canvas surface: the main window's, or one extra canvas window.
struct WorkspaceSurface {
    // "main" is reserved for the main window's canvas and must be present.
    static const QString kMainId;

    QString id;
    QString label;

    // Placement hint for an extra canvas window, as QWidget::saveGeometry()
    // wrote it.  Empty for "main", whose geometry is the main window's own
    // business.  Never authoritative — see the file comment.
    QByteArray windowGeometry;

    // An extra canvas window the operator closed (hide-and-keep, phase 7
    // maintainer ruling): the surface and its items stay recorded, the
    // window just does not open until asked.  Meaningless for "main".
    // Absent-when-false in the stored form, so phase-6 documents parse
    // unchanged and a downgrade merely drops the flag.
    bool hidden = false;

    QList<CanvasItem> items;
};

// One named arrangement — a workspace profile.  NOT a radio profile: those
// live on the radio and know nothing about layout (Principle III does not
// reach a window arrangement).  See the RFC for the full distinction.
struct Workspace {
    QString id;
    QString label;
    QList<WorkspaceSurface> surfaces;

    const WorkspaceSurface* surface(const QString& surfaceId) const;
};

class WorkspaceDocument {
public:
    // Bumped only for a change this build cannot read back compatibly.
    static constexpr int kSchemaVersion = 1;

    // Workspaces are an ORDERED array rather than the object the RFC sketched:
    // QJsonObject sorts its keys, which would silently reorder the operator's
    // switcher every time the document round-tripped.
    QList<Workspace> workspaces;

    // Radio global profile name -> workspace id.  Client-side, because the
    // radio has nowhere to put it and no concept of what it points at.
    QMap<QString, QString> bindings;

    QString activeWorkspace;

    // Whether canvas mode is switched on (View menu).  Lives here rather
    // than in a flat AppSettings key because the workspace document is the
    // feature's single configuration object (Principle V) — the toggle is as
    // much workspace state as the placement it reveals.  Optional in the
    // stored form and absent when false, so phase-2 documents parse
    // unchanged and a downgraded build that rewrites the document merely
    // drops a flag whose feature it does not have.
    bool canvasEnabled = false;

    // ── Queries ──────────────────────────────────────────────────────────
    const Workspace* workspace(const QString& id) const;
    bool contains(const QString& id) const { return workspace(id) != nullptr; }
    QStringList workspaceIds() const;
    bool isEmpty() const { return workspaces.isEmpty(); }

    // The workspace bound to a radio global profile, or empty when unbound.
    // An unbound recall must leave the workspace alone (RFC decision 8), so
    // "no binding" is a normal answer, not a failure.
    QString boundWorkspace(const QString& radioProfile) const;

    // ── Workspace CRUD (phase 6) ─────────────────────────────────────────
    //
    // The DOCUMENT owns identity: sequential unique ids ("ws-N") and
    // de-duplicated labels ("CW Contest (2)"), so no two call sites can
    // mint colliding workspaces.  Composing CONTENT for a new workspace
    // (Classic cells, live pan slots) is the controller's business — the
    // document only clones or creates empty.
    QString uniqueWorkspaceId() const;
    QString uniqueLabel(const QString& base) const;

    // Append a clone of `sourceId` — surfaces, items, rects, z, closed
    // flags, the whole arrangement — under a fresh id.  Returns the new id,
    // empty when the source does not exist.
    QString addDuplicateOf(const QString& sourceId, const QString& label);

    // Append an empty workspace (a main surface, no items).
    QString addBlank(const QString& label);

    bool renameWorkspace(const QString& id, const QString& label);

    // ── Surface CRUD (phase 7 — additional canvas windows) ──────────────
    //
    // Same ownership rule as workspace CRUD: the document owns identity
    // (sequential unique ids "canvas2", "canvas3", … and per-workspace
    // de-duplicated labels); which WINDOW realises a surface is the
    // controller's business.
    QString uniqueSurfaceId(const QString& workspaceId) const;

    // Append an empty extra surface to `workspaceId`.  Returns the new
    // surface id, empty when the workspace does not exist.
    QString addSurface(const QString& workspaceId, const QString& label);

    // Refuses "main".  The surface's items move to the END of the main
    // surface (identity is workspace-wide, so no collision is possible);
    // the caller re-places them.
    bool removeSurface(const QString& workspaceId, const QString& surfaceId);

    bool renameSurface(const QString& workspaceId, const QString& surfaceId,
                       const QString& label);

    // Refuses the last workspace (a document with none cannot describe the
    // shell).  Bindings pointing at it drop — the same rule the parser
    // applies to dangling targets — and if it was active, the first
    // remaining workspace becomes active.
    bool removeWorkspace(const QString& id);

    // ── Serialisation ────────────────────────────────────────────────────
    QJsonObject toJson() const;

    // Strict parse: returns false and leaves `out` untouched for a missing or
    // non-integer version, a version newer than kSchemaVersion (rewriting would
    // destroy what a later build knew), or a root without a workspaces array.
    // Recoverable damage is repaired and reported via `warnings`: bad/duplicate
    // items dropped, missing "main" surface added, dangling bindings dropped, an
    // unknown activeWorkspace falls back to the first.
    static bool fromJson(const QJsonObject& root,
                         WorkspaceDocument* out,
                         QString* error = nullptr,
                         QStringList* warnings = nullptr);

    // Convenience for the settings round-trip: parse a stored UTF-8 blob.
    static bool fromStoredJson(const QByteArray& blob,
                               WorkspaceDocument* out,
                               QString* error = nullptr,
                               QStringList* warnings = nullptr);
    QByteArray toStoredJson() const;
};

}  // namespace AetherSDR
