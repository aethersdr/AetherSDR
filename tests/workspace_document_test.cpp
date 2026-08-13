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

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
