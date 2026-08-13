#pragma once

// The workspace document — every workspace profile, the active selection, and
// the radio-profile bindings, as one versioned object (RFC #4887, phase 2).
//
// One document, one owner, one atomic write: Constitution Principle V (a
// feature owns its configuration as a single object) and Principle XIV
// (persist atomically).  That is the whole point of this type existing.  The
// arrangement it replaces is spread across a dozen independent settings keys
// with three different schemas and no single writer, so a shutdown path that
// skips one of them restores a MIXTURE of two layouts — which is what "the
// window rules never work" feels like from the operator's chair.
//
// Two things this schema deliberately separates:
//
//   * item rects are AUTHORITATIVE — fractions of their surface, restored
//     exactly, on every platform, every launch;
//   * `windowGeometry` on an extra canvas window is a HINT, and named like
//     one — the desktop may decline it, and always does on Wayland for
//     position.  A hint that is ignored costs one drag and loses no layout.
//
// Parsing is strict (Principle VII — untrusted input is validated at the
// boundary): the settings store is a file on disk that a user can edit, a
// backup can restore, and a future build can write a newer schema into.
//
// Widget-free, so the whole schema is unit-tested headless — see
// tests/workspace_document_test.cpp.

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

    // Refuses the last workspace (a document with none cannot describe the
    // shell).  Bindings pointing at it drop — the same rule the parser
    // applies to dangling targets — and if it was active, the first
    // remaining workspace becomes active.
    bool removeWorkspace(const QString& id);

    // ── Serialisation ────────────────────────────────────────────────────
    QJsonObject toJson() const;

    // Strict parse.  Returns false and leaves `out` untouched for anything
    // this build must not act on:
    //   * a missing or non-integer version,
    //   * a version NEWER than kSchemaVersion — a later build wrote it, and
    //     re-writing it from this one's understanding would destroy whatever
    //     it knew that we do not (AppSettings takes the same stance),
    //   * a root that is not an object, or carries no workspaces array.
    //
    // Recoverable damage inside a valid document is repaired rather than
    // rejected, and reported through `warnings` so it is visible instead of
    // silent: items with no id or an invalid rect are dropped, duplicate item
    // ids within a surface are dropped, a workspace with no "main" surface
    // gains an empty one, a binding pointing at a missing workspace is
    // dropped, and an activeWorkspace naming nothing falls back to the first.
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
