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

    // Request microphone permission early (macOS only).
    // Shows the system prompt on first launch so it's ready before PTT.
    requestMicrophonePermission();

    // Set up file logging in ~/.config/AetherSDR/ (works inside AppImage where
    // applicationDirPath() is read-only).
    // Use GenericConfigLocation + app name to avoid the double-nested
    // ~/.config/AetherSDR/AetherSDR/ path that AppConfigLocation produces.
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + "/AetherSDR";
    QDir().mkpath(logDir);

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
