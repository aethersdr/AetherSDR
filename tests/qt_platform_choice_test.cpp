// The start-up "Platform: Qt platform plugin" lines (#6285): which plugin is
// running, who asked for it, and whether Qt fell back from the first choice.
// Pure: QtPlatformChoice takes the values main.cpp reads (QT_QPA_PLATFORM and
// the -platform argument Qt prefers to it), no QGuiApplication.

#include "QtPlatformChoice.h"
#include "core/GpuSelector.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main()
{
    report("first of a list", QtPlatformChoice::firstRequested("wayland;xcb") == "wayland");
    report("first drops plugin arguments", QtPlatformChoice::firstRequested("xcb:foo=1") == "xcb");
    report("first of nothing", QtPlatformChoice::firstRequested("").isEmpty());

    report("wayland-egl is not a fallback from wayland",
           !QtPlatformChoice::fellBack("wayland;xcb", "wayland-egl"));
    report("xcb is a fallback from wayland;xcb", QtPlatformChoice::fellBack("wayland;xcb", "xcb"));
    report("wayland is a fallback from xcb;wayland", QtPlatformChoice::fellBack("xcb;wayland", "wayland"));
    report("unset is never a fallback", !QtPlatformChoice::fellBack("", "xcb"));
    report("wayland is not a fallback from wayland-egl",
           !QtPlatformChoice::fellBack("wayland-egl", "wayland"));

    {
        const QStringList l = QtPlatformChoice::logLines("wayland;xcb", QpaRequestSource::AetherSDR, "wayland");
        report("chosen by AetherSDR, loaded: one line", l.size() == 1);
        report("chosen by AetherSDR, loaded: text",
               l.value(0) == "Platform: Qt platform plugin \"wayland\" (QT_QPA_PLATFORM=wayland;xcb, set by AetherSDR)");
    }
    {
        const QStringList l = QtPlatformChoice::logLines("wayland;xcb", QpaRequestSource::AetherSDR, "xcb");
        report("fallback: two lines", l.size() == 2);
        report("fallback: text",
               l.value(1) == "Platform: Qt fell back from \"wayland\" to \"xcb\"; the first plugin in QT_QPA_PLATFORM did not load");
    }
    {
        const QStringList l = QtPlatformChoice::logLines("offscreen", QpaRequestSource::User, "offscreen");
        report("user setting: text",
               l.size() == 1
               && l.value(0) == "Platform: Qt platform plugin \"offscreen\" (QT_QPA_PLATFORM=offscreen, set by the user)");
    }
    {
        const QStringList l = QtPlatformChoice::logLines("", QpaRequestSource::Unset, "cocoa");
        report("unset: text",
               l.size() == 1
               && l.value(0) == "Platform: Qt platform plugin \"cocoa\" (QT_QPA_PLATFORM unset, Qt default)");
    }

    {
        // A CR or LF in the request cannot forge a second log line.
        const QStringList l = QtPlatformChoice::logLines(
            "xcb\n[09:14:02.101] WRN default: forged", QpaRequestSource::User, "xcb");
        report("newline in the request: one line, no line break",
               l.size() == 1 && !l.value(0).contains(QLatin1Char('\n'))
                   && !l.value(0).contains(QLatin1Char('\r')));
    }
    {
        // Qt prefers -platform to QT_QPA_PLATFORM: an explicit override is not a fallback.
        const QStringList l = QtPlatformChoice::logLines("minimal", QpaRequestSource::CommandLine, "minimal");
        report("command line override: one line", l.size() == 1);
        report("command line override: text",
               l.value(0) == "Platform: Qt platform plugin \"minimal\" (-platform minimal, on the command line)");
    }
    {
        const QStringList l = QtPlatformChoice::logLines("nosuchplugin;offscreen", QpaRequestSource::CommandLine,
                                                         "offscreen");
        report("command line fallback: two lines", l.size() == 2);
        report("command line fallback: text",
               l.value(1) == "Platform: Qt fell back from \"nosuchplugin\" to \"offscreen\"; the first plugin in -platform did not load");
    }
    {
        const char* none[] = {"AetherSDR", "--profile", "x"};
        report("no -platform argument", !QtPlatformChoice::platformArgument(3, none).has_value());
        const char* one[] = {"AetherSDR", "-platform", "minimal"};
        report("-platform value", QtPlatformChoice::platformArgument(3, one).value_or("") == "minimal");
        const char* dashes[] = {"AetherSDR", "--platform", "xcb"};
        report("--platform value", QtPlatformChoice::platformArgument(3, dashes).value_or("") == "xcb");
        const char* twice[] = {"AetherSDR", "-platform", "xcb", "-platform", "wayland;xcb"};
        report("the last -platform wins",
               QtPlatformChoice::platformArgument(5, twice).value_or("") == "wayland;xcb");
        const char* dangling[] = {"AetherSDR", "-platform"};
        report("-platform with no value is ignored", !QtPlatformChoice::platformArgument(2, dangling).has_value());
        const char* other[] = {"AetherSDR", "-platformtheme", "gtk3"};
        report("-platformtheme is not -platform", !QtPlatformChoice::platformArgument(3, other).has_value());
    }

    {
        // GpuSelector's GLX/EGL choice reads the same request Qt will use: the
        // -platform argument when there is one, so `-platform xcb` in a Wayland
        // session is X11 (GLX).
        const auto request = [](const char* const* argv, int argc) {
            return QtPlatformChoice::platformArgument(argc, argv).value_or(QString()).toLocal8Bit();
        };
        const char* xcb[] = {"AetherSDR", "-platform", "xcb"};
        report("-platform xcb in a Wayland session is X11",
               !GpuSelector::requestPicksWayland(request(xcb, 3), true));
        const char* wl[] = {"AetherSDR", "--platform", "wayland"};
        report("--platform wayland in an X11 session is Wayland",
               GpuSelector::requestPicksWayland(request(wl, 3), false));
        report("xcb;wayland runs on xcb", !GpuSelector::requestPicksWayland("xcb;wayland", true));
        report("wayland-egl is Wayland", GpuSelector::requestPicksWayland("wayland-egl", false));
        report("offscreen falls back to the session",
               GpuSelector::requestPicksWayland("offscreen", true)
                   && !GpuSelector::requestPicksWayland("offscreen", false));
        report("no request: the session decides",
               GpuSelector::requestPicksWayland("", true) && !GpuSelector::requestPicksWayland("", false));
    }

    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
