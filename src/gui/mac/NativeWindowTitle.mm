#include "NativeWindowTitle.h"

#include <QWidget>
#include <QGuiApplication>
#import <AppKit/AppKit.h>

@interface AetherSDRTitleToolbar : NSToolbar
@property NSWindowToolbarStyle previousStyle;
@end

@implementation AetherSDRTitleToolbar
@end

namespace AetherSDR::mac {

static NSView* nativeView(const QWidget* window)
{
    if (!window || QGuiApplication::platformName() != QStringLiteral("cocoa")
        || !window->internalWinId()) {
        return nil;
    }
    return reinterpret_cast<NSView*>(window->internalWinId());
}

void updateNativeTitleVisibility(QWidget* window)
{
    NSWindow* native = nativeView(window).window;
    if (!native) {
        return;
    }
    const bool expanded = window->windowFlags().testFlag(Qt::ExpandedClientAreaHint);
    native.titleVisibility = expanded ? NSWindowTitleHidden : NSWindowTitleVisible;
    if (expanded && !native.toolbar) {
        auto* toolbar = [[AetherSDRTitleToolbar alloc] initWithIdentifier:@"AetherSDR.UnifiedTitleBar"];
        toolbar.previousStyle = native.toolbarStyle;
        if (@available(macOS 15, *)) {
            // AppKit no longer supports the baseline separator setting.
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            toolbar.showsBaselineSeparator = NO;
#pragma clang diagnostic pop
        }
        toolbar.allowsUserCustomization = NO;
        native.toolbarStyle = NSWindowToolbarStyleUnified;
        native.toolbar = toolbar;
        [toolbar release];
    } else if (!expanded && [native.toolbar isKindOfClass:[AetherSDRTitleToolbar class]]) {
        const NSWindowToolbarStyle previousStyle = ((AetherSDRTitleToolbar*)native.toolbar).previousStyle;
        native.toolbar = nil;
        native.toolbarStyle = previousStyle;
    }
    if ([native.toolbar isKindOfClass:[AetherSDRTitleToolbar class]]) {
        // AppKit balances fullscreen transition suppression on this object;
        // keep it installed, but hide its empty fullscreen strip.
        native.toolbar.visible = !window->isFullScreen();
    }
}

QRectF nativeCaptionBounds(const QWidget* window)
{
    NSView* view = nativeView(window);
    NSWindow* native = view.window;
    if (!native || window->isFullScreen()
        || !window->windowFlags().testFlag(Qt::ExpandedClientAreaHint)) {
        return {};
    }
    QRectF bounds;
    for (NSWindowButton role : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton* button = [native standardWindowButton:role];
        if (!button || button.hidden || !button.superview) {
            continue;
        }
        const NSRect rect = [button convertRect:button.bounds toView:view];
        const qreal top = view.flipped ? NSMinY(rect) : NSHeight(view.bounds) - NSMaxY(rect);
        bounds = bounds.united(QRectF(NSMinX(rect), top, NSWidth(rect), NSHeight(rect)));
    }
    return bounds;
}

}
