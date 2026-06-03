#include "gui/MainWindow.h"
#include "gui/SliceColorManager.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/MacMicPermission.h"

#include <QApplication>
#include <QSurfaceFormat>
#include <QStyleFactory>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFontDatabase>
#include <QDateTime>
#include <QStandardPaths>

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
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement             = 1;
extern "C" __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
#endif // Q_OS_WIN

static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    AetherSDR::LogManager::instance().enqueueMessage(type, ctx, msg);
}

int main(int argc, char* argv[])
{
    // ── Pre-QApplication environment setup ────────────────────────────────

    // AETHER_NO_GPU: runtime toggle to force software OpenGL rendering.
    // Unlike the compile-time AETHER_GPU_SPECTRUM CMake flag, this works on
    // already-built binaries. Avoids hardware GLX/EGL entirely.
    if (qEnvironmentVariableIsSet("AETHER_NO_GPU")) {
        qputenv("QT_OPENGL", "software");
    }

    // Prefer native Wayland when running under a Wayland session (#1233).
    // Without this, Qt may fall back to XWayland (xcb platform) where GLX
    // context switching between the main window and child dialogs triggers
    // a BadAccess crash (X_GLXMakeCurrent) on some compositors.
    // Only set when QT_QPA_PLATFORM isn't already configured by the user.
    // Skip for AppImage: the bundled Qt Wayland plugin may not match the
    // host compositor's protocol version, causing an abort on init (#1389).
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")
            && !qEnvironmentVariableIsSet("APPIMAGE")) {
        const QByteArray session = qgetenv("XDG_SESSION_TYPE");
        if (session == "wayland" && qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "wayland");
        }
    }

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
    // We read the settings file directly (can't use AppSettings or
    // QStandardPaths before QApplication exists).
    {
#ifdef Q_OS_MAC
        QString settingsPath = QDir::homePath() + "/Library/Preferences/AetherSDR/AetherSDR.settings";
#elif defined(Q_OS_WIN)
        // AppSettings uses GenericConfigLocation (%LOCALAPPDATA%) + "/AetherSDR".
        // QStandardPaths isn't available before QApplication, so we reproduce
        // the path manually using the LOCALAPPDATA env var.
        QString settingsPath = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"))
                               + "/AetherSDR/AetherSDR.settings";
#else
        QString settingsPath = QDir::homePath() + "/.config/AetherSDR/AetherSDR.settings";
#endif
        QFile f(settingsPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QByteArray data = f.readAll();
            // AppSettings XML format: <UiScalePercent>125</UiScalePercent>
            QByteArray tag = "<UiScalePercent>";
            int idx = data.indexOf(tag);
            if (idx >= 0) {
                idx += tag.size();
                int end = data.indexOf('<', idx);
                if (end > idx) {
                    int pct = data.mid(idx, end - idx).trimmed().toInt();
                    // Always set — even at 100% — so a restarted child process
                    // overrides any QT_SCALE_FACTOR it inherited from its parent.
                    if (pct > 0)
                        qputenv("QT_SCALE_FACTOR", QByteArray::number(pct / 100.0, 'f', 2));
                }
            }
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName("AetherSDR");
    app.setApplicationVersion(AETHERSDR_VERSION);
    app.setOrganizationName("AetherSDR");
    app.setDesktopFileName("AetherSDR");  // matches .desktop file for taskbar icon

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

    // Request microphone permission early (macOS only).
    // Shows the system prompt on first launch so it's ready before PTT.
    requestMicrophonePermission();

    // One-shot config-dir migration: the older releases wrote some user data
    // (FFTW wisdom, ChannelStrip presets, firmware files, user themes, the
    // ONNX signal-classifier model) under QStandardPaths::AppConfigLocation,
    // which resolves to a double-nested ~/.config/AetherSDR/AetherSDR/ (or
    // %LOCALAPPDATA%/AetherSDR/AetherSDR/ on Windows, or the equivalent on
    // macOS) because both organizationName and applicationName are
    // "AetherSDR".  The new release uses GenericConfigLocation + "/AetherSDR"
    // everywhere — a single ~/.config/AetherSDR/ matching the AppSettings
    // file's location.  This block migrates any data the operator already
    // has at the old path up to the new path.
    //
    // Idempotent: skips items whose target already exists, then removes the
    // old dir if it ends up empty.  Subsequent launches early-return because
    // the old dir no longer exists.
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
    {
        auto& s = AetherSDR::AppSettings::instance();
        if (s.contains("SHistorySoftEdgeDb")) {
            s.remove("SHistorySoftEdgeDb");
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

    qDebug() << "Starting AetherSDR" << app.applicationVersion();

    int exitCode = 0;
    {
        AetherSDR::MainWindow window;
        window.show();
        exitCode = app.exec();
    }

    qInstallMessageHandler(nullptr);
    logManager.shutdownLogging();

    return exitCode;
}
