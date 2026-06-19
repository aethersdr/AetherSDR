#include "GpuSelector.h"

#include "AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

#if defined(Q_OS_LINUX)
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#elif defined(Q_OS_WIN)
#include <QtGlobal>
#include <dxgi.h>
#endif

namespace AetherSDR {

namespace {

#if defined(Q_OS_LINUX)
// Enumerate GPUs from DRM sysfs.  Each top-level cardN exposes a PCI device with
// a vendor id and a boot_vga flag (the primary, usually-integrated GPU).
QVector<GpuInfo> enumeratePlatform()
{
    QVector<GpuInfo> out;
    QDir drm(QStringLiteral("/sys/class/drm"));
    if (!drm.exists()) {
        return out;
    }
    QSet<QString> seenPci;
    const auto entries = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& card : entries) {
        // Top-level GPUs are "card0", "card1"…; connector subdirs are
        // "card0-DP-1" etc. — skip those.
        if (!card.startsWith(QLatin1String("card")) || card.contains('-')) {
            continue;
        }
        const QString devDir = QStringLiteral("/sys/class/drm/") + card + QStringLiteral("/device");
        const QString pci = QFileInfo(devDir).canonicalFilePath().section('/', -1);
        if (pci.isEmpty() || seenPci.contains(pci)) {
            continue;
        }
        seenPci.insert(pci);

        auto readTrim = [](const QString& path) -> QString {
            QFile f(path);
            return f.open(QIODevice::ReadOnly)
                       ? QString::fromLatin1(f.readAll()).trimmed()
                       : QString();
        };
        const QString vendor = readTrim(devDir + QStringLiteral("/vendor"));
        const bool bootVga   = readTrim(devDir + QStringLiteral("/boot_vga")) == QLatin1String("1");

        QString vendorName;
        bool discrete = false;
        if (vendor == QLatin1String("0x10de")) {            // NVIDIA
            vendorName = QStringLiteral("NVIDIA");
            discrete = true;
        } else if (vendor == QLatin1String("0x1002")) {     // AMD (dGPU or APU)
            vendorName = QStringLiteral("AMD");
            discrete = !bootVga;
        } else if (vendor == QLatin1String("0x8086")) {     // Intel (usually iGPU)
            vendorName = QStringLiteral("Intel");
            discrete = false;
        } else {
            vendorName = vendor.isEmpty() ? QStringLiteral("GPU") : (QStringLiteral("GPU ") + vendor);
            discrete = !bootVga;
        }

        GpuInfo g;
        g.id = QStringLiteral("pci:") + pci;
        g.name = vendorName + (discrete ? QStringLiteral(" (discrete)") : QStringLiteral(" (integrated)"));
        g.discrete = discrete;
        out.push_back(g);
    }
    return out;
}

#elif defined(Q_OS_WIN)
// Enumerate GPUs via DXGI; the adapter index lines up with QT_D3D_ADAPTER_INDEX.
QVector<GpuInfo> enumeratePlatform()
{
    QVector<GpuInfo> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))
            || !factory) {
        return out;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))
                && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {   // skip the WARP/software adapter
            GpuInfo g;
            g.id = QStringLiteral("dxgi:%1").arg(i);
            g.name = QString::fromWCharArray(desc.Description);
            g.discrete = desc.DedicatedVideoMemory > (static_cast<quint64>(256) << 20);
            out.push_back(g);
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return out;
}

#else
QVector<GpuInfo> enumeratePlatform() { return {}; }
#endif

QString s_appliedSummary = QStringLiteral("not run");

#if defined(Q_OS_LINUX)
// Will the app run on a Wayland (EGL) platform rather than X11 (GLX)?  Mirrors
// main.cpp's Wayland-preference logic; applyAtStartup() runs before QApplication
// so we read the environment directly.  GLX vendor selection only applies under
// X11 — under Wayland it is useless and __GLX_VENDOR_LIBRARY_NAME=nvidia can even
// raise a GLX BadValue.
bool willUseWayland()
{
    const QByteArray plat = qgetenv("QT_QPA_PLATFORM");
    if (!plat.isEmpty()) {
        if (plat.contains("wayland")) return true;
        if (plat.contains("xcb"))     return false;
    }
    return qEnvironmentVariableIsSet("WAYLAND_DISPLAY")
        || qgetenv("XDG_SESSION_TYPE") == QByteArrayLiteral("wayland");
}
#endif

