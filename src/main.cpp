#include "gui/MainWindow.h"
#include "gui/ConnectionPanel.h"
#include "gui/FramelessMessageBox.h"
#include "gui/SliceColorManager.h"
#include "core/AppSettings.h"
#include "core/SystemInfo.h"
#include "core/SettingsBootstrap.h"
#include "core/SettingsCredentialPolicy.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"
#include "core/SettingsSanitizer.h"
#include "models/Nr2SettingsModel.h"
#include "core/AutomationBridgeSettings.h"
#include "core/DisplayPresence.h"
#include "core/GpuSelector.h"
#include "core/QtAudioBackendGuard.h"
#include "core/LogManager.h"
#include "core/ShutdownTrace.h"
#include "core/SystemInventory.h"
#include "core/MacMicPermission.h"
#include "core/AutomationServer.h"
#include "QtPlatformChoice.h"

#ifdef Q_OS_MAC
#include "MacStartupAbortGuard.h"
#endif

#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QApplication>
#include <QGuiApplication>
#include <QStringList>
#include <QSurfaceFormat>
#include <memory>
#include <optional>
#include <QStyleFactory>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QDateTime>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

#ifdef __linux__
#include <dlfcn.h>

// Minimal forward declarations matching the Xlib error-handler ABI.
// Field order must match X11/Xlib.h's XErrorEvent exactly — otherwise
// ev->error_code etc. read the wrong bytes.
struct AetherX11Display;
struct AetherX11ErrorEvent {
    int               type;
    AetherX11Display* display;      // Display the event was read from
    unsigned long     resourceid;   // XID of failed resource
    unsigned long     serial;       // serial of failed request
    unsigned char     error_code;   // BadAccess == 10
    unsigned char     request_code; // major opcode
    unsigned char     minor_code;
};

// Tolerant X11 error handler — logs errors instead of aborting.
// On systems with non-free FFmpeg (e.g. openSUSE Packman), Qt Multimedia's
// FFmpeg backend may trigger X11 hardware-acceleration probing that causes a
// BadAccess error, even under native Wayland.  Xlib's default handler calls
// exit() on any error; ours logs and continues.  AetherSDR only uses Qt
// Multimedia for audio device enumeration and PCM I/O, so X11 errors from
// video hwaccel probing are harmless.  (#1839)
static int aetherTolerantX11ErrorHandler(AetherX11Display*, AetherX11ErrorEvent* ev)
{
    qWarning("Non-fatal X11 error suppressed (error_code=%d, request=%d) — "
             "see issue #1839",
             ev ? ev->error_code : -1,
             ev ? ev->request_code : -1);
    return 0;
}
#endif  // __linux__

#ifdef Q_OS_WIN
#include <windows.h>
// Request discrete GPU on hybrid laptops (NVIDIA Optimus / AMD PowerXpress).
// Intel iGPU D3D11 driver corrupts its stack during QRhiWidget reparenting (#1921).
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement             = 1;
__declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}
#endif // Q_OS_WIN

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    AetherSDR::LogManager::instance().enqueueMessage(type, ctx, msg);
}

