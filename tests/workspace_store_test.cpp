// WorkspaceStore — persistence and the auto-commit contract (RFC #4887,
// phase 2), against a real AppSettings in a temporary home.
//
// The contract under test is RFC decision 7: placement changes commit as they
// happen, so an automatic workspace switch can never discard unsaved work.
// That is only true if three things hold, and each is asserted here:
//
//   1. a flush actually reaches DISK — not just the in-memory settings map.
//      AppSettings deliberately keeps a failed save in memory for retry, so a
//      cache re-read would pass even when nothing was committed; the store
//      test therefore reads the row back from the FILE (Principle VIII, and
//      the pattern AppSettings::readStationRowFromDisk exists for).
//   2. a gesture COALESCES — one drag must not produce hundreds of
//      whole-document writes.
//   3. replaying saved state does NOT write back what it is reading — the
//      #4427 rule, inherited from ContainerManager rather than reinvented.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/workspace/WorkspaceStore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QStringList>

#include <cstdio>
#include <iostream>

using AetherSDR::AppSettings;
using AetherSDR::CanvasItem;
using AetherSDR::NormRect;
using AetherSDR::Workspace;
using AetherSDR::WorkspaceDocument;
using AetherSDR::WorkspaceStore;
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

WorkspaceDocument docWith(const QString& workspaceId, const QString& itemId)
{
    CanvasItem item;
    item.id   = itemId;
    item.rect = NormRect{0.0, 0.0, 0.5, 0.5};

    WorkspaceSurface main;
    main.id = WorkspaceSurface::kMainId;
    main.items.append(item);

    Workspace ws;
    ws.id    = workspaceId;
    ws.label = workspaceId;
    ws.surfaces.append(main);

    WorkspaceDocument doc;
    doc.workspaces.append(ws);
    doc.activeWorkspace = workspaceId;
    return doc;
}

// What is actually on disk, not what the settings cache believes.
QString storedOnDisk()
{
    QString value;
    if (!AppSettings::instance().readStationRowFromDisk(WorkspaceStore::kSettingsKey,
                                                        value)) {
        return {};
    }
    return value;
}

