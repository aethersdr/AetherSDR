#ifdef HAVE_DFNR

#include "DeepFilterFilter.h"
#include "Resampler.h"
#include "deep_filter.h"

#include <cstring>
#include <vector>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace AetherSDR {

static constexpr const char* kEmbeddedModelFileName = "DeepFilterNet3_onnx.dfmodel";
static constexpr const char* kLegacyModelFileName = "DeepFilterNet3_onnx.tar.gz";
static constexpr const char* kEmbeddedModelResource = ":/models/DeepFilterNet3_onnx.dfmodel";

static QByteArray existingModelPath(const QString& path, QStringList& searched)
{
    searched << path;
    if (!QFile::exists(path)) {
        return {};
    }

    const QString canonical = QFileInfo(path).canonicalFilePath();
    return (canonical.isEmpty() ? path : canonical).toUtf8();
}

static QByteArray findModelInDirectory(const QString& directory, QStringList& searched)
{
    const QDir dir(directory);
    for (const char* fileName : {kEmbeddedModelFileName, kLegacyModelFileName}) {
        const QByteArray path = existingModelPath(dir.filePath(QString::fromLatin1(fileName)), searched);
        if (!path.isEmpty()) {
            return path;
        }
    }
    return {};
}

static QByteArray extractEmbeddedModel(QStringList& searched)
{
    const QString resourcePath = QString::fromLatin1(kEmbeddedModelResource);
    searched << resourcePath;

    QFile resource(resourcePath);
    if (!resource.exists()) {
        return {};
    }
    if (!resource.open(QIODevice::ReadOnly)) {
        qWarning() << "DeepFilterFilter: embedded model resource exists but could not be opened:"
                   << resource.errorString();
        return {};
    }

    const QByteArray modelBytes = resource.readAll();
    if (modelBytes.isEmpty() && resource.size() > 0) {
        qWarning() << "DeepFilterFilter: embedded model resource could not be read:"
                   << resource.errorString();
        return {};
    }

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dataDir.isEmpty()) {
        dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    if (dataDir.isEmpty()) {
        qWarning() << "DeepFilterFilter: no writable app data directory for embedded model cache";
        return {};
    }

    const QString modelDir = QDir(dataDir).filePath(QStringLiteral("models"));
    if (!QDir().mkpath(modelDir)) {
        qWarning() << "DeepFilterFilter: could not create embedded model cache directory" << modelDir;
        return {};
    }

    const QString targetPath = QDir(modelDir).filePath(QString::fromLatin1(kEmbeddedModelFileName));
    const QString hashPath = targetPath + QStringLiteral(".sha256");
    searched << targetPath;

    const QByteArray resourceHash =
        QCryptographicHash::hash(modelBytes, QCryptographicHash::Sha256).toHex();

    bool cachedHashMatches = false;
    if (QFileInfo::exists(targetPath)) {
        QFile hashFile(hashPath);
        if (hashFile.open(QIODevice::ReadOnly)) {
            cachedHashMatches = hashFile.readAll().trimmed() == resourceHash;
        }
    }
    if (cachedHashMatches) {
        const QString canonical = QFileInfo(targetPath).canonicalFilePath();
        return (canonical.isEmpty() ? targetPath : canonical).toUtf8();
    }

    QSaveFile targetFile(targetPath);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        qWarning() << "DeepFilterFilter: could not open embedded model cache for writing:"
                   << targetPath << targetFile.errorString();
        return {};
    }
    if (targetFile.write(modelBytes) != modelBytes.size()) {
        qWarning() << "DeepFilterFilter: could not write embedded model cache:"
                   << targetPath << targetFile.errorString();
        return {};
    }
    if (!targetFile.commit()) {
        qWarning() << "DeepFilterFilter: could not commit embedded model cache:"
                   << targetPath << targetFile.errorString();
        return {};
    }

    QSaveFile newHashFile(hashPath);
    if (newHashFile.open(QIODevice::WriteOnly)) {
        newHashFile.write(resourceHash);
        if (!newHashFile.commit()) {
            qWarning() << "DeepFilterFilter: could not commit embedded model cache hash:"
                       << hashPath << newHashFile.errorString();
        }
    }

    const QString canonical = QFileInfo(targetPath).canonicalFilePath();
    return (canonical.isEmpty() ? targetPath : canonical).toUtf8();
}

static QByteArray findModelPath()
{
    QString exeDir = QCoreApplication::applicationDirPath();
    QStringList searched;

    // 1. Adjacent to the executable (Linux/Windows build dir, or installed)
    QByteArray path = findModelInDirectory(exeDir, searched);
    if (!path.isEmpty()) {
        return path;
    }
    // 2. macOS app bundle: Contents/Resources/
    path = findModelInDirectory(QDir(exeDir).filePath(QStringLiteral("../Resources")), searched);
    if (!path.isEmpty()) {
        return path;
    }
    // 3. Dev builds: third_party dir relative to exe
    path = findModelInDirectory(QDir(exeDir).filePath(QStringLiteral("../third_party/deepfilter/models")), searched);
    if (!path.isEmpty()) {
        return path;
    }
    // 4. XDG data directory (Linux installed via package or cmake --install)
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!dataDir.isEmpty()) {
        path = findModelInDirectory(QDir(dataDir).filePath(QStringLiteral("AetherSDR")), searched);
        if (!path.isEmpty()) {
            return path;
        }
    }
    // 5. System-wide install paths (Linux)
    for (const QString& prefix : {QStringLiteral("/usr/share"), QStringLiteral("/usr/local/share")}) {
        path = findModelInDirectory(QDir(prefix).filePath(QStringLiteral("AetherSDR")), searched);
        if (!path.isEmpty()) {
            return path;
        }
    }
    // 6. Store-ready Windows builds embed the model payload in Qt resources and
    // materialize it into writable app-local data because libdf requires a path.
    path = extractEmbeddedModel(searched);
    if (!path.isEmpty()) {
        return path;
    }

    qWarning() << "DeepFilterFilter: model not found. Searched:" << searched;
    return {};
}

