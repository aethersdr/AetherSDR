// AetherSDRUpdater.exe (RFC #5304 Phase 3): swaps a staged AetherSDR release
// into place after AetherSDR exits, checks it with the new version's
// self-test, and rolls back when it fails. The swap itself is UpdateSwap;
// this file is only its Windows side: arguments, the old process, the
// processes it starts, a log, and a message when nothing can be fixed.
//
// Built with the static MSVC runtime and no Qt, so it loads nothing from the
// install folder it renames. AetherSDR copies it out of the install before
// starting it (UpdateInstaller).
//
//   AetherSDRUpdater.exe --protocol 1 --wait-pid <pid> --install <dir>
//                        --staged <dir> --state-dir <dir> --version <x.y.z>
//                        [--restart]
//   AetherSDRUpdater.exe --protocol 1 --repair --state-dir <dir>
//
// --repair is what the RunOnce entry armed before the first rename runs at
// the next logon: repair() from the plan the swap left in the state folder,
// for when the swap died with no install left to start AetherSDR from.

#include "UpdateSwap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fstream>
#include <map>
#include <string>

using namespace AetherSDR::updater;

namespace {

std::wstring quoted(const std::wstring& arg)
{
    // The inverse of CommandLineToArgvW: backslashes are literal except
    // before a quote, where each is doubled and the quote escaped.
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');   // before the closing quote
    out.push_back(L'"');
    return out;
}

// The journal, durably: written and flushed to the disk (not just to the
// OS), then moved over the old one with the move itself written through, so
// a power cut leaves either record whole.
bool writeDurably(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
                    && written == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

const wchar_t* const kRunOnceKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
const wchar_t* const kRunOnceValue = L"AetherSDRUpdateRepair";
const wchar_t* const kRepairPlan = L"repair-plan.txt";

fs::path selfPath()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

// The volume serial and NTFS file ID: kept by a rename, new for a file
// written again.
std::string fileId(const fs::path& p)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) {
        return {};
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"%08lx-%08lx%08lx", info.dwVolumeSerialNumber, info.nFileIndexHigh,
             info.nFileIndexLow);
    return utf8(fs::path(buf));
}

// Whether an AetherSDR.exe is running from the install (the repair has no
// process id to wait on).
bool runningFrom(const fs::path& install)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::wstring dir = install.lexically_normal().wstring();
    bool found = false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL more = Process32FirstW(snap, &e); more && !found; more = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"AetherSDR.exe") != 0) {
            continue;
        }
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!p) {
            continue;
        }
        wchar_t image[32768];
        DWORD size = static_cast<DWORD>(std::size(image));
        if (QueryFullProcessImageNameW(p, 0, image, &size)) {
            const std::wstring parent = fs::path(image).parent_path().lexically_normal().wstring();
            found = _wcsicmp(parent.c_str(), dir.c_str()) == 0;
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}