// ── `AetherSDR --config` — settings CLI (RFC #4603 proposal D MVO) ───────────
// Show/create/delete/export settings and exit, before any GUI initialization.
// This is the recovery path when a stored value prevents the GUI from starting
// (e.g. a broken off-screen geometry): it needs a console, not a window.
// Note: a concurrently RUNNING AetherSDR holds its own in-memory cache and
// will not see CLI edits until restarted.
static int runConfigCli(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AetherSDR"));
    QCoreApplication::setOrganizationName(QStringLiteral("AetherSDR"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHERSDR_VERSION));

    const QStringList args = QCoreApplication::arguments();
    const QString op = args.value(2);
    const QString key = args.value(3);
    auto usage = [] {
        std::fputs(
            "usage: AetherSDR --config <command>\n"
            "  list [prefix]      print key<TAB>value for every setting (optionally\n"
            "                     only keys starting with prefix)\n"
            "  get <key>          print the value (exit 2 if the key is absent)\n"
            "  set <key> <value>  create or change a setting\n"
            "  unset <key>        delete a setting\n"
            "  export             sanitized dump of the whole store (secrets\n"
            "                     redacted; diagnostic output, not a backup)\n"
            "  features [family]  list radio-scoped feature documents\n"
            "                     (sanitized; written by the app's own\n"
            "                     features — HL2 state, nicknames, band stacks)\n"
            "  path               print the settings database path\n",
            stderr);
        return 1;
    };

    if (op == QStringLiteral("path")) {
        std::printf("%s\n", qPrintable(AetherSDR::SettingsPaths::databasePath()));
        return 0;
    }

    auto& settings = AetherSDR::AppSettings::instance();
    settings.load();  // runs the one-time XML import exactly like the GUI would
    const QString notice = settings.loadNotice();
    if (!notice.isEmpty()) {
        std::fprintf(stderr, "notice: %s\n", qPrintable(notice));
    }

    if (op == QStringLiteral("list")) {
        const QString prefix = args.value(3);
        QStringList keys = settings.allKeysForDiagnostics();
        keys.sort();
        for (const QString& k : keys) {
            if (!prefix.isEmpty() && !k.startsWith(prefix)) {
                continue;
            }
            std::printf("%s\t%s\n", qPrintable(k),
                        qPrintable(settings.value(k).toString()));
        }
        return 0;
    }
    if (op == QStringLiteral("get")) {
        if (key.isEmpty()) {
            return usage();
        }
        if (!settings.contains(key)) {
            std::fprintf(stderr, "%s: no such key\n", qPrintable(key));
            return 2;
        }
        std::printf("%s\n", qPrintable(settings.value(key).toString()));
        return 0;
    }
    if (op == QStringLiteral("export")) {
        std::printf("%s", qPrintable(AetherSDR::SettingsSanitizer::dump()));
        return 0;
    }
    if (op == QStringLiteral("features")) {
        const QString family = args.value(3);
        const auto rows = settings.radioFeaturesForDiagnostics();
        for (const auto& row : rows) {
            if (!family.isEmpty()
                && !row.first.startsWith(family + QLatin1Char('/'))) {
                continue;
            }
            std::printf("%s\t%s\n", qPrintable(row.first),
                        qPrintable(AetherSDR::SettingsSanitizer::redactedValue(
                            row.first, row.second)));
        }
        return 0;
    }
    if (op == QStringLiteral("set") || op == QStringLiteral("unset")) {
        if (key.isEmpty() || (op == QStringLiteral("set") && args.size() < 5)) {
            return usage();
        }
        // The credential policy applies to the CLI too (PR #4612 review):
        // creating a credential row would undo the exodus this store ships
        // with. unset stays allowed — deleting a credential is always fine.
        if (op == QStringLiteral("set")
            && AetherSDR::SettingsSanitizer::isSecretKey(key)) {
            std::fprintf(stderr,
                         "refusing: '%s' is a credential-shaped key, and "
                         "credentials are never stored in the settings "
                         "database (RFC #4603). Use the app's own UI, which "
                         "stores it in the OS keychain.\n",
                         qPrintable(key));
            return 1;
        }
        if (op == QStringLiteral("set")) {
            settings.setValue(key, args.value(4));
        } else {
            settings.remove(key);
        }
        settings.save();
        // Evidence over assertion: confirm the row really persisted by
        // reading it back from the database file, not the cache.
        QString persisted;
        const bool present = AetherSDR::SettingsDatabase::readAppValueFromFile(
            AetherSDR::SettingsPaths::databasePath(), key, persisted);
        const bool ok = op == QStringLiteral("set")
                            ? (present && persisted == args.value(4))
                            : !present;
        if (!ok) {
            std::fprintf(stderr, "error: change did not persist (see warnings "
                                 "above — store may be read-only)\n");
            return 1;
        }
        std::printf("ok\n");
        return 0;
    }
    return usage();
}

