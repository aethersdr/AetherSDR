// Migration from the legacy layout keys to a Classic workspace, and the
// Classic geometry itself (RFC #4887, phase 2).
//
// Runs against a REAL AppSettings in a temporary home (TestSettingsProfile),
// with legacy keys written in exactly the shape the current owners write
// them — the same approach bandstack_scoped_test uses.  A migration tested
// against a mock of the keys would prove nothing about the keys.
//
// The case that matters most is the last one: migration must NOT drag a
// floating pan or a floating applet onto the canvas.  Pop-out stays (RFC
// decision 1), and phase 2's contract is that there is no user-visible
// change — yanking someone's pop-outs into a canvas they have not opted into
// would be about the largest user-visible change available.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/workspace/ClassicLayout.h"
#include "gui/workspace/WorkspaceMigration.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <iostream>

using AetherSDR::AppSettings;
using AetherSDR::CanvasItem;
using AetherSDR::LegacyLayoutState;
using AetherSDR::NormRect;
using AetherSDR::WorkspaceDocument;
using AetherSDR::WorkspaceSurface;
using AetherSDR::buildClassicDocument;
using AetherSDR::classicWorkspaceId;
using AetherSDR::composeClassic;
using AetherSDR::knownPanLayoutIds;
using AetherSDR::panCellsForLayout;
using AetherSDR::panCountForLayout;
using AetherSDR::readLegacyLayoutState;

namespace {

int g_failures = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failures;
    }
}

bool nearly(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) <= tol;
}

const QStringList kApplets = {"RX", "TX", "PWR", "WAVE", "CAT"};

void setKey(const QString& key, const QString& value)
{
    AppSettings::instance().setValue(key, value);
}

void clearLegacyKeys()
{
    auto& s = AppSettings::instance();
    for (const QString& id : kApplets) {
        s.remove(QStringLiteral("Applet_") + id);
    }
    s.remove(QStringLiteral("AppletOrder"));
    s.remove(QStringLiteral("PanadapterLayout"));
    s.remove(QStringLiteral("FloatingPanIds"));
    s.remove(QStringLiteral("AppletPanelDockedLeft"));
    s.remove(QStringLiteral("AppletPanelVisible"));
    s.remove(QStringLiteral("ContainerTree"));
}

