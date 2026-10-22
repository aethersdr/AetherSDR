// Exercise the production discovery facade against HID symbols in this
// executable: no HID hardware or sockets. The same churn runs under TSan.
#include "core/HidDiscovery.h"
#include "core/RigExpertHidTransport.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_active{0};
std::atomic<int> g_overlaps{0};
std::atomic<int> g_errors{0};
wchar_t g_error[64] = L"";
bool g_reenter = false;
std::atomic<bool> g_openDevice{false};
std::atomic<bool> g_readFails{false};

struct Operation {
    Operation()
    {
        if (g_active.fetch_add(1) != 0) {
            ++g_overlaps;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ~Operation() { --g_active; }
};

} // namespace

extern "C" {

int HID_API_CALL hid_init()
{
    const Operation operation;
    return 0;
}

hid_device_info* HID_API_CALL hid_enumerate(unsigned short, unsigned short)
{
    if (g_reenter) {
        g_reenter = false;
        // macOS enumeration processes the calling thread's run loop.
        AetherSDR::HidDiscovery::init();
    }
    const Operation operation;
    std::wcscpy(g_error, L"enumeration error");
    return nullptr;
}

hid_device* HID_API_CALL hid_open(unsigned short, unsigned short, const wchar_t*)
{
    const Operation operation;
    std::wcscpy(g_error, L"open error");
    return nullptr;
}

hid_device* HID_API_CALL hid_open_path(const char*)
{
    const Operation operation;
    if (g_openDevice) { return reinterpret_cast<hid_device*>(1); }
    std::wcscpy(g_error, L"path error");
    errno = EACCES;
    return nullptr;
}

const wchar_t* HID_API_CALL hid_error(hid_device*)
{
    const Operation operation;
    // Error access can disturb errno; the facade must preserve the open's.
    errno = EINVAL;
    return g_error;
}

void HID_API_CALL hid_free_enumeration(hid_device_info*) {}
int HID_API_CALL hid_set_nonblocking(hid_device*, int) { return 0; }
int HID_API_CALL hid_read(hid_device*, unsigned char*, size_t) { return g_readFails ? -1 : 0; }
int HID_API_CALL hid_write(hid_device*, const unsigned char*, size_t) { return -1; }
void HID_API_CALL hid_close(hid_device*) {}

} // extern "C"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    constexpr int kWorkers = 4;
    constexpr int kIterations = 100;
    std::barrier start(kWorkers);
    std::vector<std::thread> workers;
    for (int n = 0; n < kWorkers; ++n) {
        workers.emplace_back([&start, n] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i) {
                if (n == 0) {
                    AetherSDR::RigExpertHidTransport::enumerate();
                    continue;
                }
                AetherSDR::HidDiscovery::init();
                AetherSDR::HidDiscovery::enumerate(0xffff, 0xffff);
                AetherSDR::HidDiscovery::open(0xffff, 0xffff, nullptr);
                std::wstring error;
                if (AetherSDR::HidDiscovery::openPath("absent", &error) != nullptr
                    || error != L"path error" || errno != EACCES) {
                    ++g_errors;
                }
            }
        });
    }
    // Exercise the production analyzer worker as guarded callers churn. Failed
    // opens also prove that the global error was copied while discovery was locked.
    for (int i = 0; i < kIterations; ++i) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        AetherSDR::RigExpertHidTransport* port = AetherSDR::RigExpertHidTransport::create(&app);
        int failures = 0;
        bool closed = false;
        QObject::connect(port, &AetherSDR::RigExpertHidTransport::failed, &loop, [&failures](const QString& error) {
            ++failures;
            if (error != QLatin1String("Cannot open Stick Pro: path error")) { ++g_errors; }
        });
        QObject::connect(port, &AetherSDR::RigExpertHidTransport::closed, &loop, [&] {
            closed = true;
            loop.quit();
        });
        deadline.start(3000);
        port->start(QStringLiteral("absent"));
        loop.exec();
        if (!closed || failures != 1) { ++g_errors; }
        port->deleteLater();
        if (!closed) { break; }
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    // A failed best-effort off write must not replace the original read error
    // or emit a second failure. These are host HID symbols, no device protocol.
    {
        g_openDevice = true;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        AetherSDR::RigExpertHidTransport* port = AetherSDR::RigExpertHidTransport::create(&app);
        int failures = 0;
        bool closed = false;
        QObject::connect(port, &AetherSDR::RigExpertHidTransport::opened, &loop, [] { g_readFails = true; });
        QObject::connect(port, &AetherSDR::RigExpertHidTransport::failed, &loop, [&failures](const QString& error) {
            ++failures;
            if (!error.startsWith(QLatin1String("HID read failed:"))) { ++g_errors; }
        });
        QObject::connect(port, &AetherSDR::RigExpertHidTransport::closed, &loop, [&] {
            closed = true;
            loop.quit();
        });
        deadline.start(3000);
        port->start(QStringLiteral("absent"));
        loop.exec();
        if (!closed || failures != 1) { ++g_errors; }
        port->deleteLater();
        g_openDevice = false;
    }
    g_reenter = true;
    AetherSDR::HidDiscovery::enumerate(0xffff, 0xffff);
    if (g_overlaps != 0 || g_errors != 0 || g_reenter) {
        std::fprintf(stderr, "FAIL: overlaps=%d, wrong errors=%d, reentry=%d\n",
                     g_overlaps.load(), g_errors.load(), g_reenter);
        return 1;
    }
    std::puts("PASS: discovery/open serialized; error copy, errno and reentry preserved");
}