// The Qt platform-selection decision made on a Wayland session, recorded before
// the log handler exists and re-emitted next to the GpuSelector summary once
// logging is up — so a Support bundle shows why we chose xcb vs wayland (the
// first "why am I on XWayland / why did fractional scaling change" question).
// nullptr choice = no override applied (not a Wayland session, or the user set
// QT_QPA_PLATFORM already). The presence itself lives in DisplayPresence.h's
// accessor (setDetectedDisplayPresence) so SpectrumWidget can read it too.
static const char* g_qpaPlatformChoice = nullptr;
// Whether the user's environment set QT_QPA_PLATFORM, read before main() may set
// it, so the start-up platform line can say who chose the value Qt started with.
static bool g_qpaPlatformUserSet = false;
// The -platform argument, which Qt prefers to QT_QPA_PLATFORM; read before
// QApplication removes it from argv.
static std::optional<QString> g_qpaPlatformArgument;

static const char* displayPresenceName(AetherSDR::DisplayPresence p)
{
    switch (p) {
        case AetherSDR::DisplayPresence::Connected: return "connected";
        case AetherSDR::DisplayPresence::Headless:  return "headless";
        case AetherSDR::DisplayPresence::Unknown:   return "unknown";
    }
    return "unknown";
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && qstrcmp(argv[1], "--config") == 0) {
        return runConfigCli(argc, argv);
    }

    // ── Pre-QApplication environment setup ────────────────────────────────

    // AETHER_NO_GPU: runtime toggle to force software OpenGL rendering.
    // Unlike the compile-time AETHER_GPU_SPECTRUM CMake flag, this works on
    // already-built binaries. Avoids hardware GLX/EGL entirely.
    if (qEnvironmentVariableIsSet("AETHER_NO_GPU")) {
        qputenv("QT_OPENGL", "software");
    }

    // NOTE ON ORDER: the QT_QPA_PLATFORM block runs BEFORE
    // GpuSelector::applyAtStartup() below. GpuSelector::willUseWayland() reads
    // the -platform argument (passed in) or else QT_QPA_PLATFORM to decide
    // whether to apply the X11/GLX NVIDIA vendor hint
    // (__GLX_VENDOR_LIBRARY_NAME), so our platform choice must be in the
    // environment before it runs — otherwise a headless box that we route to
    // XWayland/GLX would be mistaken for EGL/Wayland and lose PRIME offload.
    // applyAtStartup() is otherwise independent (pure sysfs reads + PRIME-var
    // qputenv, no Qt/GL dependency), so nothing here needs it to go first.

    // Prefer native Wayland in a Wayland session (#1233): under XWayland, GLX
    // context switching between windows crashes with BadAccess on some compositors.
    // "wayland;xcb" is an ordered fallback list, so a host without the Wayland
    // plugin lands on xcb instead of failing to start (#1389); appimage.yml deploys
    // the Wayland plugin. Skipped when the user sets QT_QPA_PLATFORM (xcb is the
    // documented escape hatch, README next to AETHER_NO_GPU), and when the user
    // passes -platform, which Qt prefers to QT_QPA_PLATFORM anyway.
    g_qpaPlatformUserSet = qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    g_qpaPlatformArgument = AetherSDR::QtPlatformChoice::platformArgument(argc, argv);
    if (!g_qpaPlatformUserSet && !g_qpaPlatformArgument) {
        const QByteArray session = qgetenv("XDG_SESSION_TYPE");
        if (session == "wayland" && qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            // Override to xcb only when the session is affirmatively headless (DRM
            // connectors present, none connected, e.g. a Pi 5 over wayvnc): native-Wayland
            // EGL cannot allocate a window surface without DRM scanout and the panadapter
            // renders black (QT_OPENGL=software doesn't help). Unknown -> keep Wayland.
            // Linux-only detection; the bool with an #else arm keeps the branch below safe.
#ifdef __linux__
            const AetherSDR::DisplayPresence presence =
                AetherSDR::detectDisplayPresence();
            const bool headless =
                presence == AetherSDR::DisplayPresence::Headless;
            AetherSDR::setDetectedDisplayPresence(presence);
#else
            constexpr bool headless = false;
#endif
            const char* platform = headless ? "xcb;wayland" : "wayland;xcb";
            qputenv("QT_QPA_PLATFORM", platform);
            g_qpaPlatformChoice = platform;  // re-emitted once logging is up
        }
    }

    // Render-adapter selection on multi-GPU systems.  Must run before the GL/D3D
    // context is created (i.e. before QApplication): sets PRIME offload (Linux)
    // or QT_D3D_ADAPTER_INDEX (Windows) from the persisted Display-menu choice.
    // Runs AFTER the QT_QPA_PLATFORM block above so GpuSelector::willUseWayland()
    // reads the platform we actually chose (see the ORDER note above).
    AetherSDR::GpuSelector::applyAtStartup(g_qpaPlatformArgument.value_or(QString()));

    // Qt 6.12's QtMultimedia defaults to its PipeWire audio backend and
    // segfaults enumerating devices when it cannot create a PipeWire context
    // (libpipewire installed, client configuration missing). Must run before
    // anything touches QMediaDevices; QApplication is the safe bound. Never
    // overrides a QT_AUDIO_BACKEND the user set.
    AetherSDR::QtAudioBackendGuard::applyAtStartup();

