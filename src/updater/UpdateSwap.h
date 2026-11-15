#pragma once

// The folder swap AetherSDRUpdater.exe performs after AetherSDR exits (RFC
// #5304 Phase 3). Plain C++17 with no Qt and no project library: the helper
// runs while the install it came from is being renamed, so it may depend on
// nothing in that folder. Everything that touches processes is injected
// (Ops), so the whole state machine is exercised by update_swap_test on any
// platform.
//
// The swap, beside the install, on its own volume:
//
//   <install>.new       the staged release, renamed (or copied, across
//                       volumes) next to the install
//   <install>           renamed to <install>.previous, then .new takes its name
//   <install>.previous  kept: the one version a rollback restores
//   <install>.failed    a release that was rolled back (one kept, for logs)
//
// Windows has no atomic directory swap: between the install's rename and
// .new's, nothing runnable is at <install> for a moment. A journal in the
// state folder records each step before it is taken, so recover() can put
// the install back if the helper dies there. Because a missing install
// cannot start the app that would start the helper, the helper also arms a
// repair (on Windows a RunOnce entry: repair() at the next logon) before
// the first rename, and disarms it once nothing is owed.
//
// After the swap the new AetherSDR runs a self-test (--update-selftest
// <marker>): it starts Qt, loads its plugins and libraries, writes the
// marker and exits, with no window, no settings writes and no radio. That
// catches what a swap can break (a missing or bad DLL or plugin) and is safe
// after a normal quit, when opening the app again would be wrong. If the
// self-test exits without the marker, or does not finish in time, the
// previous version is restored. Only Restart to update then starts the app.
//
// The outcome is written to <stateDir>/swap-result.txt ("<outcome>
// <version>"), which the app reads at its next start: a rolled-back release
// is remembered there, so it is never staged again in a loop.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace AetherSDR::updater {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;

// The helper's command-line contract. The app that launches the helper is
// the running (old) version, so the version is checked first and anything
// else is refused rather than guessed at.
constexpr int kProtocol = 1;

struct Plan {
    fs::path install;    // the folder AetherSDR runs from
    fs::path staged;     // the checked new release (UpdateStager's output)
    fs::path stateDir;   // journal and health marker; outside the install
    fs::path exeName;    // "AetherSDR.exe", relative to install
    std::string version; // the release being installed, "26.10.4"
    bool relaunch = false; // Restart to update: start the app when done
    Duration exitTimeout = std::chrono::minutes(5);
    Duration renameTimeout = std::chrono::seconds(30);
    Duration healthTimeout = std::chrono::seconds(60);
};

enum class Outcome {
    Updated,           // the new version is in place and passed its self-test
    RolledBack,        // it failed the self-test; the previous version is back
    OldStillRunning,   // AetherSDR never exited; nothing was touched
    NotPossible,       // a check before any change failed; nothing was touched
    SwapUndone,        // a rename failed mid-swap and was undone
    Broken,            // a rollback rename failed: no runnable tree; the user must act
};

// The names swap-result.txt uses (UpdateInstaller reads them back).
inline const char* outcomeName(Outcome o)
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

// A started process, opaque to the core.
struct Process {
    std::shared_ptr<void> handle;
};

struct Ops {
    // Whether the old AetherSDR has exited within timeout.
    std::function<bool(Duration timeout)> waitForOldExit;
    // Starts exe with args (native strings, so a path keeps every
    // character on Windows); an empty handle when it could not.
    std::function<Process(const fs::path& exe, const std::vector<fs::path>& args)> launch;
    // Whether p has exited (non-blocking).
    std::function<bool(const Process& p)> hasExited;
    std::function<void(const Process& p)> terminate;
    std::function<void(Duration)> sleep;
    std::function<Clock::time_point()> now;
    // A line for the helper's log.
    std::function<void(const std::string&)> log;
    // Renames a folder; empty means std::filesystem::rename. Tests inject
    // the failures a real system produces (a locked folder, another volume).
    std::function<void(const fs::path& from, const fs::path& to, std::error_code& ec)> rename;
    // Writes the journal; empty means a write-then-rename to the real file.
    // Tests inject a failed write at a chosen step.
    std::function<bool(const fs::path& path, const std::string& text)> writeJournal;
    // Arms a repair that runs repair() even if nothing starts the app (the
    // next logon); true once it is in place. Disarmed when no repair is
    // owed. Both empty: no repair is armed (tests that don't look).
    std::function<bool()> armRepair;
    std::function<void()> disarmRepair;
    // A file's identity, kept across renames and new for a file written
    // again (on Windows the volume serial and NTFS file ID); empty when
    // unknown. The journal records the new release's AetherSDR.exe, so a
    // repair never rolls back a tree it did not leave (a reinstall).
    // Empty function: no identity check.
    std::function<std::string(const fs::path&)> fileId;
};

struct Paths {
    fs::path next;      // <install>.new
    fs::path previous;  // <install>.previous
    fs::path failed;    // <install>.failed
    fs::path journal;   // <stateDir>/swap-journal.txt
    fs::path marker;    // <stateDir>/selftest-<version>.txt
    fs::path result;    // <stateDir>/swap-result.txt
};

Paths pathsFor(const Plan& plan);

// A path as UTF-8, whatever the code page, for the log and the plan file.
std::string utf8(const fs::path& p);

// Puts the install back if a previous helper died mid-swap (the journal
// says so). Returns false only when it found a broken state it could not
// repair. Safe to call when there is no journal. moved (if given) says
// whether any folder was renamed.
bool recover(const Plan& plan, const Ops& ops, bool* moved = nullptr);

// The whole update: wait for the old process, check, swap, start, watch,
// and roll back when the new version does not come up.
Outcome run(const Plan& plan, const Ops& ops);

// The armed repair: recover() alone, with no swap (plan.staged is unused).
// It re-arms itself first, since Windows deletes a RunOnce value before
// running it and an interrupted repair must run again. While AetherSDR runs
// from the install (ops.waitForOldExit) nothing is renamed and it waits for
// the next logon. A repair that moved the install back records SwapUndone,
// never RolledBack, since the release was never tested. False while the
// install is still broken: the repair stays armed and the journal kept.
bool repair(const Plan& plan, const Ops& ops);

} // namespace AetherSDR::updater
