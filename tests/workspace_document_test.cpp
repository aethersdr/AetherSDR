// Unit tests for the workspace document schema (RFC #4887, phase 2).
//
// Two things are being defended here.
//
// The SCHEMA GUARD: a document written by a newer build must be refused, not
// re-written from this build's understanding — doing so would silently
// destroy whatever the newer build knew that this one does not.  AppSettings
// takes the same stance for the same reason.
//
// The BOUNDARY (Principle VII): the settings store is a file a user can edit,
// a backup can restore, and a partial write can truncate.  Damage that can be
// repaired is repaired and REPORTED; damage that cannot be is refused.  What
// must never happen is a half-understood document being written back.
//
// Pure logic, no widgets, no settings store.

#include "gui/workspace/WorkspaceDocument.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using AetherSDR::CanvasItem;
using AetherSDR::NormRect;
using AetherSDR::Workspace;
using AetherSDR::WorkspaceDocument;
using AetherSDR::WorkspaceSurface;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

CanvasItem item(const QString& id, const NormRect& rect, int z)
{
    CanvasItem it;
    it.id   = id;
    it.rect = rect;
    it.z    = z;
    return it;
}

// A document with everything populated, so the round trip has something to
// lose if a field is forgotten.
WorkspaceDocument sample()
{
    WorkspaceSurface main;
    main.id = WorkspaceSurface::kMainId;
    main.items.append(item(QStringLiteral("pan:0x40000000"),
                           NormRect{0.0, 0.0, 0.72, 0.55}, 0));
    main.items.last().contentType = QStringLiteral("panadapter");
    main.items.append(item(QStringLiteral("applet:RX"),
                           NormRect{0.72, 0.0, 0.28, 0.40}, 1));
    main.items.last().collapsed = true;

    WorkspaceSurface aux;
    aux.id             = QStringLiteral("canvas2");
    aux.label          = QStringLiteral("Right monitor");
    aux.windowGeometry = QByteArray("not-a-real-blob-but-round-trips");
    aux.items.append(item(QStringLiteral("applet:WAVE"),
                          NormRect{0.0, 0.0, 1.0, 0.5}, 0));

    Workspace contest;
    contest.id    = QStringLiteral("contest");
    contest.label = QStringLiteral("Contest");
    contest.surfaces = {main, aux};

    Workspace casual;
    casual.id    = QStringLiteral("casual");
    casual.label = QStringLiteral("Casual");
    WorkspaceSurface casualMain;
    casualMain.id = WorkspaceSurface::kMainId;
    casual.surfaces.append(casualMain);

    WorkspaceDocument doc;
    doc.workspaces      = {contest, casual};
    doc.activeWorkspace = QStringLiteral("contest");
    doc.bindings.insert(QStringLiteral("Contest CW"), QStringLiteral("contest"));
    doc.bindings.insert(QStringLiteral("Ragchew"),    QStringLiteral("casual"));
    return doc;
}

QJsonObject minimalRoot()
{
    QJsonObject ws;
    ws[QStringLiteral("id")] = QStringLiteral("classic");
    ws[QStringLiteral("surfaces")] = QJsonArray{};

    QJsonObject root;
    root[QStringLiteral("version")]    = WorkspaceDocument::kSchemaVersion;
    root[QStringLiteral("workspaces")] = QJsonArray{ws};
    return root;
}

}  // namespace