#ifdef __linux__
    // Install a tolerant X11 error handler before QApplication and before any
    // library (FFmpeg, VA-API, VDPAU) can open an X11 connection.  Xlib's
    // default handler calls exit() on protocol errors like BadAccess, which
    // makes stray X11 probing from non-free FFmpeg builds fatal.  On the
    // Wayland platform Qt does not install its own X11 handler (unlike xcb),
    // so without this the default handler remains active.  (#1839)
    //
    // dlopen avoids a build-time dependency on libX11-dev.
    {
        using XErrHandler = int (*)(AetherX11Display*, AetherX11ErrorEvent*);
        using XSetErrHandlerFn = XErrHandler (*)(XErrHandler);
        void* x11 = dlopen("libX11.so.6", RTLD_LAZY);
        if (x11) {
            auto fn = reinterpret_cast<XSetErrHandlerFn>(
                dlsym(x11, "XSetErrorHandler"));
            if (fn)
                fn(aetherTolerantX11ErrorHandler);
            // Do not dlclose — libX11 must stay loaded for the handler to
            // remain registered for connections opened later by FFmpeg.
        }
    }
#endif

    // Apply saved UI scale factor BEFORE QApplication is created.
    // QT_SCALE_FACTOR must be set before Qt initializes the display.
    // SettingsBootstrap reads the SQLite store read-only (or the frozen
    // pre-upgrade XML on the very first launch after the upgrade) without
    // constructing the AppSettings singleton (RFC #4603).
    {
        const int pct =
            AetherSDR::SettingsBootstrap::readValue(QStringLiteral("UiScalePercent"))
                .trimmed().toInt();
        // Always set — even at 100% — so a restarted child process overrides
        // any QT_SCALE_FACTOR it inherited from its parent.
        if (pct > 0)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(pct / 100.0, 'f', 2));
    }

#ifdef Q_OS_MAC
    // SpectrumWidget and WaveformWidget require native Metal child views, but
    // their surrounding raster-widget trees must not become native siblings.
    // Together with WA_DontCreateNativeAncestors on those leaves, this avoids
    // redundant window-sized Core Animation backing stores (#4339).
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    // A restricted launcher can deny QCocoa access to macOS GUI services.
    // AppKit then calls abort() from HIServices while QApplication is being
    // constructed. Scope the handler to this constructor only: runtime aborts
    // must keep their normal crash semantics.
    AetherSDR::MacStartupAbortGuard startupAbortGuard;
    if (!startupAbortGuard.isArmed()) {
        std::fputs("AetherSDR startup error: could not install the macOS GUI "
                   "initialization guard; refusing to start.\n",
                   stderr);
        return EXIT_FAILURE;
    }
#endif
    QApplication app(argc, argv);

    // Name the GUI thread. Qt names the threads it starts, propagating
    // QThread::objectName() in QThreadPrivate::start(), but the main thread was
    // never started that way — so it is the one thread with no name, in the
    // System Info table (#2554) and in Instruments, perf and ps alike. It is
    // also usually the busiest, which made "busiest: (unnamed)" the first thing
    // the Threads tab reported. On Linux only the Qt half applies: there the
    // main thread is the thread-group leader, whose kernel name is the process
    // name, so it was never unnamed and renaming it would rename the process
    // for ps and pgrep (ThreadName.cpp).
    AetherSDR::SystemInfo::setCurrentThreadName("AetherSDR-GUI");

