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
    std::errc renameError = std::errc::permission_denied;   // what a failing rename reports
    std::string journalFailsAt;                  // a step ("swap-1") whose journal write fails
    std::vector<std::string> logLines;
    bool armed = false;      // the RunOnce repair
    bool armFails = false;
    int arms = 0;

    explicit Harness(const std::string& name)
    {
        root = fs::temp_directory_path() / fs::u8path("update-swap-test-" + name);
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
        ops.armRepair = [this] {
            if (armFails) {
                return false;
            }
            armed = true;
            ++arms;
            return true;
        };
        ops.disarmRepair = [this] { armed = false; };
        // A rename keeps a file's size and time; a file written again gets
        // a new time (reinstalled() below sets one).
        ops.fileId = [](const fs::path& p) {
            std::error_code ec;
            const auto t = fs::last_write_time(p, ec);
            if (ec) {
                return std::string();
            }
            return std::to_string(fs::file_size(p, ec)) + "-"
                   + std::to_string(static_cast<long long>(t.time_since_epoch().count()));   // libc++: __int128
        };
        ops.rename = [this](const fs::path& from, const fs::path& to, std::error_code& ec) {
            auto it = renameFailures.find(from.filename().string());
            if (it != renameFailures.end() && it->second != 0) {
                if (it->second > 0) {
                    --it->second;
                }
                ec = std::make_error_code(renameError);
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
    h.renameError = std::errc::cross_device_link;
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.installed() == "good 26.10.4");
    CHECK(!fs::exists(h.plan.staged));   // the copy's source is cleaned up
}

void aLockedStagedTreeIsNotCopied()
{
    // Review of #6767: only another volume is a reason to copy.
    Harness h("staged-locked");
    h.renameFailures["26.10.4"] = 1;
    CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
    CHECK(h.installed() == "good 26.10.3");
    CHECK(fs::exists(h.plan.staged / "AetherSDR.exe"));
    CHECK(!fs::exists(h.paths().next));
    bool copied = false;
    for (const std::string& line : h.logLines) {
        copied = copied || line.find("another volume") != std::string::npos;
    }
    CHECK(!copied);
}

void pathsOutsideTheCodePageAreSwapped()
{
    // The Windows run of #6767: a path under 漢字 crashed the helper,
    // which joined paths through the ANSI code page.
    // Escaped, so the name is these characters whatever the compiler's
    // source code page.
    Harness h(u8"\u6f22\u5b57 \u00dcn\u00efc\u00f8d\u00e9");
    h.plan.relaunch = true;
    CHECK(run(h.plan, h.ops) == Outcome::Updated);
    CHECK(h.installed() == "good 26.10.4");
    CHECK(slurp(h.paths().previous / "AetherSDR.exe") == "good 26.10.3");
    CHECK(h.paths().next.filename().u8string() == u8"AetherSDR.new");
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
    // The older previous version goes only once swap-1 is recorded.
    Harness h("journal-keeps-previous");
    put(h.paths().previous / "AetherSDR.exe", "good 26.10.2");
    h.journalFailsAt = "swap-1";
    CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
    CHECK(slurp(h.paths().previous / "AetherSDR.exe") == "good 26.10.2");
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
    // The next run repairs; the staged tree was consumed by the first
    // attempt, so there is nothing more to install.
    CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
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


void aRepairIsArmedForTheSwapAndClearedAfter()
{
    {
        Harness h("arm-updated");
        CHECK(run(h.plan, h.ops) == Outcome::Updated);
        CHECK(h.arms == 1);
        CHECK(!h.armed);
    }
    {
        // Armed before anything is renamed: no rename precedes it.
        Harness h("arm-first");
        bool renamedBeforeArm = false;
        auto inner = h.ops.rename;
        h.ops.rename = [&](const fs::path& f, const fs::path& t, std::error_code& ec) {
            if (!h.armed && f.filename() == "AetherSDR") {
                renamedBeforeArm = true;
            }
            inner(f, t, ec);
        };
        CHECK(run(h.plan, h.ops) == Outcome::Updated);
        CHECK(!renamedBeforeArm);
    }
    {
        // A repair that cannot be armed means no swap at all.
        Harness h("arm-fails");
        h.armFails = true;
        CHECK(run(h.plan, h.ops) == Outcome::NotPossible);
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!fs::exists(h.paths().previous));
        CHECK(!fs::exists(h.paths().next));
        CHECK(h.launches.empty());
    }
    {
        // Still armed while a repair is owed.
        Harness h("arm-broken");
        h.renameFailures["AetherSDR.previous"] = -1;
        put(h.plan.staged / "AetherSDR.exe", "broken");
        CHECK(run(h.plan, h.ops) == Outcome::Broken);
        CHECK(h.armed);
    }
}

void theArmedRepairRestoresAnInstallNothingCanStart()
{
    {
        // Blocker 2 of the #6767 review: died between the two renames, so
        // there is no AetherSDR.exe to start. The logon repair puts the
        // previous version back.
        Harness h("repair-between");
        fs::rename(h.plan.install, h.paths().previous);
        fs::rename(h.plan.staged, h.paths().next);
        put(h.paths().journal, "swap-1 26.10.4");
        h.armed = true;
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!fs::exists(h.paths().next));
        CHECK(!fs::exists(h.paths().journal));
        CHECK(h.result() == "SwapUndone 26.10.4\n");   // never RolledBack: it was never tested
        CHECK(!h.armed);
    }
    {
        // Nothing owed: it only disarms.
        Harness h("repair-nothing");
        h.armed = true;
        CHECK(repair(h.plan, h.ops));
        CHECK(!h.armed);
        CHECK(h.result().empty());
        CHECK(h.installed() == "good 26.10.3");
    }
    {
        // A repair that cannot finish stays armed for the next logon.
        Harness h("repair-locked");
        fs::rename(h.plan.install, h.paths().previous);
        put(h.paths().journal, "swap-1 26.10.4");
        h.renameFailures["AetherSDR.previous"] = -1;
        CHECK(!repair(h.plan, h.ops));
        CHECK(h.armed);
        CHECK(fs::exists(h.paths().journal));
        h.renameFailures.clear();
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!h.armed);
    }
}

