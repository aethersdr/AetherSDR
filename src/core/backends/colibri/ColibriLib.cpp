#include "core/backends/colibri/ColibriLib.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutexLocker>

Q_LOGGING_CATEGORY(lcColibriLib, "aether.colibri.lib")

namespace AetherSDR::colibri {

int colibriSampleRateIndex(int hz) noexcept
{
    int i = 0;
    for (const int rate : kColibriSampleRatesHz) {
        if (rate == hz)
            return i;
        ++i;
    }
    return -1;
}

ColibriLib& ColibriLib::instance()
{
    static ColibriLib lib;
    return lib;
}

QStringList ColibriLib::candidatePaths(const QString& explicitPath)
{
    QStringList paths;
    if (!explicitPath.isEmpty())
        paths << explicitPath;
    // The deployed location: next to the executable, like every other
    // runtime-loaded DLL this application ships (see NvidiaAfxFilter).
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const QString base = QStringLiteral("colibrinano_lib.dll");
#else
    const QString base = QStringLiteral("libcolibrinano_lib.so");
#endif
    paths << QDir(appDir).filePath(base);
    // Bare name last: lets the OS loader search PATH / LD_LIBRARY_PATH, which
    // is how a system-wide vendor install is found without us guessing at it.
    paths << base;
    return paths;
}

bool ColibriLib::ensureLoaded(const QString& explicitPath, QString* error)
{
    QMutexLocker lock(&m_mutex);
    if (m_initialized)
        return true;

    for (const QString& candidate : candidatePaths(explicitPath)) {
        m_lib.setFileName(candidate);
        if (m_lib.load()) {
            m_path = candidate;
            break;
        }
    }
    if (!m_lib.isLoaded()) {
        if (error)
            *error = QStringLiteral("colibrinano_lib not found (tried: %1)")
                         .arg(candidatePaths(explicitPath).join(QStringLiteral("; ")));
        return false;
    }

    auto resolve = [this](const char* name) { return m_lib.resolve(name); };
    m_initialize = reinterpret_cast<FnVoid>(resolve("initialize"));
    m_finalize = reinterpret_cast<FnVoid>(resolve("finalize"));
    m_version = reinterpret_cast<FnVersion>(resolve("version"));
    m_information = reinterpret_cast<FnInformation>(resolve("information"));
    m_devices = reinterpret_cast<FnDevices>(resolve("devices"));
    m_open = reinterpret_cast<FnOpen>(resolve("open"));
    m_close = reinterpret_cast<FnClose>(resolve("close"));
    m_start = reinterpret_cast<FnStart>(resolve("start"));
    m_stop = reinterpret_cast<FnStop>(resolve("stop"));
    m_setPreamp = reinterpret_cast<FnSetPreamp>(resolve("setPream"));
    m_setFrequency = reinterpret_cast<FnSetFrequency>(resolve("setFrequency"));

    if (!m_initialize || !m_finalize || !m_version || !m_information || !m_devices
        || !m_open || !m_close || !m_start || !m_stop || !m_setPreamp
        || !m_setFrequency) {
        if (error)
            *error = QStringLiteral("%1 is not a colibrinano_lib (missing exports)")
                         .arg(m_path);
        m_lib.unload();
        m_path.clear();
        return false;
    }

    m_initialize();
    m_initialized = true;

    std::uint32_t maj = 0, min = 0, pat = 0;
    m_version(maj, min, pat);
    qCInfo(lcColibriLib) << "loaded" << m_path
                         << QStringLiteral("v%1.%2.%3").arg(maj).arg(min).arg(pat);
    return true;
}

void ColibriLib::version(std::uint32_t& major, std::uint32_t& minor, std::uint32_t& patch)
{
    QMutexLocker lock(&m_mutex);
    major = minor = patch = 0;
    if (m_version)
        m_version(major, minor, patch);
}

QString ColibriLib::information()
{
    QMutexLocker lock(&m_mutex);
    if (!m_information)
        return {};
    char* s = nullptr;
    m_information(&s);
    return s ? QString::fromLatin1(s) : QString{};
}

std::uint32_t ColibriLib::deviceCount()
{
    QMutexLocker lock(&m_mutex);
    if (!m_devices)
        return 0;
    std::uint32_t n = 0;
    m_devices(n);
    return n;
}

bool ColibriLib::open(ColibriDescriptor* out, std::uint32_t index)
{
    QMutexLocker lock(&m_mutex);
    return m_open ? m_open(out, index) : false;
}

void ColibriLib::close(ColibriDescriptor dev)
{
    QMutexLocker lock(&m_mutex);
    if (m_close)
        m_close(dev);
}

bool ColibriLib::start(ColibriDescriptor dev, int sampleRateIndex, ColibriRxCallback cb,
                       void* user)
{
    QMutexLocker lock(&m_mutex);
    return m_start ? m_start(dev, sampleRateIndex, cb, user) : false;
}

bool ColibriLib::stop(ColibriDescriptor dev)
{
    QMutexLocker lock(&m_mutex);
    return m_stop ? m_stop(dev) : false;
}

bool ColibriLib::setPreamp(ColibriDescriptor dev, float db)
{
    QMutexLocker lock(&m_mutex);
    return m_setPreamp ? m_setPreamp(dev, db) : false;
}

bool ColibriLib::setFrequency(ColibriDescriptor dev, std::uint32_t hz)
{
    QMutexLocker lock(&m_mutex);
    return m_setFrequency ? m_setFrequency(dev, hz) : false;
}

}  // namespace AetherSDR::colibri
