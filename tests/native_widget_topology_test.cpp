#include "gui/NativeWidgetTopology.h"

#include <QApplication>
#include <QVariantMap>
#include <QWidget>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

QVariantMap snapshotFor(const QWidget& widget)
{
    QVariantMap snapshot;
    snapshot[QStringLiteral("panIndex")] = 7;
    AetherSDR::appendNativeWidgetTopology(snapshot, widget);
    return snapshot;
}

void testDefaultTopology()
{
    QWidget root;
    QWidget leaf(&root);

    const QVariantMap snapshot = snapshotFor(leaf);
    check(!snapshot.value(QStringLiteral("nativeWindow")).toBool(),
          "non-native leaf reports nativeWindow=false");
    check(!snapshot.value(QStringLiteral("nativeAncestorsBlocked")).toBool(),
          "default leaf reports nativeAncestorsBlocked=false");
    check(snapshot.value(QStringLiteral("nativeAncestorCount")).toInt() == 0,
          "default hierarchy reports no marked native ancestors");
    check(snapshot.value(QStringLiteral("panIndex")).toInt() == 7,
          "topology fields preserve existing RHI snapshot entries");
}

void testIsolatedNativeLeaf()
{
    QWidget root;
    QWidget container(&root);
    QWidget leaf(&container);
    leaf.setAttribute(Qt::WA_DontCreateNativeAncestors);
    leaf.setAttribute(Qt::WA_NativeWindow);
    static_cast<void>(leaf.winId());

    QVariantMap snapshot = snapshotFor(leaf);
    check(snapshot.value(QStringLiteral("nativeWindow")).toBool(),
          "native leaf reports nativeWindow=true");
    check(snapshot.value(QStringLiteral("nativeAncestorsBlocked")).toBool(),
          "isolated leaf reports nativeAncestorsBlocked=true");
    check(snapshot.value(QStringLiteral("nativeAncestorCount")).toInt() == 0,
          "isolated leaf does not promote QWidget ancestors");

    // PanadapterStack reasserts WA_NativeWindow after a top-level reparent.
    // The isolation attribute is independent and must survive that cycle.
    leaf.setAttribute(Qt::WA_NativeWindow, false);
    leaf.setAttribute(Qt::WA_NativeWindow);
    static_cast<void>(leaf.winId());
    snapshot = snapshotFor(leaf);
    check(snapshot.value(QStringLiteral("nativeAncestorsBlocked")).toBool(),
          "native-window reassertion preserves ancestor isolation");
    check(snapshot.value(QStringLiteral("nativeAncestorCount")).toInt() == 0,
          "native-window reassertion keeps ancestors non-native");
}

void testExplicitNativeAncestors()
{
    QWidget root;
    QWidget container(&root);
    QWidget leaf(&container);
    root.setAttribute(Qt::WA_NativeWindow);
    container.setAttribute(Qt::WA_NativeWindow);

    const QVariantMap snapshot = snapshotFor(leaf);
    check(snapshot.value(QStringLiteral("nativeAncestorCount")).toInt() == 2,
          "snapshot counts every ancestor marked WA_NativeWindow");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);

    testDefaultTopology();
    testIsolatedNativeLeaf();
    testExplicitNativeAncestors();

    if (failures == 0) {
        std::printf("\nAll native widget topology tests passed.\n");
        return 0;
    }
    std::printf("\n%d native widget topology test(s) FAILED.\n", failures);
    return 1;
}
