// The folder swap AetherSDRUpdater.exe performs (src/updater/UpdateSwap),
// driven through injected processes on a simulated clock, on any platform.
//
// Each "install" is a folder whose AetherSDR.exe is a text file saying what
// the binary would do when its self-test runs: "good <version>" writes the
// marker and exits, "broken" exits without it, "hang" never exits. Renames
// can be made to fail, as a locked folder or another volume would.
// No Qt, no sockets, no real processes.

#include "updater/UpdateSwap.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace AetherSDR::updater;

namespace {

int failures = 0;

void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void put(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

struct FakeProc {
    std::string kind;   // what the launched binary does
    fs::path marker;
    std::string version;
    bool exited = false;
    bool terminated = false;
};

struct Harness {
    fs::path root;
    Plan plan;
    Ops ops;
    Clock::time_point clock{};
    bool oldExits = true;
    std::vector<std::pair<fs::path, std::vector<fs::path>>> launches;
    std::vector<std::shared_ptr<FakeProc>> procs;
    std::map<std::string, int> renameFailures;   // "<from name>" -> how many times to fail
    std::string journalFailsAt;                  // a step ("swap-1") whose journal write fails
    std::vector<std::string> logLines;

    explicit Harness(const std::string& name)
    {
        root = fs::temp_directory_path() / ("update-swap-test-" + name);
        fs::remove_all(root);
        plan.install = root / "apps" / "AetherSDR";
        plan.staged = root / "data" / "update" / "26.10.4";
        plan.stateDir = root / "data" / "update";
        plan.exeName = "AetherSDR.exe";
        plan.version = "26.10.4";
        put(plan.install / "AetherSDR.exe", "good 26.10.3");
        put(plan.install / "Qt6Core.dll", "qt 6.12 old");
        put(plan.install / "unins000.dat", "the installer's own");
        put(plan.staged / "AetherSDR.exe", "good 26.10.4");
        put(plan.staged / "Qt6Core.dll", "qt 6.12 new");
        put(plan.staged / "unins000.dat", "the installer's own");

        ops.waitForOldExit = [this](Duration) { return oldExits; };
        ops.launch = [this](const fs::path& exe, const std::vector<fs::path>& args) {
            launches.push_back({exe, args});
            auto p = std::make_shared<FakeProc>();
            const std::string body = slurp(exe);
            p->kind = body.substr(0, body.find(' '));
            p->version = body.find(' ') == std::string::npos ? "" : body.substr(body.find(' ') + 1);
            if (args.size() == 2 && args[0] == "--update-selftest") {
                p->marker = args[1];
                if (p->kind == "good") {
                    put(p->marker, p->version);
                    p->exited = true;
                } else if (p->kind == "broken") {
                    p->exited = true;
                }
            }
            procs.push_back(p);
            return Process{p};
        };
        ops.hasExited = [](const Process& p) { return static_cast<FakeProc*>(p.handle.get())->exited; };
        ops.terminate = [](const Process& p) {
            auto* f = static_cast<FakeProc*>(p.handle.get());
            f->terminated = true;
            f->exited = true;
        };
        ops.sleep = [this](Duration d) {
            clock += d;
            // No path waits an hour: a loop with no way out is a failure,
            // not a hang.
            if (clock.time_since_epoch() > std::chrono::hours(1)) {
                std::fprintf(stderr, "FAIL: the swap waited over an hour (a loop with no exit)\n");
                std::exit(1);
            }
        };
        ops.now = [this] { return clock; };
        ops.log = [this](const std::string& s) { logLines.push_back(s); };
        ops.writeJournal = [this](const fs::path& path, const std::string& text) {
            if (!journalFailsAt.empty() && text.rfind(journalFailsAt, 0) == 0) {
                return false;
            }
            put(path, text);
            return true;
        };
        ops.rename = [this](const fs::path& from, const fs::path& to, std::error_code& ec) {
            auto it = renameFailures.find(from.filename().string());
            if (it != renameFailures.end() && it->second != 0) {
                if (it->second > 0) {
                    --it->second;
                }
                ec = std::make_error_code(std::errc::permission_denied);
                return;
            }
            fs::rename(from, to, ec);
        };
    }
    ~Harness() { fs::remove_all(root); }

    Paths paths() const { return pathsFor(plan); }
    std::string installed() const { return slurp(plan.install / "AetherSDR.exe"); }
    std::string result() const { return slurp(paths().result); }
};

void cleanUpdate()
{
    Harness h("clean");
    h.plan.relaunch = true;
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.installed() == "good 26.10.4");
    CHECK(slurp(h.plan.install / "Qt6Core.dll") == "qt 6.12 new");
    CHECK(slurp(h.paths().previous / "AetherSDR.exe") == "good 26.10.3");   // one kept
    CHECK(!fs::exists(h.paths().next));
    CHECK(!fs::exists(h.paths().journal));
    CHECK(!fs::exists(h.paths().marker));
    CHECK(h.result() == "Updated 26.10.4\n");
    // The self-test, then the app itself (Restart to update).
    CHECK(h.launches.size() == 2);
    CHECK(h.launches[0].second.size() == 2 && h.launches[0].second[0] == "--update-selftest");
    CHECK(h.launches[1].second.empty());
}

void onQuitTheAppIsNotStarted()
{
    Harness h("quit");
    h.plan.relaunch = false;
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.launches.size() == 1);   // the self-test only
}