// A ContainerTree blob in the shape ContainerManager::saveState() writes.
QString containerTreeWith(const QStringList& floatingIds)
{
    QJsonObject containers;
    for (const QString& id : floatingIds) {
        QJsonObject entry;
        entry[QStringLiteral("mode")]        = QStringLiteral("floating");
        entry[QStringLiteral("visible")]     = true;
        entry[QStringLiteral("contentType")] = QString();
        entry[QStringLiteral("parent")]      = QString();
        containers[id] = entry;
    }
    QJsonObject root;
    root[QStringLiteral("version")]    = 1;
    root[QStringLiteral("containers")] = containers;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

const CanvasItem* findItem(const QList<CanvasItem>& items, const QString& id)
{
    for (const CanvasItem& it : items) {
        if (it.id == id) {
            return &it;
        }
    }
    return nullptr;
}

double totalArea(const QList<NormRect>& cells)
{
    double area = 0.0;
    for (const NormRect& r : cells) {
        area += r.w * r.h;
    }
    return area;
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-workspace-migration-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ── The pan layout table ─────────────────────────────────────────────
    {
        // All twelve ids the dialog offers.  (The RFC's problem statement said
        // eleven and listed ten — the real table has twelve.)
        report("every layout id is known",
               knownPanLayoutIds().size() == 12);

        report("single is one full cell",
               panCellsForLayout(QStringLiteral("1"))
                   == QList<NormRect>{NormRect{0.0, 0.0, 1.0, 1.0}});

        report("2h is two columns",
               panCellsForLayout(QStringLiteral("2h"))
                   == QList<NormRect>({NormRect{0.0, 0.0, 0.5, 1.0},
                                       NormRect{0.5, 0.0, 0.5, 1.0}}));

        report("2v is two rows",
               panCellsForLayout(QStringLiteral("2v"))
                   == QList<NormRect>({NormRect{0.0, 0.0, 1.0, 0.5},
                                       NormRect{0.0, 0.5, 1.0, 0.5}}));

        // The asymmetric ones are where a hand-written table goes wrong.
        const QList<NormRect> mixed = panCellsForLayout(QStringLiteral("2h1"));
        report("2h1 is two on top and one below",
               mixed.size() == 3
                   && mixed.at(0) == NormRect{0.0, 0.0, 0.5, 0.5}
                   && mixed.at(1) == NormRect{0.5, 0.0, 0.5, 0.5}
                   && mixed.at(2) == NormRect{0.0, 0.5, 1.0, 0.5});

        const QList<NormRect> fourThree = panCellsForLayout(QStringLiteral("4h3"));
        report("4h3 is four on top and three below",
               fourThree.size() == 7
                   && nearly(fourThree.at(0).w, 0.25)
                   && nearly(fourThree.at(4).w, 1.0 / 3.0)
                   && nearly(fourThree.at(4).y, 0.5));

        // Every layout must tile the surface exactly — no dead strips, no
        // overlap.  This is the property that would silently rot if a new id
        // were added with a typo in its row counts.
        bool allTile = true;
        for (const QString& id : knownPanLayoutIds()) {
            if (!nearly(totalArea(panCellsForLayout(id)), 1.0, 1e-9)) {
                allTile = false;
                std::printf("       layout '%s' covers %.6f\n",
                            qPrintable(id), totalArea(panCellsForLayout(id)));
            }
        }
        report("every layout tiles the surface exactly", allTile);

        report("pan counts match the layout ids",
               panCountForLayout(QStringLiteral("2x4")) == 8
                   && panCountForLayout(QStringLiteral("3h2")) == 5
                   && panCountForLayout(QStringLiteral("1")) == 1);

        report("an unknown layout id yields nothing",
               panCellsForLayout(QStringLiteral("nope")).isEmpty()
                   && panCountForLayout(QStringLiteral("nope")) == 0);
    }

    // ── Classic composition ──────────────────────────────────────────────
    {
        const QList<CanvasItem> items =
            composeClassic({"0x40000000", "0x40000001"}, {"RX", "TX"},
                           QStringLiteral("2v"), /*appletsLeft=*/false);

        report("classic places every pan and applet", items.size() == 4);

        const CanvasItem* panA = findItem(items, QStringLiteral("pan:0x40000000"));
        const CanvasItem* rx   = findItem(items, QStringLiteral("applet:RX"));
        report("ids are namespaced by kind", panA && rx);

        report("pans take the surface minus the applet column",
               nearly(panA->rect.x, 0.0)
                   && nearly(panA->rect.w, 1.0 - AetherSDR::kClassicAppletColumnWidth));
        report("the applet column sits on the right",
               nearly(rx->rect.x, 1.0 - AetherSDR::kClassicAppletColumnWidth)
                   && nearly(rx->rect.w, AetherSDR::kClassicAppletColumnWidth));
        report("applets share the column height",
               nearly(rx->rect.h, 0.5));
        report("content types are set",
               panA->contentType == QStringLiteral("panadapter")
                   && rx->contentType == QStringLiteral("applet"));

        // Docked left: the mirror image.
        const QList<CanvasItem> left =
            composeClassic({"0x40000000"}, {"RX"}, QStringLiteral("1"), true);
        report("docked left mirrors the arrangement",
               nearly(findItem(left, "applet:RX")->rect.x, 0.0)
                   && nearly(findItem(left, "pan:0x40000000")->rect.x,
                             AetherSDR::kClassicAppletColumnWidth));

        // No applets: the pans take everything, rather than leaving a band of
        // dead canvas where the column would have been.
        const QList<CanvasItem> noApplets =
            composeClassic({"0x40000000"}, {}, QStringLiteral("1"), false);
        report("with no applets the pans take the whole surface",
               noApplets.size() == 1
                   && nearly(noApplets.at(0).rect.w, 1.0));

        // More pans than the layout has cells: the operator's chosen layout
        // wins, extras are dropped rather than a cell being invented.
        const QList<CanvasItem> tooMany =
            composeClassic({"a", "b", "c"}, {}, QStringLiteral("2h"), false);
        report("pans beyond the layout's cells are dropped", tooMany.size() == 2);

        // Unknown layout id falls back to single rather than placing nothing.
        const QList<CanvasItem> fallback =
            composeClassic({"a"}, {}, QStringLiteral("bogus"), false);
        report("an unknown layout falls back to single", fallback.size() == 1);

        // z is assigned, dense and ascending, so the document arrives with a
        // stacking order CanvasLayout will accept unchanged.
        bool zAscending = true;
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).z != i) {
                zAscending = false;
            }
        }
        report("composed items carry dense ascending z", zAscending);
    }

    // ── Reading the legacy keys ──────────────────────────────────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("PanadapterLayout"), QStringLiteral("2x2"));
        setKey(QStringLiteral("AppletOrder"), QStringLiteral("TX,RX,PWR"));
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("True"));
        setKey(QStringLiteral("Applet_TX"), QStringLiteral("True"));
        setKey(QStringLiteral("Applet_PWR"), QStringLiteral("False"));
        setKey(QStringLiteral("AppletPanelDockedLeft"), QStringLiteral("True"));

        const LegacyLayoutState state = readLegacyLayoutState(kApplets);

        report("the pan layout id is read", state.panLayoutId == QStringLiteral("2x2"));
        report("the saved applet order is honoured",
               state.openAppletIds == QStringList({"TX", "RX"}));
        report("an applet switched off is not placed",
               !state.openAppletIds.contains(QStringLiteral("PWR")));
        report("the dock side is read", state.appletsLeft);
        report("panel visibility defaults to true", state.appletPanelVisible);

        // An applet the operator has never touched has no key at all, and its
        // default lives in AppletPanel — so it is simply not placed rather
        // than guessed at.
        report("an applet with no key is not placed",
               !state.openAppletIds.contains(QStringLiteral("WAVE"))
                   && !state.openAppletIds.contains(QStringLiteral("CAT")));
    }

    // ── An unreadable boolean falls back, it does not mean "off" ─────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("yes-ish"));
        setKey(QStringLiteral("AppletPanelVisible"), QStringLiteral("garbage"));

        const LegacyLayoutState state = readLegacyLayoutState(kApplets);
        report("an unreadable applet flag is treated as not-open",
               !state.openAppletIds.contains(QStringLiteral("RX")));
        report("an unreadable panel-visible flag falls back to visible",
               state.appletPanelVisible);
    }

    // ── An unknown pan layout id falls back to single ────────────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("PanadapterLayout"), QStringLiteral("17x42"));
        report("an unknown stored layout id falls back to single",
               readLegacyLayoutState(kApplets).panLayoutId == QStringLiteral("1"));
    }

    // ── Floating things are left alone (RFC decision 1) ──────────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("PanadapterLayout"), QStringLiteral("2h"));
        setKey(QStringLiteral("AppletOrder"), QStringLiteral("RX,TX,WAVE"));
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("True"));
        setKey(QStringLiteral("Applet_TX"), QStringLiteral("True"));
        setKey(QStringLiteral("Applet_WAVE"), QStringLiteral("True"));
        setKey(QStringLiteral("FloatingPanIds"), QStringLiteral("0x40000001"));
        setKey(QStringLiteral("ContainerTree"), containerTreeWith({"WAVE"}));

        const LegacyLayoutState state = readLegacyLayoutState(kApplets);

        report("a floated applet is recorded as floating",
               state.floatingAppletIds == QStringList({"WAVE"}));
        report("a floated applet is NOT placed on the canvas",
               state.openAppletIds == QStringList({"RX", "TX"}));
        report("floating pan ids are recorded",
               state.floatingPanIds == QStringList({"0x40000001"}));

        const WorkspaceDocument doc =
            buildClassicDocument(state, {"0x40000000", "0x40000001"});
        const WorkspaceSurface* main =
            doc.workspace(classicWorkspaceId())->surface(WorkspaceSurface::kMainId);

        report("the floated applet is absent from the document",
               findItem(main->items, QStringLiteral("applet:WAVE")) == nullptr);
        report("the docked applets are present",
               findItem(main->items, QStringLiteral("applet:RX")) != nullptr
                   && findItem(main->items, QStringLiteral("applet:TX")) != nullptr);
    }

    // ── A corrupt ContainerTree does not fail the migration ──────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("True"));
        setKey(QStringLiteral("ContainerTree"), QStringLiteral("{ this is not json"));

        const LegacyLayoutState state = readLegacyLayoutState(kApplets);
        report("a corrupt container tree still yields a usable Classic",
               state.openAppletIds == QStringList({"RX"})
                   && state.floatingAppletIds.isEmpty());
    }

    // ── The document migration produces ──────────────────────────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("PanadapterLayout"), QStringLiteral("2v"));
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("True"));

        const WorkspaceDocument doc =
            buildClassicDocument(readLegacyLayoutState(kApplets), {"0x40000000"});

        report("exactly one workspace is produced", doc.workspaces.size() == 1);
        report("it is Classic, and active",
               doc.activeWorkspace == classicWorkspaceId()
                   && doc.workspace(classicWorkspaceId())->label == QStringLiteral("Classic"));
        report("it has exactly one main surface",
               doc.workspace(classicWorkspaceId())->surfaces.size() == 1
                   && doc.workspace(classicWorkspaceId())->surfaces.at(0).id
                          == WorkspaceSurface::kMainId);
        report("migration creates no bindings", doc.bindings.isEmpty());

        // And it survives the schema it will be stored in.
        WorkspaceDocument reparsed;
        QStringList warnings;
        report("the migrated document round-trips through the schema",
               WorkspaceDocument::fromStoredJson(doc.toStoredJson(), &reparsed,
                                                 nullptr, &warnings)
                   && warnings.isEmpty()
                   && reparsed.workspaceIds() == doc.workspaceIds());
    }

    // ── A hidden applet panel means no column ────────────────────────────
    {
        clearLegacyKeys();
        setKey(QStringLiteral("Applet_RX"), QStringLiteral("True"));
        setKey(QStringLiteral("AppletPanelVisible"), QStringLiteral("False"));

        const WorkspaceDocument doc =
            buildClassicDocument(readLegacyLayoutState(kApplets), {"0x40000000"});
        const WorkspaceSurface* main =
            doc.workspace(classicWorkspaceId())->surface(WorkspaceSurface::kMainId);

        report("a hidden panel places no applets",
               findItem(main->items, QStringLiteral("applet:RX")) == nullptr);
        report("...and the pan takes the whole surface",
               nearly(findItem(main->items, QStringLiteral("pan:0x40000000"))->rect.w, 1.0));
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