DeepFilterFilter::DeepFilterFilter()
    : m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
{
    QByteArray modelPath = findModelPath();
    if (modelPath.isEmpty()) {
        return;
    }
    qDebug() << "DeepFilterFilter: loading model from" << modelPath;
    m_state = df_create(modelPath.constData(), m_attenLimit.load(), nullptr);
    if (m_state) {
        m_frameSize = static_cast<int>(df_get_frame_length(m_state));
        qDebug() << "DeepFilterFilter: initialized, frame size =" << m_frameSize;
    } else {
        qWarning() << "DeepFilterFilter: df_create() failed!";
    }
}

DeepFilterFilter::~DeepFilterFilter()
{
    if (m_state) {
        df_free(m_state);
    }
}

void DeepFilterFilter::reset()
{
    if (m_state) {
        df_free(m_state);
        m_state = nullptr;
    }
    QByteArray modelPath = findModelPath();
    if (!modelPath.isEmpty()) {
        m_state = df_create(modelPath.constData(), m_attenLimit.load(), nullptr);
        if (m_state) {
            m_frameSize = static_cast<int>(df_get_frame_length(m_state));
        }
    }
    m_up = std::make_unique<Resampler>(24000, 48000);
    m_down = std::make_unique<Resampler>(48000, 24000);
    m_inAccum.clear();
    m_outAccum.clear();
    m_paramsDirty.store(true);
}

void DeepFilterFilter::setAttenLimit(float db)
{
    m_attenLimit.store(db);
    m_paramsDirty.store(true);
}

void DeepFilterFilter::setPostFilterBeta(float beta)
{
    m_postFilterBeta.store(beta);
    m_paramsDirty.store(true);
}

QByteArray DeepFilterFilter::process(const QByteArray& pcm24kStereo)
{
    if (!m_state || m_frameSize <= 0 || pcm24kStereo.isEmpty()) {
        return pcm24kStereo;
    }

    // Apply any pending parameter changes (main thread writes atomic, audio thread reads here)
    if (m_paramsDirty.exchange(false)) {
        df_set_atten_lim(m_state, m_attenLimit.load());
        df_set_post_filter_beta(m_state, m_postFilterBeta.load());
    }

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoFrames = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // 1. Upsample 24kHz stereo float32 → 48kHz mono float32 via r8brain
    QByteArray mono48k = m_up->processStereoToMono(src, stereoFrames);

    // Already float32 in [-1, 1] range — DeepFilterNet's native format
    const auto* mono48kSamples = reinterpret_cast<const float*>(mono48k.constData());
    const int monoSamples48k = mono48k.size() / static_cast<int>(sizeof(float));

    // 2. Append to input accumulator and process complete frames
    const int prevAccumSamples = m_inAccum.size() / static_cast<int>(sizeof(float));
    {
        const int startIdx = prevAccumSamples;
        m_inAccum.resize((startIdx + monoSamples48k) * sizeof(float));
        auto* floatBuf = reinterpret_cast<float*>(m_inAccum.data());
        for (int i = 0; i < monoSamples48k; ++i) {
            floatBuf[startIdx + i] = mono48kSamples[i];
        }
    }

    const int totalAccumSamples = prevAccumSamples + monoSamples48k;
    const int completeFrames = totalAccumSamples / m_frameSize;

    if (completeFrames > 0) {
        auto* accumData = reinterpret_cast<float*>(m_inAccum.data());
        std::vector<float> processed48k(completeFrames * m_frameSize);

        for (int f = 0; f < completeFrames; ++f) {
            df_process_frame(m_state,
                             &accumData[f * m_frameSize],
                             &processed48k[f * m_frameSize]);
        }

        // Keep leftover input samples
        const int consumedSamples = completeFrames * m_frameSize;
        const int leftoverSamples = totalAccumSamples - consumedSamples;
        if (leftoverSamples > 0) {
            QByteArray leftover(reinterpret_cast<const char*>(&accumData[consumedSamples]),
                                leftoverSamples * sizeof(float));
            m_inAccum = leftover;
        } else {
            m_inAccum.clear();
        }

        // 3. Downsample processed 48kHz mono float32 → 24kHz stereo float32
        const int outputMonoSamples = completeFrames * m_frameSize;

        QByteArray downsampled = m_down->processMonoToStereo(
            processed48k.data(), outputMonoSamples);

        m_outAccum.append(downsampled);
    }

    // 4. Return exactly the same number of bytes as input
    const int needed = pcm24kStereo.size();
    if (m_outAccum.size() >= needed) {
        QByteArray result = m_outAccum.left(needed);
        m_outAccum.remove(0, needed);
        return result;
    }

    // Not enough output yet — return silence (only happens during startup)
    return QByteArray(needed, '\0');
}

} // namespace AetherSDR

#endif // HAVE_DFNR