void anOlderPreviousIsReplaced()
{
    Harness h("older-previous");
    put(h.paths().previous / "AetherSDR.exe", "good 26.10.1");
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(slurp(h.paths().previous / "AetherSDR.exe") == "good 26.10.3");
}

void aBrokenReleaseIsRolledBack()
{
    Harness h("broken");
    h.plan.relaunch = true;
    put(h.plan.staged / "AetherSDR.exe", "broken");
    CHECK(run(h.plan, h.ops) == Outcome::RolledBack);
    CHECK(h.installed() == "good 26.10.3");
    CHECK(slurp(h.paths().failed / "AetherSDR.exe") == "broken");
    CHECK(h.result() == "RolledBack 26.10.4\n");
    CHECK(h.launches.size() == 2 && h.launches[1].second.empty());   // the old version restarted
}

void aSelfTestThatHangsIsStoppedAndRolledBack()
{
    Harness h("hang");
    put(h.plan.staged / "AetherSDR.exe", "hang");
    CHECK(run(h.plan, h.ops) == Outcome::RolledBack);
    CHECK(h.procs.size() == 1 && h.procs[0]->terminated);
    CHECK(h.installed() == "good 26.10.3");
}

void anOldBinaryCannotPassForTheNewOne()
{
    // The staged tree still holds the old binary: its self-test reports
    // the wrong version and is treated as a failure.
    Harness h("wrong-version");
    put(h.plan.staged / "AetherSDR.exe", "good 26.10.3");
    CHECK(run(h.plan, h.ops) == Outcome::RolledBack);
}

void aRunningAppIsNeverTouched()
{
    Harness h("still-running");
    h.oldExits = false;
    CHECK(run(h.plan, h.ops) == Outcome::OldStillRunning);
    CHECK(h.installed() == "good 26.10.3");
    CHECK(fs::exists(h.plan.staged / "AetherSDR.exe"));
    CHECK(h.launches.empty());
}

void preFlightFailuresChangeNothing()
{
    {
        Harness h("no-staged-exe");
        fs::remove(h.plan.staged / "AetherSDR.exe");
        CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
        CHECK(h.installed() == "good 26.10.3");
    }
    {
        Harness h("leftover-new");
        put(h.paths().next / "x", "someone's folder");
        CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
        CHECK(slurp(h.paths().next / "x") == "someone's folder");   // not ours, not removed
    }
}

void anotherVolumeIsCopiedBesideTheInstall()
{
    Harness h("cross-volume");
    h.renameFailures["26.10.4"] = 1;   // staged -> .new fails, as across volumes
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.installed() == "good 26.10.4");
    CHECK(!fs::exists(h.plan.staged));   // the copy's source is cleaned up
}

void aBriefLockIsWaitedOut()
{
    Harness h("brief-lock");
    h.renameFailures["AetherSDR"] = 3;   // the install folder, held for ~0.75 s
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.installed() == "good 26.10.4");
}

void aLockThatNeverClearsIsUndone()
{
    {
        Harness h("lock-install");
        h.renameFailures["AetherSDR"] = -1;   // install -> .previous never succeeds
        CHECK(run(h.plan, h.ops) == Outcome::SwapUndone);
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!fs::exists(h.paths().next));
    }
    {
        Harness h("lock-new");
        h.renameFailures["AetherSDR.new"] = -1;   // .new -> install never succeeds
        CHECK(run(h.plan, h.ops) == Outcome::SwapUndone);
        CHECK(h.installed() == "good 26.10.3");   // the previous version put back
        CHECK(!fs::exists(h.paths().next));
    }
}

