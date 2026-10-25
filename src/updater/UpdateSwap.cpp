#include "UpdateSwap.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace AetherSDR::updater {

namespace {

constexpr Duration kRetryStep{250};
constexpr Duration kPollStep{100};

// A path for the log, as UTF-8 whatever the code page.
std::string logText(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path normalised(const fs::path& p)
{
    fs::path n = p.lexically_normal();
    if (n.filename().empty()) {
        n = n.parent_path();   // a trailing separator
    }
    return n;
}

// Paths are joined as paths, never through std::string: on Windows that is
// the ANSI code page, and a name it cannot hold (漢字 on code page 1252)
// throws.
fs::path sibling(const fs::path& install, const char* suffix)
{
    const fs::path n = normalised(install);
    fs::path name = n.filename();
    name += suffix;
    return n.parent_path() / name;
}

bool writeText(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text;
        if (!out.flush()) {
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

bool present(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

bool isFolder(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec));
}

void removeAll(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

// A rename that waits out a brief lock (an Explorer window or an antivirus
// scan holding a handle inside the folder), up to timeout. Nothing is ever
// killed to make a rename succeed.
void doRename(const Ops& ops, const fs::path& from, const fs::path& to, std::error_code& ec)
{
    ec.clear();
    if (ops.rename) {
        ops.rename(from, to, ec);
    } else {
        fs::rename(from, to, ec);
    }
}

bool renameRetry(const fs::path& from, const fs::path& to, Duration timeout, const Ops& ops)
{
    const auto deadline = ops.now() + timeout;
    for (;;) {
        std::error_code ec;
        doRename(ops, from, to, ec);
        if (!ec) {
            return true;
        }
        if (ops.now() >= deadline) {
            ops.log("rename " + logText(from) + " -> " + logText(to) + " failed: " + ec.message());
            return false;
        }
        ops.sleep(kRetryStep);
    }
}

// The staged tree to <install>.new. A rename when both are on one volume;
// across volumes (per-user app data on C:, a portable copy on D:) a copy,
// so the swap itself is always a same-volume rename.
bool place(const fs::path& staged, const fs::path& next, const Ops& ops)
{
    std::error_code ec;
    doRename(ops, staged, next, ec);
    if (!ec) {
        return true;
    }
    // Only another volume is a reason to copy; a lock or a refusal is not.
    bool otherVolume = ec == std::errc::cross_device_link;
#ifdef _WIN32
    otherVolume = otherVolume || ec.value() == 17;   // ERROR_NOT_SAME_DEVICE
#endif
    if (!otherVolume) {
        ops.log("the staged tree could not be moved beside the install: " + ec.message());
        return false;
    }
    ops.log("staged tree is on another volume (" + ec.message() + "); copying it beside the install");
    removeAll(next);
    fs::copy(staged, next, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        ops.log("copy failed: " + ec.message());
        removeAll(next);
        return false;
    }
    removeAll(staged);
    return true;
}

// Whether the folder that holds the install accepts new entries.
bool parentWritable(const fs::path& install)
{
    const fs::path probe = sibling(install, ".write-probe");
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

// Records the step about to be taken. The step is taken only if this
// succeeds, so the journal on disk always describes a state the tree can be
// in.
bool journal(const Ops& ops, const Paths& paths, const std::string& step)
{
    const bool ok = ops.writeJournal ? ops.writeJournal(paths.journal, step) : writeText(paths.journal, step);
    if (!ok) {
        ops.log("could not write the swap journal (" + step + ")");
    }
    return ok;
}

Outcome finish(const Plan& plan, const Ops& ops, const Paths& paths, Outcome outcome, bool restart)
{
    // The journal is what a later run repairs from: it stays while the
    // swap is unfinished (Broken) or never started its repair because the
    // app was still running (OldStillRunning).
    if (outcome != Outcome::Broken && outcome != Outcome::OldStillRunning) {
        std::error_code ec;
        fs::remove(paths.journal, ec);
        if (ops.disarmRepair && !present(paths.journal)) {
            ops.disarmRepair();
        }
    }
    writeText(paths.result, std::string(outcomeName(outcome)) + " " + plan.version + "\n");
    ops.log(std::string("outcome: ") + outcomeName(outcome) + " " + plan.version);
    if (restart && plan.relaunch) {
        ops.launch(normalised(plan.install) / plan.exeName, {});
    }
    return outcome;
}

// Starts the installed tree's self-test and waits for its verdict.
bool selfTestPasses(const Plan& plan, const Ops& ops, const Paths& paths)
{
    std::error_code ec;
    fs::remove(paths.marker, ec);
    const Process p = ops.launch(normalised(plan.install) / plan.exeName,
                                 {fs::path("--update-selftest"), paths.marker});
    if (!p.handle) {
        ops.log("self-test could not be started");
        return false;
    }
    const auto deadline = ops.now() + plan.healthTimeout;
    for (;;) {
        if (ops.hasExited(p)) {
            // The marker names the version that wrote it: an old binary left
            // in place cannot pass for the new one.
            const bool ok = present(paths.marker) && readText(paths.marker) == plan.version;
            if (!ok) {
                ops.log("self-test exited without reporting " + plan.version);
            }
            return ok;
        }
        if (ops.now() >= deadline) {
            ops.log("self-test did not finish in time; stopping it");
            ops.terminate(p);
            return false;
        }
        ops.sleep(kPollStep);
    }
}

} // namespace

const char* outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Updated:         return "Updated";
    case Outcome::RolledBack:      return "RolledBack";
    case Outcome::OldStillRunning: return "OldStillRunning";
    case Outcome::NotPossible:     return "NotPossible";
    case Outcome::SwapUndone:      return "SwapUndone";
    case Outcome::Broken:          return "Broken";
    }
    return "Unknown";
}

Paths pathsFor(const Plan& plan)
{
    Paths p;
    p.next = sibling(plan.install, ".new");
    p.previous = sibling(plan.install, ".previous");
    p.failed = sibling(plan.install, ".failed");
    p.journal = plan.stateDir / "swap-journal.txt";
    p.marker = plan.stateDir / ("selftest-" + plan.version + ".txt");
    p.result = plan.stateDir / "swap-result.txt";
    return p;
}

bool recover(const Plan& plan, const Ops& ops)
{
    const Paths paths = pathsFor(plan);
    if (!present(paths.journal)) {
        return true;
    }
    const fs::path install = normalised(plan.install);
    const std::string step = readText(paths.journal);
    ops.log("found a swap journal (" + step + "); recovering");
    // A helper that died during the self-test or the rollback left a new
    // release whose health is unknown: the version that ran before is known
    // to work, so it goes back.
    // swap-2 too: the journal says swap-2 until the self-test starts, so a
    // helper that died just after .new became the install left it untested.
    if ((step.rfind("swap-2", 0) == 0 || step.rfind("selftest", 0) == 0
         || step.rfind("rollback", 0) == 0)
        && isFolder(install) && isFolder(paths.previous)) {
        removeAll(paths.failed);
        if (!renameRetry(install, paths.failed, plan.renameTimeout, ops)) {
            return false;
        }
    }
    // Whatever step a previous helper died in, the install must exist. The
    // version that was running before is preferred: it is known to work.
    if (!isFolder(install)) {
        if (isFolder(paths.previous)) {
            if (!renameRetry(paths.previous, install, plan.renameTimeout, ops)) {
                return false;
            }
        } else if (isFolder(paths.next)) {
            if (!renameRetry(paths.next, install, plan.renameTimeout, ops)) {
                return false;
            }
        } else {
            ops.log("no install and nothing to restore it from");
            return false;
        }
    }
    removeAll(paths.next);   // a half-placed tree is never trusted
    std::error_code ec;
    fs::remove(paths.journal, ec);
    return true;
}

bool repair(const Plan& plan, const Ops& ops)
{
    const Paths paths = pathsFor(plan);
    if (!present(paths.journal)) {
        if (ops.disarmRepair) {
            ops.disarmRepair();   // nothing owed
        }
        return true;
    }
    if (!recover(plan, ops)) {
        ops.log("the repair could not restore the install; it runs again at the next logon");
        if (ops.armRepair) {
            ops.armRepair();
        }
        return false;
    }
    writeText(paths.result, std::string(outcomeName(Outcome::SwapUndone)) + " " + plan.version + "\n");
    ops.log(std::string("outcome: ") + outcomeName(Outcome::SwapUndone) + " " + plan.version + " (repaired)");
    if (ops.disarmRepair) {
        ops.disarmRepair();
    }
    return true;
}

Outcome run(const Plan& plan, const Ops& ops)
{
    const Paths paths = pathsFor(plan);
    const fs::path install = normalised(plan.install);

    // Nothing is touched, not even a repair, while AetherSDR runs: a
    // journal left by an earlier helper waits for the next exit too.
    if (!ops.waitForOldExit(plan.exitTimeout)) {
        // Never kill it: the next quit tries again.
        return finish(plan, ops, paths, Outcome::OldStillRunning, false);
    }
    if (!recover(plan, ops)) {
        return finish(plan, ops, paths, Outcome::Broken, false);
    }

    // Checks before anything changes.
    if (!isFolder(install) || !present(install / plan.exeName) || !isFolder(plan.staged)
        || !present(plan.staged / plan.exeName) || present(paths.next) || !parentWritable(install)) {
        ops.log("pre-flight checks failed; nothing was changed");
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }

    // Each step is taken only once the journal records it. When a write
    // fails, the step is not taken and what was done so far is undone, so
    // the last journal that did reach the disk still matches the tree.
    if (!journal(ops, paths, "placing " + plan.version)) {
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }
    if (!place(plan.staged, paths.next, ops)) {
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }

    // From the first rename on, a helper that dies may leave no install to
    // start: the repair is armed first, or nothing is renamed.
    if (ops.armRepair && !ops.armRepair()) {
        ops.log("could not arm the repair; nothing was changed");
        removeAll(paths.next);
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }
    if (!journal(ops, paths, "swap-1 " + plan.version)) {
        removeAll(paths.next);   // the install is untouched
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }
    // Only one previous version is kept, and the older one goes only now
    // the swap is recorded.
    removeAll(paths.previous);
    if (!renameRetry(install, paths.previous, plan.renameTimeout, ops)) {
        removeAll(paths.next);
        return finish(plan, ops, paths, Outcome::SwapUndone, true);
    }

    // From here on, undoing means putting .previous back.
    const auto undo = [&]() {
        if (!renameRetry(paths.previous, install, plan.renameTimeout, ops)) {
            return finish(plan, ops, paths, Outcome::Broken, false);
        }
        removeAll(paths.next);
        return finish(plan, ops, paths, Outcome::SwapUndone, true);
    };
    if (!journal(ops, paths, "swap-2 " + plan.version)) {
        return undo();
    }
    if (!renameRetry(paths.next, install, plan.renameTimeout, ops)) {
        return undo();
    }

    // The new tree is in place, untested. Both failures below end the same
    // way, by restoring .previous.
    const auto rollBack = [&](Outcome outcome) {
        // Taken even if this journal write fails: the one already on disk
        // (swap-2 or selftest) makes recover() roll back the same way.
        journal(ops, paths, "rollback " + plan.version);
        removeAll(paths.failed);
        if (!renameRetry(install, paths.failed, plan.renameTimeout, ops)
            || !renameRetry(paths.previous, install, plan.renameTimeout, ops)) {
            return finish(plan, ops, paths, Outcome::Broken, false);
        }
        return finish(plan, ops, paths, outcome, true);
    };
    if (!journal(ops, paths, "selftest " + plan.version)) {
        return rollBack(Outcome::SwapUndone);   // never tested, so never kept
    }
    if (selfTestPasses(plan, ops, paths)) {
        std::error_code ec;
        fs::remove(paths.marker, ec);
        return finish(plan, ops, paths, Outcome::Updated, true);
    }
    return rollBack(Outcome::RolledBack);
}

} // namespace AetherSDR::updater