// The state a helper that died mid-self-test leaves: the new release at
// <install> (renamed, so the same file), the old one at .previous, and the
// journal naming the file it left.
void diedInSelfTest(Harness& h)
{
    const std::string id = h.ops.fileId(h.plan.staged / "AetherSDR.exe");
    fs::rename(h.plan.install, h.paths().previous);
    fs::rename(h.plan.staged, h.plan.install);
    put(h.paths().journal, "selftest 26.10.4 id=" + id);
}

void aReinstallIsNeverRolledBack()
{
    {
        // The tree the swap left, untested: rolled back.
        Harness h("ours-rolled-back");
        diedInSelfTest(h);
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(h.result() == "SwapUndone 26.10.4\n");
    }
    {
        // Review of #6767: the user reinstalled after the Broken message.
        // The logon repair keeps the fresh install and moves nothing.
        Harness h("reinstalled");
        diedInSelfTest(h);
        const fs::path exe = h.plan.install / "AetherSDR.exe";
        fs::remove(exe);
        put(exe, "good 26.10.4");
        fs::last_write_time(exe, fs::last_write_time(exe) + std::chrono::hours(1));
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.4");
        CHECK(!fs::exists(h.paths().failed));
        CHECK(!fs::exists(h.paths().journal));
        CHECK(h.result().empty());   // nothing was undone
        CHECK(!h.armed);
    }
    {
        // A journal written by a real swap carries the id.
        Harness h("journal-id");
        std::vector<std::string> steps;
        h.ops.writeJournal = [&](const fs::path& path, const std::string& text) {
            steps.push_back(text);
            put(path, text);
            return true;
        };
        CHECK(run(h.plan, h.ops) == Outcome::Updated);
        bool tagged = false;
        for (const std::string& st : steps) {
            tagged = tagged || (st.rfind("selftest 26.10.4 id=", 0) == 0 && st.size() > 20);
        }
        CHECK(tagged);
    }
}

void theRepairRearmsWaitsAndOnlyReportsWhatItMoved()
{
    {
        // Re-armed first: a repair cut short runs again.
        Harness h("repair-rearm");
        diedInSelfTest(h);
        bool armedBeforeRename = false;
        auto inner = h.ops.rename;
        h.ops.rename = [&](const fs::path& f, const fs::path& t, std::error_code& ec) {
            armedBeforeRename = armedBeforeRename || h.armed;
            inner(f, t, ec);
        };
        CHECK(repair(h.plan, h.ops));
        CHECK(armedBeforeRename);
        CHECK(!h.armed);
    }
    {
        // AetherSDR already running from the install at logon: nothing is
        // renamed, nothing is reported broken, and it stays armed.
        Harness h("repair-app-running");
        diedInSelfTest(h);
        h.oldExits = false;
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.4");
        CHECK(fs::exists(h.paths().journal));
        CHECK(h.armed);
        CHECK(h.result().empty());
    }
    {
        // Died after swap-1 was journaled but before the rename: the
        // install is whole, so only the stray .new goes and nothing is
        // reported undone.
        Harness h("repair-nothing-moved");
        fs::rename(h.plan.staged, h.paths().next);
        put(h.paths().journal, "swap-1 26.10.4 id=x");
        CHECK(repair(h.plan, h.ops));
        CHECK(h.installed() == "good 26.10.3");
        CHECK(!fs::exists(h.paths().next));
        CHECK(h.result().empty());
    }
}

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
    aLockedStagedTreeIsNotCopied();
    pathsOutsideTheCodePageAreSwapped();
    aBriefLockIsWaitedOut();
    aLockThatNeverClearsIsUndone();
    aFailedRollbackIsReportedNotHidden();
    aJournalThatCannotBeWrittenStopsTheStep();
    aPendingRepairWaitsForTheAppToExit();
    aHelperThatDiedMidSwapIsRecovered();
    aRepairIsArmedForTheSwapAndClearedAfter();
    theArmedRepairRestoresAnInstallNothingCanStart();
    aReinstallIsNeverRolledBack();
    theRepairRearmsWaitsAndOnlyReportsWhatItMoved();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("update_swap_test: all checks passed");
    return 0;
}