void aFailedRollbackIsReportedNotHidden()
{
    Harness h("broken-rollback");
    put(h.plan.staged / "AetherSDR.exe", "broken");
    h.renameFailures["AetherSDR.previous"] = -1;   // previous -> install never succeeds
    CHECK(run(h.plan, h.ops) == Outcome::Broken);
    CHECK(h.result() == "Broken 26.10.4\n");
    // The journal survives, so once the lock clears the next run repairs it.
    CHECK(fs::exists(h.paths().journal));
    CHECK(!fs::exists(h.plan.install));
    h.renameFailures.clear();
    CHECK(recover(h.plan, h.ops));
    CHECK(h.installed() == "good 26.10.3");
    CHECK(!fs::exists(h.paths().journal));
}

void aJournalThatCannotBeWrittenStopsTheStep()
{
    for (const char* step : {"placing", "swap-1", "swap-2", "selftest"}) {
        Harness h(std::string("journal-") + step);
        h.journalFailsAt = step;
        const Outcome o = run(h.plan, h.ops);
        CHECK(o == Outcome::NotPossible || o == Outcome::SwapUndone);
        CHECK(h.installed() == "good 26.10.3");   // the old version, whole
        CHECK(!fs::exists(h.paths().next));
        CHECK(!fs::exists(h.paths().journal));
        // Nothing untested was ever left in place: no self-test ran.
        CHECK(h.launches.empty());
    }
}

void aPendingRepairWaitsForTheAppToExit()
{
    // A journal from a helper that died mid-self-test, and an app that is
    // running again: nothing is renamed until it exits.
    Harness h("pending-repair");
    fs::rename(h.plan.install, h.paths().previous);
    fs::rename(h.plan.staged, h.plan.install);
    put(h.paths().journal, "selftest 26.10.4");
    h.oldExits = false;
    std::vector<std::string> renamed;
    auto inner = h.ops.rename;
    h.ops.rename = [&](const fs::path& f, const fs::path& t, std::error_code& ec) {
        renamed.push_back(f.filename().string());
        inner(f, t, ec);
    };
    CHECK(run(h.plan, h.ops) == Outcome::OldStillRunning);
    CHECK(renamed.empty());
    CHECK(h.installed() == "good 26.10.4");   // untouched
    CHECK(fs::exists(h.paths().journal));     // still pending
    h.oldExits = true;
    run(h.plan, h.ops);   // the next run repairs, then updates as usual
    CHECK(!fs::exists(h.paths().journal));
}

void aHelperThatDiedMidSwapIsRecovered()
{
    {
        // Died after install -> .previous, before .new -> install.
        Harness h("died-swap");
        fs::rename(h.plan.install, h.paths().previous);
        fs::rename(h.plan.staged, h.paths().next);
        put(h.paths().journal, "swap-2 26.10.4");
        CHECK(recover(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!fs::exists(h.paths().next));
        CHECK(!fs::exists(h.paths().journal));
    }
    {
        // Died just after .new became the install, before the self-test was
        // journaled: untested, so the previous version goes back.
        Harness h("died-after-swap-2");
        put(h.plan.staged / "AetherSDR.exe", "broken");
        fs::rename(h.plan.install, h.paths().previous);
        fs::rename(h.plan.staged, h.plan.install);
        put(h.paths().journal, "swap-2 26.10.4");
        CHECK(recover(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(slurp(h.paths().failed / "AetherSDR.exe") == "broken");
    }
    {
        // Died during the self-test: the new release's health is unknown,
        // so the version that ran before goes back.
        Harness h("died-selftest");
        fs::rename(h.plan.install, h.paths().previous);
        fs::rename(h.plan.staged, h.plan.install);
        put(h.paths().journal, "selftest 26.10.4");
        CHECK(recover(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(slurp(h.paths().failed / "AetherSDR.exe") == "good 26.10.4");
    }
    {
        // No journal: nothing to do, nothing touched.
        Harness h("no-journal");
        CHECK(recover(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
    }
}

} // namespace

int main()
{
    cleanUpdate();
    onQuitTheAppIsNotStarted();
    anOlderPreviousIsReplaced();
    aBrokenReleaseIsRolledBack();
    aSelfTestThatHangsIsStoppedAndRolledBack();
    anOldBinaryCannotPassForTheNewOne();
    aRunningAppIsNeverTouched();
    preFlightFailuresChangeNothing();
    anotherVolumeIsCopiedBesideTheInstall();
    aBriefLockIsWaitedOut();
    aLockThatNeverClearsIsUndone();
    aFailedRollbackIsReportedNotHidden();
    aJournalThatCannotBeWrittenStopsTheStep();
    aPendingRepairWaitsForTheAppToExit();
    aHelperThatDiedMidSwapIsRecovered();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("update_swap_test: all checks passed");
    return 0;
}