// The repair's plan beside the helper, then the RunOnce entry that starts
// it; only both together are an armed repair.
bool armRepair(const Plan& plan)
{
    const fs::path self = selfPath();
    const std::string record = utf8(plan.install) + "\n" + plan.version + "\n";
    if (self.empty() || !writeDurably(plan.stateDir / kRepairPlan, record)) {
        return false;
    }
    const std::wstring command = quoted(self.wstring()) + L" --protocol " + std::to_wstring(kProtocol)
                                 + L" --repair --state-dir " + quoted(plan.stateDir.wstring());
    if (command.size() >= MAX_PATH) {
        return false;   // RunOnce runs no longer command line; nothing is swapped then
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(key, kRunOnceValue, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(command.c_str()),
                                  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

void disarmRepair(const Plan& plan)
{
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kRunOnceValue);
    std::error_code ec;
    fs::remove(plan.stateDir / kRepairPlan, ec);
}

// The plan armRepair() left: the install's path and the version, UTF-8.
bool readRepairPlan(Plan* plan)
{
    std::ifstream in(plan->stateDir / kRepairPlan, std::ios::binary);
    std::string install;
    std::string version;
    if (!std::getline(in, install) || !std::getline(in, version) || install.empty()
        || version.empty()) {
        return false;
    }
    plan->install = fs::u8path(install);
    plan->version = version;
    return true;
}

void showBroken(const Plan& plan)
{
    // Nothing runnable is in place and the helper could not fix it: say
    // where everything is rather than fail silently.
    const Paths paths = pathsFor(plan);
    const std::wstring text =
        L"AetherSDR could not finish updating and could not restore the previous version.\n\n"
        L"Install folder: " + plan.install.wstring() + L"\n"
        L"Previous version: " + paths.previous.wstring() + L"\n"
        L"New version: " + paths.next.wstring() + L"\n\n"
        L"Rename the previous version's folder back to the install folder's name, "
        L"or reinstall AetherSDR. Details: " + (plan.stateDir / "updater.log").wstring();
    MessageBoxW(nullptr, text.c_str(), L"AetherSDR update", MB_OK | MB_ICONERROR);
}

// What a swap and a repair share. plan must outlive the result.
Ops baseOps(const Plan& plan, std::ofstream& logFile)
{
    Ops ops;
    ops.log = [&logFile](const std::string& line) { logFile << line << '\n' << std::flush; };
    ops.now = [] { return Clock::now(); };
    ops.sleep = [](Duration d) { Sleep(static_cast<DWORD>(d.count())); };
    ops.writeJournal = writeDurably;
    ops.armRepair = [&plan] { return armRepair(plan); };
    ops.disarmRepair = [&plan] { disarmRepair(plan); };
    ops.fileId = fileId;
    return ops;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::map<std::wstring, std::wstring> args;
    bool restart = false;
    bool repairOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--restart") {
            restart = true;
        } else if (a == L"--repair") {
            repairOnly = true;
        } else if (i + 1 < argc && a.rfind(L"--", 0) == 0) {
            args[a] = argv[++i];
        }
    }
    LocalFree(argv);

    // The contract first: an app of another protocol is refused, not guessed at.
    if (args[L"--protocol"] != std::to_wstring(kProtocol)) {
        return 2;
    }
    if (repairOnly) {
        Plan plan;
        plan.stateDir = args[L"--state-dir"].empty() ? selfPath().parent_path()
                                                      : fs::path(args[L"--state-dir"]);
        plan.exeName = L"AetherSDR.exe";
        std::ofstream logFile(plan.stateDir / "updater.log", std::ios::app);
        Ops ops = baseOps(plan, logFile);
        if (!readRepairPlan(&plan)) {
            ops.log("AetherSDRUpdater: no repair plan; nothing to repair");
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kRunOnceValue);
            return 0;
        }
        // No process id to wait on: the app counts as running while any
        // AetherSDR.exe runs from the install.
        ops.waitForOldExit = [&ops, &plan](Duration timeout) {
            const auto deadline = Clock::now() + timeout;
            while (runningFrom(plan.install)) {
                if (Clock::now() >= deadline) {
                    return false;
                }
                ops.sleep(Duration(1000));
            }
            return true;
        };
        ops.log("AetherSDRUpdater: repairing the install after an interrupted update to " + plan.version);
        bool repaired = false;
        try {
            repaired = repair(plan, ops);
        } catch (const std::exception& e) {
            ops.log(std::string("the repair stopped on an error: ") + e.what());
            armRepair(plan);
        }
        if (!repaired) {
            showBroken(plan);
        }
        return repaired ? 0 : 1;
    }
    if (args[L"--wait-pid"].empty()
        || args[L"--install"].empty() || args[L"--staged"].empty() || args[L"--state-dir"].empty()
        || args[L"--version"].empty()) {
        return 2;
    }

    Plan plan;
    plan.install = args[L"--install"];
    plan.staged = args[L"--staged"];
    plan.stateDir = args[L"--state-dir"];
    plan.exeName = L"AetherSDR.exe";
    for (wchar_t c : args[L"--version"]) {
        plan.version.push_back(static_cast<char>(c));   // digits and dots only
    }
    plan.relaunch = restart;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(args[L"--wait-pid"].c_str(), nullptr, 10));

    std::ofstream logFile(plan.stateDir / "updater.log", std::ios::app);
    Ops ops = baseOps(plan, logFile);
    ops.waitForOldExit = [pid](Duration timeout) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) {
            return true;   // already gone
        }
        const DWORD r = WaitForSingleObject(h, static_cast<DWORD>(timeout.count()));
        CloseHandle(h);
        return r == WAIT_OBJECT_0;
    };
    ops.launch = [&plan](const fs::path& exe, const std::vector<fs::path>& extra) {
        std::wstring cmd = quoted(exe.wstring());
        for (const fs::path& a : extra) {
            cmd += L' ' + quoted(a.wstring());
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring dir = plan.install.wstring();
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            dir.c_str(), &si, &pi)) {
            return Process{};
        }
        CloseHandle(pi.hThread);
        return Process{std::shared_ptr<void>(pi.hProcess, [](void* h) { CloseHandle(h); })};
    };
    ops.hasExited = [](const Process& p) {
        return WaitForSingleObject(static_cast<HANDLE>(p.handle.get()), 0) == WAIT_OBJECT_0;
    };
    ops.terminate = [](const Process& p) {
        TerminateProcess(static_cast<HANDLE>(p.handle.get()), 1);
        WaitForSingleObject(static_cast<HANDLE>(p.handle.get()), 5000);
    };

    ops.log("AetherSDRUpdater: installing " + plan.version);
    Outcome outcome = Outcome::Broken;
    try {
        outcome = run(plan, ops);
    } catch (const std::exception& e) {
        // Never a silent crash: put the known-good install back the way a
        // later run would, and say so. Only an install still missing is
        // Broken; a swap that had already finished is left as it reported.
        ops.log(std::string("the swap stopped on an error: ") + e.what());
        try {
            const Paths paths = pathsFor(plan);
            std::error_code ec;
            if (!fs::exists(paths.journal, ec) && fs::exists(paths.result, ec)) {
                ops.log("the swap had already finished; its result stands");
                outcome = Outcome::NotPossible;   // not Broken: no message
            } else {
                const bool owed = fs::exists(paths.journal, ec);
                const bool restored = recover(plan, ops);
                const bool runnable = restored && fs::exists(plan.install / plan.exeName, ec);
                outcome = !runnable ? Outcome::Broken : owed ? Outcome::SwapUndone : Outcome::NotPossible;
                std::ofstream(paths.result, std::ios::trunc)
                    << outcomeName(outcome) << ' ' << plan.version << '\n';
                ops.log(std::string("outcome: ") + outcomeName(outcome) + " " + plan.version);
                if (runnable) {
                    disarmRepair(plan);
                    if (plan.relaunch) {
                        ops.launch(plan.install / plan.exeName, {});
                    }
                }
            }
        } catch (const std::exception& again) {
            ops.log(std::string("the repair stopped on an error too: ") + again.what());
        }
    }

    if (outcome == Outcome::Broken) {
        showBroken(plan);
    }
    return outcome == Outcome::Updated ? 0 : 1;
}