#ifdef Q_OS_MAC
    if (!startupAbortGuard.disarm()) {
        std::fputs("AetherSDR startup error: could not restore the SIGABRT "
                   "handler after GUI initialization; refusing to continue.\n",
                   stderr);
        return EXIT_FAILURE;
    }
#endif

    // Release the Hermes-Lite 2 if this process is killed or crashes: a signal
    // skips Hl2Backend's destructor, and an HL2 never told to stop keeps streaming,
    // then ignores discovery until power-cycled. Installed after the macOS startup
    // guard restores SIGABRT so the guard can't overwrite it. No-op while
    // disconnected; see Hl2EmergencyStop.h for why it acts inside the handler.
    AetherSDR::hl2::installEmergencyStopSignalHandlers();

    app.setApplicationName("AetherSDR");
    app.setApplicationVersion(AETHERSDR_VERSION);
    app.setOrganizationName("AetherSDR");
    app.setDesktopFileName("AetherSDR");  // matches .desktop file for taskbar icon

    // ── Main-thread stall watchdog (diagnostic, log-only) ─────────────────
    // 250 ms heartbeat on the GUI event loop. If a tick arrives late by more
    // than the threshold, the main thread was blocked — e.g. the ~2 s
    // band-change+ATU stall that delays the TCI `vfo:` echo past WSJT-X's
    // 2000 ms timeout ("TCI failed set rxfreq" → disconnect → dead RX).
    // The warning fires immediately after the loop unblocks, so the stall's
    // start ≈ (log timestamp − reported gap), and whatever logs right after
    // it is what the loop was waiting to do. qWarning → visible at default
    // log levels.
    {
        static QElapsedTimer s_lastBeat;
        s_lastBeat.start();
        auto* heartbeat = new QTimer(&app);
        QObject::connect(heartbeat, &QTimer::timeout, &app, []() {
            const qint64 gapMs = s_lastBeat.restart();
            if (gapMs > 600) {
                qWarning().nospace()
                    << "MainThreadWatchdog: event loop stalled ~" << gapMs
                    << " ms (heartbeat is 250 ms; stall began ~" << gapMs
                    << " ms before this line)";
            }
        });
        heartbeat->start(250);
    }

    // ── Bundled DSEG fonts (SIL OFL 1.1) ──────────────────────────────────
    // Register the 13 TTFs into QFontDatabase so themes can resolve
    // "DSEG7 Modern" / "DSEG14 Modern" / "DSEGWeather" without depending
    // on the system having them installed.  Files live in resources.qrc
    // under /fonts/ and are baked into the binary at build time.
    {
        static constexpr const char* kDsegFonts[] = {
            ":/fonts/DSEG7Modern-Light.ttf",
            ":/fonts/DSEG7Modern-LightItalic.ttf",
            ":/fonts/DSEG7Modern-Regular.ttf",
            ":/fonts/DSEG7Modern-Italic.ttf",
            ":/fonts/DSEG7Modern-Bold.ttf",
            ":/fonts/DSEG7Modern-BoldItalic.ttf",
            ":/fonts/DSEG14Modern-Light.ttf",
            ":/fonts/DSEG14Modern-LightItalic.ttf",
            ":/fonts/DSEG14Modern-Regular.ttf",
            ":/fonts/DSEG14Modern-Italic.ttf",
            ":/fonts/DSEG14Modern-Bold.ttf",
            ":/fonts/DSEG14Modern-BoldItalic.ttf",
            ":/fonts/DSEGWeather.ttf",
        };
        for (const char* path : kDsegFonts) {
            if (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) < 0)
                qWarning() << "Failed to load bundled font:" << path;
        }
    }

    // Bundled Inter UI font (SIL OFL 1.1). The theme's `font.family.ui` resolves to
    // "Inter", and px-tuned QSS assumes it, so register it rather than fall through
    // to a per-platform family (#4036). Static Regular + Bold, not the variable
    // font: variable-axis support needs Qt 6.7+.
    {
        static constexpr const char* kInterFonts[] = {
            ":/fonts/Inter-Regular.ttf",
            ":/fonts/Inter-Bold.ttf",
        };
        for (const char* path : kInterFonts) {
            if (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) < 0) {
                qWarning() << "Failed to load bundled font:" << path;
            }
        }
    }

    // Request microphone permission early (macOS only).
    // Shows the system prompt on first launch so it's ready before PTT.
    requestMicrophonePermission();

    // One-shot migration from the double-nested AppConfigLocation
    // (~/.config/AetherSDR/AetherSDR/, because org and app name are both
    // "AetherSDR") to GenericConfigLocation + "/AetherSDR", where AppSettings lives.
    // Idempotent: skips existing targets and removes the old dir once empty.
    {
        const QString newDir = QStandardPaths::writableLocation(
                                   QStandardPaths::GenericConfigLocation)
                               + QStringLiteral("/AetherSDR");
        const QString oldDir = QStandardPaths::writableLocation(
                                   QStandardPaths::AppConfigLocation);
        if (newDir != oldDir && QDir(oldDir).exists()) {
            QDir().mkpath(newDir);
            const QDir od(oldDir);
            const auto entries = od.entryList(QDir::NoDotAndDotDot | QDir::AllEntries
                                              | QDir::Hidden | QDir::System);
            int moved = 0, skipped = 0;
            for (const QString& entry : entries) {
                const QString src = oldDir + QLatin1Char('/') + entry;
                const QString dst = newDir + QLatin1Char('/') + entry;
                if (QFileInfo::exists(dst)) {
                    qDebug() << "Config-dir migration: skipping" << src
                             << "— target already exists at" << dst;
                    ++skipped;
                    continue;
                }
                // QFile::rename handles both regular files and directories
                // on Linux (rename(2) is happy with both as long as src and
                // dst are on the same filesystem, which they are by
                // construction).  Same path family on Windows / macOS.
                if (QFile::rename(src, dst)) {
                    qDebug() << "Config-dir migration: moved" << src << "to" << dst;
                    ++moved;
                } else {
                    qWarning() << "Config-dir migration: failed to move"
                               << src << "to" << dst;
                }
            }
            // rmdir succeeds only if oldDir is empty after the moves —
            // anything we skipped or couldn't move stays put.
            if (QDir().rmdir(oldDir)) {
                qDebug() << "Config-dir migration: removed empty" << oldDir
                         << "(moved" << moved << "skipped" << skipped << ")";
            } else if (moved + skipped > 0) {
                qDebug() << "Config-dir migration:" << oldDir
                         << "retained (contains unrecognised entries; moved"
                         << moved << "skipped" << skipped << ")";
            }
        }
    }

    // Set up file logging in ~/.config/AetherSDR/logs/.  The dedicated
    // logs/ subdir keeps the rotated debug files out of the config-root
    // listing (which was getting crowded with 50+ aethersdr-*.log files
    // sitting alongside the actual settings).  GenericConfigLocation +
    // app name still avoids the AppConfigLocation double-nest.
    const QString configRoot = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                               + "/AetherSDR";
    const QString logDir = configRoot + "/logs";
    QDir().mkpath(logDir);
    // Source-feed logs (dxcluster.log, rbn.log, wsjtx.log, …) live in
    // their own subdir so they don't mingle with the application logs.
    QDir().mkpath(configRoot + "/spothub");

    // One-shot migration of the existing root-level log files into the
    // new subdirs.  Each rename() is best-effort; failures fall through
    // so the app keeps working with whatever can be moved.
    {
        QDir rootDir(configRoot);
        // App debug logs — every aethersdr-*.log, plus the aethersdr.log
        // symlink (we remove it; LogManager will recreate inside logs/).
        for (const QString& f : rootDir.entryList({"aethersdr-*.log"},
                                                  QDir::Files | QDir::Hidden)) {
            const QString src = configRoot + "/" + f;
            const QString dst = logDir     + "/" + f;
            if (!QFileInfo::exists(dst))
                QFile::rename(src, dst);
        }
        const QString oldSymlink = configRoot + "/aethersdr.log";
        if (QFileInfo(oldSymlink).isSymLink() || QFile::exists(oldSymlink))
            QFile::remove(oldSymlink);

        // SpotHub source feeds.
        for (const QString& f : QStringList{
                 "dxcluster.log",   "spotcollector.log", "wsjtx.log",
                 "freedv.log",      "pota.log",          "rbn.log",
                 "pskreporter.log"}) {
            const QString src = configRoot + "/" + f;
            const QString dst = configRoot + "/spothub/" + f;
            if (QFileInfo::exists(src) && !QFileInfo::exists(dst))
                QFile::rename(src, dst);
        }
    }

    // Load AppSettings before pruning/log-start so retention config and the
    // active-file size cap (AppSettings["LogRetention"], #2498) are available
    // to LogManager. SHistorySoftEdgeDb migration moved here for the same
    // reason.
    AetherSDR::AppSettings::instance().load();
    // Construct the NR2 feature-owned settings model on the main thread after
    // AppSettings is loaded. This also performs its one-time flat-key migration.
    static_cast<void>(AetherSDR::Nr2SettingsModel::instance());
    AetherSDR::AppSettings::instance().initializeGuiClientIdentity();
    {
        // Retired keys nothing reads any more. AboutDialogGeometry went when
        // About moved to CanonWindow, which opens centred (RFC #6226).
        auto& s = AetherSDR::AppSettings::instance();
        bool removed = false;
        for (const char* key : {"SHistorySoftEdgeDb", "AboutDialogGeometry"}) {
            if (s.contains(key)) {
                s.remove(key);
                removed = true;
            }
        }
        if (removed) {
            s.save();
        }
    }

    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString logPath = logDir + "/aethersdr-" + timestamp + ".log";

    // Bounded historical footprint: retention by age + total size cap,
    // both configurable in AppSettings["LogRetention"]. (#2498)
    AetherSDR::LogManager::instance().pruneOldLogs(logDir);

    // Skip stderr when it's a pipe to a non-draining parent (Stream Deck
    // "Run Command", systemd user services, GUI launchers).  Once the
    // ~64 KB pipe buffer fills, a blocking write could lock up the logger.
    static const bool stderrIsTty = isatty(fileno(stderr));

    auto& logManager = AetherSDR::LogManager::instance();
    if (logManager.startLogging(logPath, stderrIsTty)) {
        qInstallMessageHandler(messageHandler);

        // Record the GPU-selection decision now the log handler exists
        // (GpuSelector::applyAtStartup() ran before logging was available).
        qInfo().noquote() << "GpuSelector: render GPU ->"
                          << AetherSDR::GpuSelector::appliedSummary();
        qInfo().noquote() << "QtAudioBackendGuard:"
                          << AetherSDR::QtAudioBackendGuard::appliedSummary();

        // Likewise the Wayland platform choice (decided before logging existed).
        if (g_qpaPlatformChoice) {
            qInfo().noquote()
                << "Platform: Wayland session, display presence"
                << displayPresenceName(AetherSDR::detectedDisplayPresence())
                << "-> QT_QPA_PLATFORM" << g_qpaPlatformChoice;
        }
        {
            using AetherSDR::QpaRequestSource;
            const QpaRequestSource source = g_qpaPlatformArgument ? QpaRequestSource::CommandLine
                : g_qpaPlatformUserSet                            ? QpaRequestSource::User
                : g_qpaPlatformChoice                             ? QpaRequestSource::AetherSDR
                                                                  : QpaRequestSource::Unset;
            const QString requested = g_qpaPlatformArgument
                ? *g_qpaPlatformArgument
                : qEnvironmentVariable("QT_QPA_PLATFORM");
            const QStringList lines = AetherSDR::QtPlatformChoice::logLines(
                requested, source, QGuiApplication::platformName());
            qInfo().noquote() << lines.value(0);
            if (lines.size() > 1) {
                qWarning().noquote() << lines.at(1);
            }
        }

        // Symlink aethersdr.log → latest timestamped file (for Support dialog)
        const QString symlink = logDir + "/aethersdr.log";
#ifdef Q_OS_UNIX
        // Atomic replace: create temp symlink, then rename() over the old one.
        // POSIX guarantees rename() is atomic within a single filesystem,
        // closing the TOCTOU window that the previous remove()+link() pair
        // left open between the two calls. (Principle XIV)
        const QByteArray symlinkBytes = symlink.toLocal8Bit();
        const QByteArray tmpBytes     = (symlink + QStringLiteral(".new")).toLocal8Bit();
        const QByteArray targetBytes  = logPath.toLocal8Bit();
        ::unlink(tmpBytes.constData());  // clear stale temp from a prior crash
        if (::symlink(targetBytes.constData(), tmpBytes.constData()) != 0) {
            qWarning("failed to create temp log symlink: %s", strerror(errno));
        } else if (::rename(tmpBytes.constData(), symlinkBytes.constData()) != 0) {
            qWarning("failed to rename log symlink into place: %s", strerror(errno));
            ::unlink(tmpBytes.constData());
        }
#else
        // Windows: QFile::link creates a .lnk shortcut; non-atomic is
        // acceptable here since the symlink is only consumed by the
        // Support dialog locally.
        QFile::remove(symlink);
        QFile::link(logPath, symlink);
#endif
    } else {
        fprintf(stderr, "Warning: could not open log file %s\n", logPath.toLocal8Bit().constData());
    }

    // Use Fusion style as a clean cross-platform base
    // (our dark theme overrides colors via stylesheet)
    app.setStyle(QStyleFactory::create("Fusion"));

    // Load slice color overrides (must be after AppSettings::load)
    AetherSDR::SliceColorManager::instance().load();

    // Load per-module logging toggles (must be after AppSettings::load)
    AetherSDR::LogManager::instance().loadSettings();

    // Hardware/capability inventory (#4986): after loadSettings() so the
    // aether.sysinfo filter rules are live, and before the main window exists
    // so no ggml code can have run yet (Copy Assist is built lazily on first
    // panel open — on a CPU below the speech engine's ISA baseline, entering
    // ggml is the crash this block diagnoses). Flushed explicitly so the block
    // is on disk before anything else in startup runs, rather than on the
    // async writer's next periodic flush.
    AetherSDR::SystemInventory::logSystemInventory();
    AetherSDR::LogManager::instance().flushLog();

    qDebug() << "Starting AetherSDR" << app.applicationVersion();

    int exitCode = 0;
    {
        AetherSDR::MainWindow window;
        window.show();

        // Settings-store notices a user must see (RFC #4603): a backup was
        // restored after corruption, a newer-schema store opened read-only, or
        // older-version changes were not carried forward. One-shot, after the
        // window is up so the box has a parent and never blocks startup.
        {
            const QString settingsNotice =
                AetherSDR::AppSettings::instance().loadNotice();
            if (!settingsNotice.isEmpty()) {
                QTimer::singleShot(0, &window, [&window, settingsNotice] {
                    AetherSDR::FramelessMessageBox::warning(
                        &window, QStringLiteral("Settings"), settingsNotice);
                });
            }
        }

        // Agent automation bridge (#3646), built in MainWindow::startAutomationBridge().
        // Starts when the Radio Setup -> Network toggle is persisted or AETHER_AUTOMATION
        // is set (the headless/CI override). AETHER_AUTOMATION_SOCKET names the socket
        // verbatim; otherwise it is PID-suffixed so concurrent instances don't unlink
        // each other's socket. The window owns the server.
        const bool bridgePersisted = AetherSDR::AutomationBridgeSettings::enabled();
        if (qEnvironmentVariableIsSet("AETHER_AUTOMATION") || bridgePersisted)
            window.startAutomationBridge();

        exitCode = app.exec();
    }

    AetherSDR::ShutdownTrace::complete("application");

    qInstallMessageHandler(nullptr);
    logManager.shutdownLogging();

    return exitCode;
}
