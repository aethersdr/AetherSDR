#pragma once

#include <QString>
#include <QVector>

namespace AetherSDR {

// Cross-platform GPU enumeration + persisted render-adapter selection.
//
// The QRhi render adapter cannot be hot-swapped under a live GL/D3D context, so
// the choice is applied ONCE at startup — applyAtStartup() runs in main() before
// QApplication and sets the platform's adapter-selection environment:
//   • Linux : PRIME offload (__NV_PRIME_RENDER_OFFLOAD / __GLX_VENDOR_LIBRARY_NAME)
//   • Windows: QT_D3D_ADAPTER_INDEX (honoured by Qt's D3D11/D3D12 RHI backend)
// The UI persists the choice (AppSettings "Graphics" key, one nested JSON blob —
// Principle V) and takes effect on the next launch.
struct GpuInfo {
    QString id;            // stable, platform-specific id (persisted); "auto" = default
    QString name;          // human-readable label for the menu
    bool    discrete{false};
    // False when the adapter is present but unsafe to select: the Windows
    // integrated adapter re-triggers the #1921 QRhiWidget-reparenting crash that
    // the NvOptimusEnablement export exists to avoid.  The menu disables such
    // entries and applyAtStartup() refuses them (keeping the system default).
    bool    selectable{true};
    // True when this adapter's selection path is not yet hardware-soaked (only
    // the Linux NVIDIA-offload path is validated on real hardware).  The menu
    // marks these "(experimental)".
    bool    experimental{false};
};

class GpuSelector {
public:
    static constexpr const char* kAutoId = "auto";

    // GPUs the system exposes, "Auto (system default)" always first.  Best
    // effort; a result with a single real GPU means there is nothing to choose.
    static QVector<GpuInfo> available();

    // True when the system exposes more than one selectable GPU (so the UI
    // should show the selector).
    static bool hasMultiple();

    // Persisted choice id (AppSettings "Graphics" → "gpu"); kAutoId when unset.
    static QString savedChoiceId();
    static void    saveChoiceId(const QString& id);

    // Called from main() BEFORE QApplication.  Reads the saved choice and sets
    // the adapter-selection env for the active backend.  No-op for Auto, when
    // the saved GPU is no longer present, or when the user already set the
    // relevant environment variable explicitly.
    static void applyAtStartup();

    // One-line description of what applyAtStartup() did, for logging once the
    // log handler is up (applyAtStartup itself runs before logging exists).
    static QString appliedSummary();
};

} // namespace AetherSDR
