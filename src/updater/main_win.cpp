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

#include "UpdateSwap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

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

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::map<std::wstring, std::wstring> args;
    bool restart = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--restart") {
            restart = true;
        } else if (i + 1 < argc && a.rfind(L"--", 0) == 0) {
            args[a] = argv[++i];
        }
    }
    LocalFree(argv);

    // The contract first: an app of another protocol is refused, not guessed at.
    if (args[L"--protocol"] != std::to_wstring(kProtocol) || args[L"--wait-pid"].empty()
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
    Ops ops;
    ops.log = [&logFile](const std::string& line) { logFile << line << '\n' << std::flush; };
    ops.now = [] { return Clock::now(); };
    ops.sleep = [](Duration d) { Sleep(static_cast<DWORD>(d.count())); };
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
    const Outcome outcome = run(plan, ops);

    if (outcome == Outcome::Broken) {
        // Nothing runnable is in place and the helper could not fix it:
        // say where everything is rather than fail silently.
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
    return outcome == Outcome::Updated ? 0 : 1;
}
