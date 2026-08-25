#include "WindowChrome.h"

#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

namespace {

constexpr CGFloat kTitleBarHeight = 52.0;
char kTitleBarMaterialKey;

NSWindow* nativeWindowFor(QWidget* widget)
{
    if (!widget) {
        return nil;
    }
    QWindow* handle = widget->windowHandle();
    if (!handle) {
        return nil;
    }
    NSView* view = reinterpret_cast<NSView*>(handle->winId());
    if (![view isKindOfClass:[NSView class]]) {
        return nil;
    }
    return view.window;
}

} // namespace

namespace AetherSDR::mac {

bool isSupported()
{
    return true;
}

void applyFramelessWindowStyle(QWidget* window, int cornerRadius)
{
    NSWindow* native = nativeWindowFor(window);
    if (!native) {
        return;
    }

    // Qt::FramelessWindowHint gives us NSWindowStyleMaskBorderless, which also
    // drops NSWindowStyleMaskResizable — and with it AppKit's own edge and
    // corner resize handles.  Putting the bit back is what keeps dragging an
    // edge working, and it costs nothing else: a borderless window with the
    // resizable bit still has no title bar and no system buttons.
    //
    // This is also why the app does not need QWindow::startSystemResize() on
    // macOS: AppKit owns the resize, so tiling and snapping stay native.
    native.styleMask |= NSWindowStyleMaskResizable;

    // Rounded corners come from the content view's layer.  A borderless window
    // is square by default, which is the single thing that most makes a custom
    // chrome look wrong next to other Mac windows.
    NSView* content = native.contentView;
    if (content) {
        content.wantsLayer = YES;
        content.layer.cornerRadius = cornerRadius;
        content.layer.masksToBounds = YES;
        if (@available(macOS 10.15, *)) {
            // Continuous ("squircle") curvature, matching the system's own
            // window corners rather than a plain circular arc.
            content.layer.cornerCurve = kCACornerCurveContinuous;
        }

        // A subview of `content` always composites above the QNSView's own
        // backing layer. Putting the effect there covers every Qt-painted
        // title-bar control even when the subview is ordered below the other
        // subviews. Install it as a sibling immediately beneath the Qt content
        // surface instead, so the translucent Qt tint reveals the material
        // without sacrificing the brand, tabs, or caption controls.
        NSView* frameView = content.superview;
        NSView* existingMaterial = objc_getAssociatedObject(
            content, &kTitleBarMaterialKey);
        NSVisualEffectView* material =
            [existingMaterial isKindOfClass:[NSVisualEffectView class]]
                ? (NSVisualEffectView*)existingMaterial
                : nil;
        if (!material && frameView) {
            material = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
            material.translatesAutoresizingMaskIntoConstraints = NO;
            material.material = NSVisualEffectMaterialHUDWindow;
            material.blendingMode = NSVisualEffectBlendingModeBehindWindow;
            material.state = NSVisualEffectStateFollowsWindowActiveState;
            [frameView addSubview:material
                       positioned:NSWindowBelow
                       relativeTo:content];
            [NSLayoutConstraint activateConstraints:@[
                [material.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
                [material.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
                [material.topAnchor constraintEqualToAnchor:content.topAnchor],
                [material.heightAnchor constraintEqualToConstant:kTitleBarHeight],
            ]];
            objc_setAssociatedObject(content, &kTitleBarMaterialKey, material,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
#if !__has_feature(objc_arc)
            [material release];
#endif
        }
    }

    // The rounded corners only read as rounded if the window itself is not
    // painting an opaque square underneath them.
    native.backgroundColor = [NSColor clearColor];
    native.opaque = NO;
    native.hasShadow = YES;
}

} // namespace AetherSDR::mac
