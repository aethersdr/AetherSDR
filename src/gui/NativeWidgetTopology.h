#pragma once

#include <QVariantMap>
#include <QWidget>

namespace AetherSDR {

// Append the macOS-facing native-widget topology fields used by `get rhi`.
// Kept as a small QWidget-only helper so the bridge contract can be exercised
// without constructing the full SpectrumWidget/radio stack.
inline void appendNativeWidgetTopology(QVariantMap& snapshot, const QWidget& widget)
{
    snapshot[QStringLiteral("nativeWindow")] = widget.windowHandle() != nullptr;
    snapshot[QStringLiteral("nativeAncestorsBlocked")] =
        widget.testAttribute(Qt::WA_DontCreateNativeAncestors);

    int nativeAncestorCount = 0;
    for (const QWidget* ancestor = widget.parentWidget(); ancestor;
         ancestor = ancestor->parentWidget()) {
        if (ancestor->testAttribute(Qt::WA_NativeWindow)) {
            ++nativeAncestorCount;
        }
    }
    snapshot[QStringLiteral("nativeAncestorCount")] = nativeAncestorCount;
}

}  // namespace AetherSDR
