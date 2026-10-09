// The start-up "Platform: Qt platform plugin" lines (#6285): which plugin is
// running, who asked for it, and whether Qt fell back from the first choice.
// Pure: QtPlatformChoice takes the values main.cpp reads, no QGuiApplication.

#include "QtPlatformChoice.h"

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
               && l.value(0) == "Platform: Qt platform plugin \"cocoa\" (QT_QPA_PLATFORM=, unset, Qt default)");
    }

    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
