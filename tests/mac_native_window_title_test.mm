// Native toolbar ownership and window-state policy. Offscreen cannot reach AppKit.
#include "gui/mac/NativeWindowTitle.h"

#include <QApplication>
#include <QGuiApplication>
#include <QWidget>
#import <AppKit/AppKit.h>

#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

NSWindow* nativeWindow(const QWidget& window)
{
    return reinterpret_cast<NSView*>(window.internalWinId()).window;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        std::puts("SKIP: native toolbar checks require QT_QPA_PLATFORM=cocoa");
        return 77;
    }

    @autoreleasepool {
        QWidget window;
        window.setWindowFlags(Qt::Window | Qt::ExpandedClientAreaHint
                              | Qt::NoTitleBarBackgroundHint);
        window.winId();
        NSWindow* native = nativeWindow(window);
        check(native != nil, "hidden widget has a native window");
        if (!native) {
            return 1;
        }
        native.toolbarStyle = NSWindowToolbarStyleExpanded;

        AetherSDR::mac::updateNativeTitleVisibility(&window);
        NSToolbar* toolbar = native.toolbar;
        check([toolbar.identifier isEqualToString:@"AetherSDR.UnifiedTitleBar"],
              "expanded window installs its own alignment toolbar");
        check(native.toolbarStyle == NSWindowToolbarStyleUnified,
              "windowed toolbar uses unified style");
        check(toolbar.visible, "windowed alignment toolbar is visible");
        AetherSDR::mac::updateNativeTitleVisibility(&window);
        check(native.toolbar == toolbar, "repeated layout keeps the same toolbar");

        // A hidden window records the requested Qt state without a Space
        // transition. The bridge exercises real native fullscreen separately.
        for (int cycle = 0; cycle < 2; ++cycle) {
            window.setWindowState(Qt::WindowFullScreen);
            AetherSDR::mac::updateNativeTitleVisibility(&window);
            check(native.toolbar == toolbar, "fullscreen keeps the same toolbar for AppKit transitions");
            check(!native.toolbar.visible, "fullscreen hides the alignment toolbar");
            AetherSDR::mac::updateNativeTitleVisibility(&window);
            check(!native.toolbar.visible, "fullscreen layout keeps the toolbar hidden");

            window.setWindowState(Qt::WindowNoState);
            AetherSDR::mac::updateNativeTitleVisibility(&window);
            check([native.toolbar.identifier isEqualToString:@"AetherSDR.UnifiedTitleBar"],
                  "leaving fullscreen restores the alignment toolbar");
            check(native.toolbar == toolbar && native.toolbar.visible,
                  "leaving fullscreen shows the same toolbar");
            check(native.toolbarStyle == NSWindowToolbarStyleUnified,
                  "leaving fullscreen restores unified style");
        }

        // Change the policy input without recreating the native window, so
        // the helper must restore the style on the window it configured.
        const Qt::WindowFlags expandedFlags = window.windowFlags();
        window.overrideWindowFlags(expandedFlags & ~Qt::ExpandedClientAreaHint);
        AetherSDR::mac::updateNativeTitleVisibility(&window);
        native = nativeWindow(window);
        check(native.toolbar == nil, "disabling expanded chrome removes the toolbar");
        check(native.toolbarStyle == NSWindowToolbarStyleExpanded,
              "disabling expanded chrome restores the original style");

        NSToolbar* foreign = [[NSToolbar alloc] initWithIdentifier:@"ForeignToolbar"];
        for (bool expanded : {true, false}) {
            window.overrideWindowFlags(expanded ? expandedFlags
                                               : expandedFlags & ~Qt::ExpandedClientAreaHint);
            native = nativeWindow(window);
            native.toolbar = foreign;
            native.toolbarStyle = NSWindowToolbarStyleExpanded;
            foreign.visible = YES;
            for (Qt::WindowState state : {Qt::WindowNoState, Qt::WindowFullScreen}) {
                window.setWindowState(state);
                AetherSDR::mac::updateNativeTitleVisibility(&window);
                check(native.toolbar == foreign, "foreign toolbar is left in place");
                check(native.toolbarStyle == NSWindowToolbarStyleExpanded,
                      "foreign toolbar style is left alone");
                check(native.toolbar.visible, "foreign toolbar visibility is left alone");
            }
            native.toolbar = nil;
        }
        [foreign release];
    }

    std::printf("Native toolbar checks: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