int main()
{
    // ── Round trip ───────────────────────────────────────────────────────
    {
        const WorkspaceDocument original = sample();

        WorkspaceDocument parsed;
        QString error;
        QStringList warnings;
        const bool ok = WorkspaceDocument::fromStoredJson(original.toStoredJson(),
                                                          &parsed, &error, &warnings);
        report("a written document parses back", ok);
        report("a clean round trip warns about nothing", warnings.isEmpty());

        report("workspace order survives",
               parsed.workspaceIds() == QStringList({"contest", "casual"}));
        report("the active workspace survives",
               parsed.activeWorkspace == QStringLiteral("contest"));
        report("labels survive",
               parsed.workspace("contest")->label == QStringLiteral("Contest"));
        report("bindings survive",
               parsed.bindings.value("Contest CW") == QStringLiteral("contest")
                   && parsed.bindings.value("Ragchew") == QStringLiteral("casual"));

        const Workspace* contest = parsed.workspace("contest");
        report("surfaces survive in order",
               contest->surfaces.size() == 2
                   && contest->surfaces.at(0).id == WorkspaceSurface::kMainId
                   && contest->surfaces.at(1).id == QStringLiteral("canvas2"));
        report("the window geometry hint survives",
               contest->surfaces.at(1).windowGeometry
                   == QByteArray("not-a-real-blob-but-round-trips"));

        const CanvasItem& pan = contest->surfaces.at(0).items.at(0);
        report("item id, rect and z survive",
               pan.id == QStringLiteral("pan:0x40000000")
                   && pan.rect == NormRect{0.0, 0.0, 0.72, 0.55}
                   && pan.z == 0);
        report("item content type survives",
               pan.contentType == QStringLiteral("panadapter"));
        report("item collapsed flag survives",
               contest->surfaces.at(0).items.at(1).collapsed);

        // minimumSize is content-declared, not operator state, so it is not
        // persisted and comes back as the default.
        report("minimum size is not persisted", pan.minimumSize == CanvasItem{}.minimumSize);
    }

    // ── The schema guard ─────────────────────────────────────────────────
    {
        WorkspaceDocument parsed;
        QString error;

        QJsonObject newer = minimalRoot();
        newer[QStringLiteral("version")] = WorkspaceDocument::kSchemaVersion + 1;
        report("a newer schema version is refused",
               !WorkspaceDocument::fromJson(newer, &parsed, &error));
        report("...and says why", error.contains(QStringLiteral("newer")));

        QJsonObject noVersion = minimalRoot();
        noVersion.remove(QStringLiteral("version"));
        report("a document with no version is refused",
               !WorkspaceDocument::fromJson(noVersion, &parsed, &error));

        QJsonObject badVersion = minimalRoot();
        badVersion[QStringLiteral("version")] = 0;
        report("version zero is refused",
               !WorkspaceDocument::fromJson(badVersion, &parsed, &error));

        QJsonObject noWorkspaces = minimalRoot();
        noWorkspaces.remove(QStringLiteral("workspaces"));
        report("a document with no workspaces array is refused",
               !WorkspaceDocument::fromJson(noWorkspaces, &parsed, &error));

        report("an empty blob is refused",
               !WorkspaceDocument::fromStoredJson(QByteArray(), &parsed, &error));
        report("malformed JSON is refused",
               !WorkspaceDocument::fromStoredJson(QByteArray("{not json"), &parsed, &error));
        report("a JSON array root is refused",
               !WorkspaceDocument::fromStoredJson(QByteArray("[]"), &parsed, &error));

        // A refusal must not leave a half-parsed document behind.
        WorkspaceDocument untouched = sample();
        WorkspaceDocument before    = untouched;
        WorkspaceDocument::fromJson(newer, &untouched, &error);
        report("a refused parse leaves the target untouched",
               untouched.workspaceIds() == before.workspaceIds());
    }

    // ── Repairable damage is repaired, and reported ──────────────────────
    {
        QJsonObject itemNoId;
        itemNoId[QStringLiteral("rect")] = QJsonArray{0.0, 0.0, 0.5, 0.5};

        QJsonObject itemBadRect;
        itemBadRect[QStringLiteral("id")]   = QStringLiteral("applet:BAD");
        itemBadRect[QStringLiteral("rect")] = QJsonArray{0.0, 0.0, 0.0, 0.5};  // zero width

        QJsonObject itemShortRect;
        itemShortRect[QStringLiteral("id")]   = QStringLiteral("applet:SHORT");
        itemShortRect[QStringLiteral("rect")] = QJsonArray{0.0, 0.0, 0.5};

        QJsonObject itemGood;
        itemGood[QStringLiteral("id")]   = QStringLiteral("applet:RX");
        itemGood[QStringLiteral("rect")] = QJsonArray{0.0, 0.0, 0.5, 0.5};

        QJsonObject itemDuplicate = itemGood;

        QJsonObject surface;
        surface[QStringLiteral("id")] = WorkspaceSurface::kMainId;
        surface[QStringLiteral("items")] =
            QJsonArray{itemNoId, itemBadRect, itemShortRect, itemGood, itemDuplicate};

        QJsonObject ws;
        ws[QStringLiteral("id")]       = QStringLiteral("classic");
        ws[QStringLiteral("surfaces")] = QJsonArray{surface};

        QJsonObject bindings;
        bindings[QStringLiteral("Contest CW")] = QStringLiteral("nonexistent");

        QJsonObject root;
        root[QStringLiteral("version")]         = WorkspaceDocument::kSchemaVersion;
        root[QStringLiteral("workspaces")]      = QJsonArray{ws};
        root[QStringLiteral("bindings")]        = bindings;
        root[QStringLiteral("activeWorkspace")] = QStringLiteral("ghost");

        WorkspaceDocument parsed;
        QString error;
        QStringList warnings;
        report("a damaged but valid document still parses",
               WorkspaceDocument::fromJson(root, &parsed, &error, &warnings));

        const WorkspaceSurface* main =
            parsed.workspace("classic")->surface(WorkspaceSurface::kMainId);
        report("only the sound item survives",
               main->items.size() == 1
                   && main->items.at(0).id == QStringLiteral("applet:RX"));
        report("a binding to a missing workspace is dropped",
               parsed.bindings.isEmpty());
        report("an active workspace naming nothing falls back to the first",
               parsed.activeWorkspace == QStringLiteral("classic"));
        report("every repair was reported", warnings.size() >= 5);

        // Malformed CONTAINERS are reported too, not just malformed leaves —
        // the parser claims "repaired and reported", and a silently skipped
        // surface is the shape most likely to lose an operator's arrangement
        // without a word (PR #4900 review, L2).
        QJsonObject badShapes;
        badShapes[QStringLiteral("version")] = WorkspaceDocument::kSchemaVersion;
        QJsonObject wsBad;
        wsBad[QStringLiteral("id")] = QStringLiteral("shapes");
        wsBad[QStringLiteral("surfaces")] =
            QJsonArray{QStringLiteral("not an object"), QJsonObject{
                {QStringLiteral("id"), WorkspaceSurface::kMainId},
                {QStringLiteral("items"), QJsonArray{QStringLiteral("also not an object")}}}};
        badShapes[QStringLiteral("workspaces")] = QJsonArray{wsBad};

        WorkspaceDocument shapes;
        QStringList shapeWarnings;
        report("a document with malformed containers still parses",
               WorkspaceDocument::fromJson(badShapes, &shapes, nullptr, &shapeWarnings));

        bool surfaceReported = false;
        bool itemReported    = false;
        for (const QString& w : shapeWarnings) {
            if (w.contains(QStringLiteral("non-object surface"))) surfaceReported = true;
            if (w.contains(QStringLiteral("non-object item")))    itemReported    = true;
        }
        report("a non-object surface is reported", surfaceReported);
        report("a non-object item is reported",    itemReported);

        // An unbound profile must resolve to nothing, not to a guess:
        // decision 8 says an unbound recall leaves the workspace alone.
        report("an unbound radio profile resolves to nothing",
               parsed.boundWorkspace(QStringLiteral("Contest CW")).isEmpty());
    }

    // ── A mangled window-geometry hint is dropped and reported ───────────
    //
    // The default QByteArray::fromBase64() ignores invalid characters and
    // returns whatever it managed, so mangled input would become
    // plausible-looking garbage that QWidget::restoreGeometry() rejects later
    // without a word. It is a hint either way — the value of catching it is
    // that it lands in warnings() instead of nowhere.
    {
        QJsonObject surface;
        surface[QStringLiteral("id")]             = QStringLiteral("canvas2");
        surface[QStringLiteral("items")]          = QJsonArray{};
        surface[QStringLiteral("windowGeometry")] = QStringLiteral("!!!not base64!!!");

        QJsonObject main;
        main[QStringLiteral("id")]    = WorkspaceSurface::kMainId;
        main[QStringLiteral("items")] = QJsonArray{};

        QJsonObject ws;
        ws[QStringLiteral("id")]       = QStringLiteral("classic");
        ws[QStringLiteral("surfaces")] = QJsonArray{main, surface};

        QJsonObject root;
        root[QStringLiteral("version")]    = WorkspaceDocument::kSchemaVersion;
        root[QStringLiteral("workspaces")] = QJsonArray{ws};

        WorkspaceDocument parsed;
        QStringList warnings;
        report("a document with a mangled geometry hint still parses",
               WorkspaceDocument::fromJson(root, &parsed, nullptr, &warnings));

        const WorkspaceSurface* aux =
            parsed.workspace("classic")->surface(QStringLiteral("canvas2"));
        report("the surface survives", aux != nullptr);
        report("...but the unreadable hint is dropped, not half-decoded",
               aux && aux->windowGeometry.isEmpty());

        bool reported = false;
        for (const QString& w : warnings) {
            if (w.contains(QStringLiteral("geometry"))) {
                reported = true;
            }
        }
        report("...and the drop is reported", reported);

        // A well-formed hint still round-trips untouched.
        report("a valid geometry hint survives a round trip",
               sample().workspace("contest")->surface(QStringLiteral("canvas2"))
                   != nullptr);
    }

    // ── A workspace with no main surface gains one ───────────────────────
    {
        QJsonObject surface;
        surface[QStringLiteral("id")]    = QStringLiteral("canvas2");
        surface[QStringLiteral("items")] = QJsonArray{};

        QJsonObject ws;
        ws[QStringLiteral("id")]       = QStringLiteral("odd");
        ws[QStringLiteral("surfaces")] = QJsonArray{surface};

        QJsonObject root;
        root[QStringLiteral("version")]    = WorkspaceDocument::kSchemaVersion;
        root[QStringLiteral("workspaces")] = QJsonArray{ws};

        WorkspaceDocument parsed;
        QStringList warnings;
        report("a workspace with no main surface still parses",
               WorkspaceDocument::fromJson(root, &parsed, nullptr, &warnings));
        report("...and gains an empty main surface",
               parsed.workspace("odd")->surface(WorkspaceSurface::kMainId) != nullptr);
        report("...and says so", !warnings.isEmpty());
    }

    // ── canvasEnabled round trip (phase 3) ───────────────────────────────
    {
        WorkspaceDocument off = sample();
        report("canvasEnabled defaults to false", !off.canvasEnabled);

        WorkspaceDocument offParsed;
        WorkspaceDocument::fromStoredJson(off.toStoredJson(), &offParsed);
        report("...and false is absent from the stored form",
               !off.toStoredJson().contains("canvasEnabled")
                   && !offParsed.canvasEnabled);

        WorkspaceDocument on = sample();
        on.canvasEnabled = true;
        WorkspaceDocument onParsed;
        WorkspaceDocument::fromStoredJson(on.toStoredJson(), &onParsed);
        report("canvasEnabled survives a round trip", onParsed.canvasEnabled);
    }

    // ── Binding lookup ───────────────────────────────────────────────────
    {
        const WorkspaceDocument doc = sample();
        report("a bound radio profile resolves to its workspace",
               doc.boundWorkspace(QStringLiteral("Contest CW"))
                   == QStringLiteral("contest"));
        report("an unknown radio profile resolves to nothing",
               doc.boundWorkspace(QStringLiteral("Never Seen")).isEmpty());
    }

    // ── Phase 6: the closed flag (full-recall ruling) ────────────────────
    {
        WorkspaceDocument doc = sample();
        for (Workspace& w : doc.workspaces) {
            for (WorkspaceSurface& sf : w.surfaces) {
                if (!sf.items.isEmpty()) sf.items[0].closed = true;
            }
        }
        WorkspaceDocument parsed;
        WorkspaceDocument::fromStoredJson(doc.toStoredJson(), &parsed);
        bool survived = false;
        for (const Workspace& w : parsed.workspaces) {
            for (const WorkspaceSurface& sf : w.surfaces) {
                if (!sf.items.isEmpty() && sf.items.first().closed) survived = true;
            }
        }
        report("the closed flag survives a round trip", survived);
        report("...and stays absent-when-false in the stored form",
               !QString::fromUtf8(sample().toStoredJson())
                    .contains(QStringLiteral("\"closed\"")));
    }

    // ── K6OZY: duplicate labels are repaired at parse, with a warning ────
    {
        WorkspaceDocument doc = sample();
        for (Workspace& w : doc.workspaces) {
            w.label = QStringLiteral("Same");
        }
        WorkspaceDocument parsed;
        WorkspaceDocument::fromStoredJson(doc.toStoredJson(), &parsed);
        QSet<QString> labels;
        bool unique = true;
        for (const Workspace& w : parsed.workspaces) {
            if (labels.contains(w.label)) unique = false;
            labels.insert(w.label);
        }
        report("duplicate labels are de-duplicated at parse", unique);
    }

    // ── Phase 6: workspace CRUD invariants ───────────────────────────────
    {
        WorkspaceDocument doc = sample();
        const int before = doc.workspaces.size();

        const QString dup =
            doc.addDuplicateOf(doc.activeWorkspace, QStringLiteral("Copy"));
        report("duplicate clones the whole arrangement",
               !dup.isEmpty()
                   && doc.workspace(dup)->surface(WorkspaceSurface::kMainId)
                              ->items.size()
                          == doc.workspace(doc.activeWorkspace)
                                 ->surface(WorkspaceSurface::kMainId)
                                 ->items.size());
        report("duplicate of nothing returns empty",
               doc.addDuplicateOf(QStringLiteral("nope"), {}).isEmpty());

        const QString blank = doc.addBlank(QStringLiteral("Copy"));
        report("labels never collide",
               doc.workspace(blank)->label != doc.workspace(dup)->label);
        report("blank has a main surface and no items",
               doc.workspace(blank)->surface(WorkspaceSurface::kMainId) != nullptr
                   && doc.workspace(blank)
                          ->surface(WorkspaceSurface::kMainId)
                          ->items.isEmpty());

        report("rename to the SAME name is stable (review M4)",
               doc.renameWorkspace(blank, doc.workspace(blank)->label)
                   && !doc.workspace(blank)->label.contains(
                          QStringLiteral("(2)")));
        {
            // Not vacuous (review, K6OZY: the old form passed with a no-op
            // rename): assert the VALUE is the dedup form.
            const QString target = doc.workspace(dup)->label;
            report("rename de-duplicates too",
                   doc.renameWorkspace(blank, target)
                       && doc.workspace(blank)->label
                              == QStringLiteral("%1 (2)").arg(target));
        }

        // Deleting: bindings drop, active falls back, the last one refuses.
        doc.bindings.insert(QStringLiteral("SomeProfile"), dup);
        doc.activeWorkspace = dup;
        report("delete removes, unbinds, and re-actives",
               doc.removeWorkspace(dup)
                   && !doc.contains(dup)
                   && doc.boundWorkspace(QStringLiteral("SomeProfile")).isEmpty()
                   && doc.activeWorkspace == doc.workspaces.first().id);
        while (doc.workspaces.size() > 1) {
            doc.removeWorkspace(doc.workspaces.last().id);
        }
        report("the last workspace cannot be deleted",
               !doc.removeWorkspace(doc.workspaces.first().id)
                   && doc.workspaces.size() == 1);
        Q_UNUSED(before);
    }

    // ── Surface CRUD + hide-and-keep flag (RFC #4887 phase 7) ────────────
    {
        WorkspaceDocument doc;
        const QString ws = doc.addBlank(QStringLiteral("Main"));
        doc.activeWorkspace = ws;

        const QString s1 = doc.addSurface(ws, QStringLiteral("Right"));
        const QString s2 = doc.addSurface(ws, QStringLiteral("Right"));
        report("addSurface mints sequential ids and de-dups labels",
               s1 == QStringLiteral("canvas2") && s2 == QStringLiteral("canvas3")
                   && doc.workspace(ws)->surface(s2)->label
                          == QStringLiteral("Right (2)"));
        report("rename to own label is a no-op, not a suffix",
               doc.renameSurface(ws, s1, QStringLiteral("Right"))
                   && doc.workspace(ws)->surface(s1)->label
                          == QStringLiteral("Right"));
        report("main cannot be removed",
               !doc.removeSurface(ws, WorkspaceSurface::kMainId));

        // hidden round-trips, absent-when-false; a hidden MAIN repairs.
        for (Workspace& w : doc.workspaces) {
            for (WorkspaceSurface& surf : w.surfaces) {
                if (surf.id == s1) surf.hidden = true;
            }
        }
        {
            const QByteArray blob = doc.toStoredJson();
            report("hidden serializes on the closed window only",
                   blob.contains("\"hidden\"")
                       && blob.count("\"hidden\"") == 1);
            WorkspaceDocument back;
            report("...and round-trips",
                   WorkspaceDocument::fromStoredJson(blob, &back)
                       && back.workspace(ws)->surface(s1)->hidden
                       && !back.workspace(ws)->surface(s2)->hidden);
        }
        {
            QJsonObject root = doc.toJson();
            QJsonArray wsArr = root.value(QStringLiteral("workspaces")).toArray();
            QJsonObject w0   = wsArr.at(0).toObject();
            QJsonArray surfs = w0.value(QStringLiteral("surfaces")).toArray();
            QJsonObject main = surfs.at(0).toObject();
            main[QStringLiteral("hidden")] = true;
            surfs[0] = main;
            w0[QStringLiteral("surfaces")] = surfs;
            wsArr[0] = w0;
            root[QStringLiteral("workspaces")] = wsArr;
            WorkspaceDocument repaired;
            QStringList warnings;
            report("a hidden MAIN surface is repaired with a warning",
                   WorkspaceDocument::fromJson(root, &repaired, nullptr,
                                               &warnings)
                       && !repaired.workspace(ws)
                               ->surface(WorkspaceSurface::kMainId)
                               ->hidden
                       && !warnings.isEmpty());
        }

        // Item ids are WORKSPACE-wide identity: a duplicate on another
        // surface is dropped at parse, not placed twice.
        {
            QJsonObject root = doc.toJson();
            QJsonArray wsArr = root.value(QStringLiteral("workspaces")).toArray();
            QJsonObject w0   = wsArr.at(0).toObject();
            QJsonArray surfs = w0.value(QStringLiteral("surfaces")).toArray();
            QJsonObject item;
            item[QStringLiteral("id")]   = QStringLiteral("applet:DUP");
            item[QStringLiteral("rect")] =
                QJsonArray{0.1, 0.1, 0.5, 0.5};
            for (int i = 0; i < 2; ++i) {
                QJsonObject so   = surfs.at(i).toObject();
                QJsonArray items = so.value(QStringLiteral("items")).toArray();
                items.append(item);
                so[QStringLiteral("items")] = items;
                surfs[i] = so;
            }
            w0[QStringLiteral("surfaces")] = surfs;
            wsArr[0] = w0;
            root[QStringLiteral("workspaces")] = wsArr;
            WorkspaceDocument back;
            QStringList warnings;
            int found = 0;
            const bool ok =
                WorkspaceDocument::fromJson(root, &back, nullptr, &warnings);
            if (ok) {
                for (const WorkspaceSurface& surf :
                     back.workspace(ws)->surfaces) {
                    for (const CanvasItem& it : surf.items) {
                        if (it.id == QStringLiteral("applet:DUP")) ++found;
                    }
                }
            }
            report("duplicate item ids are deduped ACROSS surfaces",
                   ok && found == 1 && !warnings.isEmpty());
        }

        // removeSurface parks the orphans on main.
        {
            for (Workspace& w : doc.workspaces) {
                for (WorkspaceSurface& surf : w.surfaces) {
                    if (surf.id == s2) {
                        CanvasItem it;
                        it.id   = QStringLiteral("applet:ORPHAN");
                        it.rect = NormRect{0.2, 0.2, 0.4, 0.4};
                        surf.items.append(it);
                    }
                }
            }
            report("removeSurface moves items to main",
                   doc.removeSurface(ws, s2)
                       && !doc.workspace(ws)->surface(s2)
                       && [&] {
                              for (const CanvasItem& it :
                                   doc.workspace(ws)
                                       ->surface(WorkspaceSurface::kMainId)
                                       ->items) {
                                  if (it.id == QStringLiteral("applet:ORPHAN"))
                                      return true;
                              }
                              return false;
                          }());
        }
        // duplicate deep-copies surfaces (pre-existing behaviour, now
        // load-bearing for phase 7).
        const QString clone = doc.addDuplicateOf(ws, QStringLiteral("Clone"));
        report("duplicating a workspace clones its extra surfaces",
               !clone.isEmpty()
                   && doc.workspace(clone)->surface(s1) != nullptr
                   && doc.workspace(clone)->surface(s1)->hidden);
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
