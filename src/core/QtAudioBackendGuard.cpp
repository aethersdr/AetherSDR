#include "QtAudioBackendGuard.h"

#include <QByteArray>

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#endif

namespace AetherSDR {

namespace {

// Decided before logging exists; reported later by appliedSummary().
QString g_summary = QStringLiteral("not evaluated");

#ifdef Q_OS_LINUX
// Mirrors the calls Qt's PipeWire backend makes to get a client context: init
// the library, make a loop, make a context. Opaque pointers only — the probe
// needs no PipeWire headers, so it builds wherever the app does.
//
// The library is left initialised and loaded afterwards, never pw_deinit()ed
// or dlclose()d: Qt initialises and loads the same libpipewire moments later,
// pw_init() returns early when already initialised, and tearing the library
// down under it buys nothing (the same reasoning as the X11 handler in main.cpp).
bool pipewireContextUsable(void* lib)
{
    using InitFn = void (*)(int*, char***);
    using LoopNewFn = void* (*)(const void*);
    using LoopDestroyFn = void (*)(void*);
    using ContextNewFn = void* (*)(void*, void*, size_t);
    using ContextDestroyFn = void (*)(void*);

    auto init = reinterpret_cast<InitFn>(dlsym(lib, "pw_init"));
    auto loopNew = reinterpret_cast<LoopNewFn>(dlsym(lib, "pw_loop_new"));
    auto loopDestroy = reinterpret_cast<LoopDestroyFn>(dlsym(lib, "pw_loop_destroy"));
    auto contextNew = reinterpret_cast<ContextNewFn>(dlsym(lib, "pw_context_new"));
    auto contextDestroy = reinterpret_cast<ContextDestroyFn>(dlsym(lib, "pw_context_destroy"));
    if (!init || !loopNew || !loopDestroy || !contextNew || !contextDestroy) {
        // A libpipewire without the API Qt calls: Qt will not get a context
        // from it either.
        return false;
    }

    init(nullptr, nullptr);
    void* loop = loopNew(nullptr);
    void* context = loop ? contextNew(loop, nullptr, 0) : nullptr;
    const bool usable = context != nullptr;
    if (context)
        contextDestroy(context);
    if (loop)
        loopDestroy(loop);
    return usable;
}
#endif

} // namespace

void QtAudioBackendGuard::applyAtStartup()
{
#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsSet("QT_AUDIO_BACKEND")) {
        g_summary = QStringLiteral("QT_AUDIO_BACKEND=%1 (set by the user; not probed)")
                        .arg(QString::fromLocal8Bit(qgetenv("QT_AUDIO_BACKEND")));
        return;
    }
    void* lib = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        g_summary = QStringLiteral("libpipewire not present; Qt chooses its own backend");
        return;
    }
    if (pipewireContextUsable(lib)) {
        g_summary = QStringLiteral("PipeWire context OK; Qt's default backend left in place");
        return;
    }
    qputenv("QT_AUDIO_BACKEND", "pulseaudio");
    g_summary = QStringLiteral(
        "PipeWire context could not be created (no client configuration?); "
        "QT_AUDIO_BACKEND=pulseaudio so Qt 6.12 does not crash enumerating audio devices");
#else
    g_summary = QStringLiteral("not applicable on this platform");
#endif
}

QString QtAudioBackendGuard::appliedSummary()
{
    return g_summary;
}

} // namespace AetherSDR