QString readSavedId()
{
    const QJsonObject o = QJsonDocument::fromJson(
        AppSettings::instance().value(QStringLiteral("Graphics")).toString().toUtf8()).object();
    return o.value(QStringLiteral("gpu")).toString(QString::fromLatin1(GpuSelector::kAutoId));
}

} // namespace

QVector<GpuInfo> GpuSelector::available()
{
    QVector<GpuInfo> out;
    out.push_back({ QString::fromLatin1(kAutoId), QStringLiteral("Auto (system default)"), false });
    out += enumeratePlatform();
    return out;
}

bool GpuSelector::hasMultiple()
{
    // available() includes the synthetic "Auto" entry, so >1 real GPU == size>2.
    return available().size() > 2;
}

QString GpuSelector::savedChoiceId()
{
    return readSavedId();
}

void GpuSelector::saveChoiceId(const QString& id)
{
    QJsonObject o;
    o[QStringLiteral("gpu")] = id;
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("Graphics"),
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

void GpuSelector::applyAtStartup()
{
    // This runs in main() BEFORE QApplication and before main()'s own
    // AppSettings::load(), so the settings map is still empty.  Load it here so
    // the persisted choice is actually read (load() only touches QFile/XML and
    // is re-run harmlessly during normal startup).
    AppSettings::instance().load();

    const QString id = savedChoiceId();
    if (id.isEmpty() || id == QLatin1String(kAutoId)) {
        s_appliedSummary = QStringLiteral("Auto (system default — no override)");
        return;
    }
    const auto gpus = available();
    const GpuInfo* chosen = nullptr;
    for (const auto& g : gpus) {
        if (g.id == id) { chosen = &g; break; }
    }
    if (!chosen) {
        s_appliedSummary = QStringLiteral("saved GPU '%1' not present — using system default").arg(id);
        return;
    }

#if defined(Q_OS_LINUX)
    // Honour an explicit user override of the offload env.
    if (qEnvironmentVariableIsSet("__NV_PRIME_RENDER_OFFLOAD")
            || qEnvironmentVariableIsSet("__GLX_VENDOR_LIBRARY_NAME")) {
        s_appliedSummary = QStringLiteral("'%1' requested, but __NV_PRIME/__GLX env already set — left as-is")
                               .arg(chosen->name);
        return;
    }
    // The right lever differs by windowing system: __GLX_VENDOR_LIBRARY_NAME only
    // applies under X11/XWayland (GLX).  Under Wayland the app uses EGL, where it
    // is useless and =nvidia can raise GLX BadValue, so set only the
    // windowing-agnostic offload hints.
    const bool wayland = willUseWayland();
    if (chosen->name.contains(QLatin1String("NVIDIA"))) {
        qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");        // GLX + EGL offload hint
        qputenv("__VK_LAYER_NV_optimus", "NVIDIA_only");  // Vulkan offload hint
        if (wayland) {
            s_appliedSummary = chosen->name
                + QStringLiteral(" via __NV_PRIME_RENDER_OFFLOAD (Wayland/EGL offload)");
        } else {
            qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
            s_appliedSummary = chosen->name
                + QStringLiteral(" via PRIME offload (X11/GLX, __GLX_VENDOR_LIBRARY_NAME=nvidia)");
        }
    } else {
        // Integrated / AMD.  Under Wayland the integrated GPU is already the
        // default, so nothing to force; under X11 pin the Mesa GLX vendor.
        if (wayland) {
            s_appliedSummary = chosen->name + QStringLiteral(" (Wayland default — no override needed)");
        } else {
            qputenv("__GLX_VENDOR_LIBRARY_NAME", "mesa");
            s_appliedSummary = chosen->name + QStringLiteral(" via __GLX_VENDOR_LIBRARY_NAME=mesa (X11/GLX)");
        }
    }
#elif defined(Q_OS_WIN)
    if (qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX")) {
        s_appliedSummary = QStringLiteral("'%1' requested, but QT_D3D_ADAPTER_INDEX already set — left as-is")
                               .arg(chosen->name);
        return;
    }
    if (chosen->id.startsWith(QLatin1String("dxgi:"))) {
        const QByteArray idx = chosen->id.mid(5).toUtf8();
        qputenv("QT_D3D_ADAPTER_INDEX", idx);
        s_appliedSummary = chosen->name + QStringLiteral(" via QT_D3D_ADAPTER_INDEX=") + QString::fromUtf8(idx);
    }
#endif
}

QString GpuSelector::appliedSummary()
{
    return s_appliedSummary;
}

} // namespace AetherSDR