// Spin the event loop until `predicate` holds or the budget runs out, so the
// debounce cases neither sleep blindly nor hang a CI run.
bool waitFor(const std::function<bool()>& predicate, int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < budgetMs) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return predicate();
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-workspace-store-test"));
    if (!profile.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ── An empty store ───────────────────────────────────────────────────
    {
        WorkspaceStore store;
        report("loading an empty store fails cleanly", !store.load());
        report("...and is not marked loaded", !store.isLoaded());
        report("...and says why", !store.lastError().isEmpty());
    }

    // ── Write, and prove it reached disk ─────────────────────────────────
    {
        WorkspaceStore store;
        store.setDocument(docWith(QStringLiteral("classic"), QStringLiteral("applet:RX")));
        report("setDocument marks the store dirty", store.isDirty());

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);
        report("flush reports that it wrote", store.flush());
        report("...and is clean afterwards", !store.isDirty());
        report("...and emitted flushed once", flushSpy.count() == 1);

        // The load-bearing assertion: the row is on the FILE, not merely in
        // the settings cache.
        report("the document reached disk", storedOnDisk().contains("applet:RX"));

        report("flushing a clean store is a no-op", !store.flush());
    }

    // ── Read back in a fresh store ───────────────────────────────────────
    {
        WorkspaceStore store;
        report("a stored document loads", store.load());
        report("...and is marked loaded", store.isLoaded());
        report("...with no warnings", store.warnings().isEmpty());
        report("...and carries its content",
               store.document().activeWorkspace == QStringLiteral("classic")
                   && store.document()
                              .workspace(QStringLiteral("classic"))
                              ->surface(WorkspaceSurface::kMainId)
                              ->items.size() == 1);
        report("a freshly loaded store is clean", !store.isDirty());
    }

    // ── A gesture coalesces into one write ───────────────────────────────
    {
        WorkspaceStore store;
        store.load();
        store.setDebounceMs(30);

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);

        // Stand in for a drag: many placement changes in quick succession.
        for (int i = 0; i < 200; ++i) {
            store.touch();
        }
        report("a gesture in progress has not written yet", flushSpy.count() == 0);

        report("the debounce eventually fires",
               waitFor([&flushSpy] { return flushSpy.count() > 0; }, 2000));
        report("200 changes produced exactly one write", flushSpy.count() == 1);
        report("...and the store is clean again", !store.isDirty());
    }

    // ── Ending a gesture flushes immediately ─────────────────────────────
    {
        WorkspaceStore store;
        store.load();
        store.setDebounceMs(60'000);   // long enough that only an explicit flush can win

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);
        store.setDocument(docWith(QStringLiteral("contest"), QStringLiteral("applet:TX")));
        report("a pending change has not written", flushSpy.count() == 0);

        store.flush();
        report("the end of a gesture writes immediately", flushSpy.count() == 1);
        report("...and it reached disk", storedOnDisk().contains("applet:TX"));
    }

    // ── Restoring must not write back what it is reading (#4427) ─────────
    {
        WorkspaceStore store;
        store.load();
        store.setDebounceMs(10);

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);

        store.setRestoring(true);
        store.setDocument(docWith(QStringLiteral("replayed"), QStringLiteral("applet:VU")));
        store.touch();
        report("a flush during restore is refused", !store.flush());

        // Give the debounce every chance to fire behind our back.
        waitFor([] { return false; }, 100);
        report("no write happened while restoring", flushSpy.count() == 0);
        report("the change is still pending", store.isDirty());
        report("nothing was written to disk",
               !storedOnDisk().contains("applet:VU"));

        // Leaving restore must not flush by itself — it leaves the store as
        // dirty as it found it, for the caller to decide.
        store.setRestoring(false);
        report("leaving restore does not write on its own", flushSpy.count() == 0);
        report("...and the change is still pending", store.isDirty());

        report("an explicit flush after restore works", store.flush());
        report("...and reaches disk", storedOnDisk().contains("applet:VU"));
    }

    // ── A dirty store commits at destruction ─────────────────────────────
    //
    // Auto-commit promises there is no unsaved work; a store torn down with a
    // pending debounce would be exactly that.
    {
        {
            WorkspaceStore store;
            store.load();
            store.setDebounceMs(60'000);
            store.setDocument(docWith(QStringLiteral("teardown"),
                                      QStringLiteral("applet:CAT")));
            report("the change is pending at teardown", store.isDirty());
        }
        report("teardown committed the pending change",
               storedOnDisk().contains("applet:CAT"));
    }

    // ── A corrupt stored document is refused, not half-adopted ───────────
    {
        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                QStringLiteral("{ not json"));
        AppSettings::instance().save();

        WorkspaceStore store;
        report("a corrupt document fails to load", !store.load());
        report("...and the store is not marked loaded", !store.isLoaded());
        report("...and reports the parse error", !store.lastError().isEmpty());
        report("...and is reported as unusable, not absent",
               store.loadWithStatus() == WorkspaceStore::LoadResult::Unusable);
    }

    // ── A present-but-unusable document is NEVER migrated over ───────────
    //
    // The failure this closes: an operator runs a build whose schema is newer,
    // hits a bug, downgrades. If loadOrMigrate() treated "refused" the same as
    // "absent", it would write a v1 Classic over the v2 document and every
    // workspace, binding and surface the newer build held would be gone — the
    // exact loss WorkspaceDocument's newer-schema guard exists to prevent.
    {
        const QString newerDocument = QStringLiteral(
            R"({"version":99,"activeWorkspace":"from-the-future",)"
            R"("workspaces":[{"id":"from-the-future","surfaces":[]}]})");

        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                newerDocument);
        AppSettings::instance().save();

        WorkspaceStore store;
        report("a newer-schema document is refused",
               store.loadWithStatus() == WorkspaceStore::LoadResult::Unusable);

        bool migrated = true;
        report("loadOrMigrate refuses rather than migrating over it",
               !store.loadOrMigrate({"RX"}, {"0x40000000"}, &migrated));
        report("...and reports that it did not migrate", !migrated);
        report("...and says why", store.lastError().contains(QStringLiteral("newer")));

        // The load-bearing assertion: the newer document is still on disk,
        // byte for byte, for the build that understands it.
        report("the newer document survives untouched",
               storedOnDisk() == newerDocument);

        // Same for damage that is merely corrupt.
        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                QStringLiteral("{ truncated"));
        AppSettings::instance().save();

        WorkspaceStore corrupt;
        report("a corrupt document is not migrated over either",
               !corrupt.loadOrMigrate({"RX"}, {"0x40000000"}));
        report("...and it also survives", storedOnDisk() == QStringLiteral("{ truncated"));
    }

    // ── A change made during restore still gets its debounce ─────────────
    //
    // touch() starts no timer while suppressed, so leaving restore has to
    // rearm it — otherwise the change waits for the next unrelated touch(),
    // an explicit flush(), or the destructor, and a crash skips the last one.
    {
        AppSettings::instance().removeStationValue(WorkspaceStore::kSettingsKey);
        AppSettings::instance().save();

        WorkspaceStore store;
        store.setDocument(docWith(QStringLiteral("base"), QStringLiteral("applet:RX")));
        store.flush();
        store.setDebounceMs(30);

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);

        store.setRestoring(true);
        store.setDocument(docWith(QStringLiteral("during"),
                                  QStringLiteral("applet:DURING")));
        report("nothing is written while restoring", flushSpy.count() == 0);

        store.setRestoring(false);
        report("leaving restore still does not write immediately",
               flushSpy.count() == 0);
        report("...but the debounce is rearmed and fires on its own",
               waitFor([&flushSpy] { return flushSpy.count() > 0; }, 2000));
        report("...and the change reached disk",
               storedOnDisk().contains("applet:DURING"));
    }

    // ── loadOrMigrate falls back to Classic, once ────────────────────────
    {
        AppSettings::instance().removeStationValue(WorkspaceStore::kSettingsKey);
        AppSettings::instance().save();

        AppSettings::instance().setValue(QStringLiteral("Applet_RX"),
                                         QStringLiteral("True"));
        AppSettings::instance().setValue(QStringLiteral("PanadapterLayout"),
                                         QStringLiteral("2v"));

        WorkspaceStore store;
        bool migrated = false;
        report("loadOrMigrate succeeds with nothing stored",
               store.loadOrMigrate({"RX", "TX"}, {"0x40000000"}, &migrated));
        report("...and reports that it migrated", migrated);
        report("...producing Classic",
               store.document().activeWorkspace == QStringLiteral("classic"));
        report("...which it wrote to disk", storedOnDisk().contains("classic"));

        // The legacy keys are left in place: dual-write for a release means a
        // downgrade still finds the state it expects.
        report("migration does not delete the legacy keys",
               AppSettings::instance().contains(QStringLiteral("Applet_RX"))
                   && AppSettings::instance().contains(QStringLiteral("PanadapterLayout")));

        // Second time around there IS a document, so migration must not run
        // again and overwrite whatever the operator has since arranged.
        WorkspaceStore second;
        bool migratedAgain = true;
        report("a second loadOrMigrate loads instead of migrating",
               second.loadOrMigrate({"RX", "TX"}, {"0x40000000"}, &migratedAgain)
                   && !migratedAgain);
    }

    // ── The write block: refusing to parse is only half a guard ──────────
    //
    // The first round closed the overwrite at loadOrMigrate(). It was still
    // open one call further along: nothing consulted the failed load on the
    // WRITE path, so a single setDocument() or touch() from phase 3 — a canvas
    // resize while the error is on screen is enough — would commit an empty
    // default document over the row this build could not read.
    {
        const QString newerDocument = QStringLiteral(
            R"({"version":99,"activeWorkspace":"from-the-future",)"
            R"("workspaces":[{"id":"from-the-future","surfaces":[]}]})");

        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                newerDocument);
        AppSettings::instance().save();

        WorkspaceStore store;
        store.loadWithStatus();
        report("a refused load arms the write block", store.isWriteBlocked());

        QSignalSpy flushSpy(&store, &WorkspaceStore::flushed);

        // Exactly the phase-3 accident: an ordinary edit after a failed load.
        store.setDocument(docWith(QStringLiteral("accident"),
                                  QStringLiteral("applet:OOPS")));
        report("an explicit flush is suppressed",
               store.flushWithStatus() == WorkspaceStore::FlushResult::Suppressed);
        report("no write was emitted", flushSpy.count() == 0);

        // And the debounce must not sneak one through either.
        store.setDebounceMs(20);
        store.touch();
        waitFor([&flushSpy] { return flushSpy.count() > 0; }, 300);
        report("the debounce cannot bypass the write block", flushSpy.count() == 0);

        report("the newer document is still intact", storedOnDisk() == newerDocument);

        // Only an explicit decision clears it.
        store.allowOverwrite();
        report("allowOverwrite clears the block", !store.isWriteBlocked());
        report("...and then the write lands", store.flush());
        report("...over the old document", storedOnDisk().contains("applet:OOPS"));
    }

    // ── A dirty write-blocked store does not commit at teardown either ───
    {
        AppSettings::instance().setStationValue(WorkspaceStore::kSettingsKey,
                                                QStringLiteral("{ corrupt"));
        AppSettings::instance().save();
        {
            WorkspaceStore store;
            store.loadWithStatus();
            store.setDocument(docWith(QStringLiteral("teardown-blocked"),
                                      QStringLiteral("applet:NOPE")));
        }
        report("teardown respects the write block",
               storedOnDisk() == QStringLiteral("{ corrupt"));
    }

    // ── flush() distinguishes suppressed from clean ──────────────────────
    {
        AppSettings::instance().removeStationValue(WorkspaceStore::kSettingsKey);
        AppSettings::instance().save();

        WorkspaceStore store;
        store.setDocument(docWith(QStringLiteral("tri"), QStringLiteral("applet:RX")));
        store.flush();

        report("a clean store reports Clean",
               store.flushWithStatus() == WorkspaceStore::FlushResult::Clean);

        store.touch();
        store.setRestoring(true);
        report("a restoring store reports Suppressed",
               store.flushWithStatus() == WorkspaceStore::FlushResult::Suppressed);
        store.setRestoring(false);
        report("and a pending change reports Wrote",
               store.flushWithStatus() == WorkspaceStore::FlushResult::Wrote);
    }

    // ── An empty-but-valid document is Absent, not Loaded ────────────────
    //
    // Otherwise loadOrMigrate() succeeds holding no workspace at all and never
    // migrates, leaving phase 3 with nothing to show and no way to get it.
    {
        AppSettings::instance().setStationValue(
            WorkspaceStore::kSettingsKey,
            QStringLiteral(R"({"version":1,"activeWorkspace":"","workspaces":[]})"));
        AppSettings::instance().save();

        WorkspaceStore store;
        report("a document with no workspaces reads as Absent",
               store.loadWithStatus() == WorkspaceStore::LoadResult::Absent);
        report("...so it is not write-blocked", !store.isWriteBlocked());

        bool migrated = false;
        report("...and loadOrMigrate migrates into it",
               store.loadOrMigrate({"RX"}, {"0x40000000"}, &migrated) && migrated);
        report("...producing a real workspace",
               store.document().workspaceIds() == QStringList({"classic"}));
    }

    std::printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES present.");
    return g_failures == 0 ? 0 : 1;
}
