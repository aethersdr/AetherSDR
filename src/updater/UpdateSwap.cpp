#include "UpdateSwap.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace AetherSDR::updater {

namespace {

constexpr Duration kRetryStep{250};
constexpr Duration kPollStep{100};

fs::path normalised(const fs::path& p)
{
    fs::path n = p.lexically_normal();
    if (n.filename().empty()) {
        n = n.parent_path();   // a trailing separator
    }
    return n;
}

fs::path sibling(const fs::path& install, const char* suffix)
{
    const fs::path n = normalised(install);
    return n.parent_path() / (n.filename().string() + suffix);
}

bool writeText(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
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
            ops.log("rename " + from.string() + " -> " + to.string() + " failed: " + ec.message());
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

Outcome finish(const Plan& plan, const Ops& ops, const Paths& paths, Outcome outcome, bool restart)
{
    std::error_code ec;
    fs::remove(paths.journal, ec);
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
    if ((step.rfind("selftest", 0) == 0 || step.rfind("rollback", 0) == 0) && isFolder(install)
        && isFolder(paths.previous)) {
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

Outcome run(const Plan& plan, const Ops& ops)
{
    const Paths paths = pathsFor(plan);
    const fs::path install = normalised(plan.install);

    if (!recover(plan, ops)) {
        return finish(plan, ops, paths, Outcome::Broken, false);
    }
    if (!ops.waitForOldExit(plan.exitTimeout)) {
        // Never kill it: the next quit tries again.
        return finish(plan, ops, paths, Outcome::OldStillRunning, false);
    }

    // Checks before anything changes.
    if (!isFolder(install) || !present(install / plan.exeName) || !isFolder(plan.staged)
        || !present(plan.staged / plan.exeName) || present(paths.next) || !parentWritable(install)) {
        ops.log("pre-flight checks failed; nothing was changed");
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }

    writeText(paths.journal, "placing " + plan.version);
    if (!place(plan.staged, paths.next, ops)) {
        return finish(plan, ops, paths, Outcome::NotPossible, true);
    }

    // Only one previous version is kept.
    removeAll(paths.previous);
    writeText(paths.journal, "swap-1 " + plan.version);
    if (!renameRetry(install, paths.previous, plan.renameTimeout, ops)) {
        removeAll(paths.next);
        return finish(plan, ops, paths, Outcome::SwapUndone, true);
    }
    writeText(paths.journal, "swap-2 " + plan.version);
    if (!renameRetry(paths.next, install, plan.renameTimeout, ops)) {
        if (!renameRetry(paths.previous, install, plan.renameTimeout, ops)) {
            return finish(plan, ops, paths, Outcome::Broken, false);
        }
        removeAll(paths.next);
        return finish(plan, ops, paths, Outcome::SwapUndone, true);
    }

    writeText(paths.journal, "selftest " + plan.version);
    if (selfTestPasses(plan, ops, paths)) {
        std::error_code ec;
        fs::remove(paths.marker, ec);
        return finish(plan, ops, paths, Outcome::Updated, true);
    }

    // Roll back: the failed release aside (one kept), the previous one back.
    writeText(paths.journal, "rollback " + plan.version);
    removeAll(paths.failed);
    if (!renameRetry(install, paths.failed, plan.renameTimeout, ops)
        || !renameRetry(paths.previous, install, plan.renameTimeout, ops)) {
        return finish(plan, ops, paths, Outcome::Broken, false);
    }
    return finish(plan, ops, paths, Outcome::RolledBack, true);
}

} // namespace AetherSDR::updater
