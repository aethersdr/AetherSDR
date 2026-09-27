#ifdef HAVE_DFNR

#include "DeepFilterFilter.h"
#include "Resampler.h"
#include "deep_filter.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
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

DeepFilterFilter::DeepFilterFilter(int sampleRate)
    : m_sampleRate(sampleRate)
{
    if (sampleRate != 24000 && sampleRate != 48000) {
        qWarning() << "DeepFilterFilter: unsupported sample rate" << sampleRate;
        return;
    }
    createStates();
    if (isValid()) {
        qDebug() << "DeepFilterFilter: initialized, frame size =" << m_frameSize;
    }
}

DeepFilterFilter::~DeepFilterFilter()
{
    freeStates();
}

void DeepFilterFilter::createStates()
{
    for (int channel = 0; channel < 2; ++channel) {
        if (m_sampleRate == 24000) {
            m_up[channel] = std::make_unique<Resampler>(24000, 48000);
            m_down[channel] = std::make_unique<Resampler>(48000, 24000);
        }
    }
    QByteArray modelPath = findModelPath();
    if (modelPath.isEmpty()) {
        return;
    }
    qDebug() << "DeepFilterFilter: loading model from" << modelPath;
    for (auto& state : m_states) {
        state = df_create(modelPath.constData(), m_attenLimit.load(), nullptr);
    }
    if (!isValid()) {
        qWarning() << "DeepFilterFilter: df_create() failed!";
        freeStates();
        return;
    }
    m_frameSize = static_cast<int>(df_get_frame_length(m_states[0]));
}

void DeepFilterFilter::freeStates()
{
    for (auto& state : m_states) {
        if (state) {
            df_free(state);
            state = nullptr;
        }
    }
}

void DeepFilterFilter::reset()
{
    if (m_sampleRate != 24000 && m_sampleRate != 48000) {
        return;
    }
    freeStates();
    createStates();
    for (auto& accum : m_inAccum) {
        accum.clear();
    }
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

QByteArray DeepFilterFilter::process(const QByteArray& pcmStereo)
{
    if (!isValid() || m_frameSize <= 0 || pcmStereo.isEmpty()) {
        return pcmStereo;
    }

    // Apply any pending parameter changes (main thread writes atomic, audio thread reads here)
    if (m_paramsDirty.exchange(false)) {
        for (auto state : m_states) {
            df_set_atten_lim(state, m_attenLimit.load());
            df_set_post_filter_beta(state, m_postFilterBeta.load());
        }
    }

    const auto* src = reinterpret_cast<const float*>(pcmStereo.constData());
    const int stereoFrames = pcmStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // Both channels see the same sample counts through identically configured
    // resamplers, so they reach the same whole-frame count and their
    // DeepFilterNet states advance in lockstep.
    int completeFrames = std::numeric_limits<int>::max();
    std::array<int, 2> totalAccumSamples{0, 0};
    for (int channel = 0; channel < 2; ++channel) {
        // 1. Split out this channel, then upsample legacy24 or pass native48
        //    directly to the model.
        auto& channelInput = m_channelInput[channel];
        channelInput.resize(stereoFrames);
        for (int i = 0; i < stereoFrames; ++i) {
            channelInput[i] = src[i * 2 + channel];
        }
        QByteArray input48k = m_up[channel]
            ? m_up[channel]->process(channelInput.data(), stereoFrames)
            : QByteArray(reinterpret_cast<const char*>(channelInput.data()),
                         stereoFrames * static_cast<int>(sizeof(float)));

        // 2. Append to this channel's accumulator.
        const int samples48k = input48k.size() / static_cast<int>(sizeof(float));
        const int prevAccumSamples =
            m_inAccum[channel].size() / static_cast<int>(sizeof(float));
        m_inAccum[channel].append(input48k);
        totalAccumSamples[channel] = prevAccumSamples + samples48k;
        completeFrames = std::min(completeFrames,
                                  totalAccumSamples[channel] / m_frameSize);
    }

    if (completeFrames > 0) {
        const int consumedSamples = completeFrames * m_frameSize;
        int outputFrames = std::numeric_limits<int>::max();
        for (int channel = 0; channel < 2; ++channel) {
            auto* accumData = reinterpret_cast<float*>(m_inAccum[channel].data());
            auto& processed = m_processed48k[channel];
            processed.resize(static_cast<std::size_t>(consumedSamples));
            for (int f = 0; f < completeFrames; ++f) {
                df_process_frame(m_states[channel],
                                 &accumData[f * m_frameSize],
                                 &processed[f * m_frameSize]);
            }

            // Keep leftover input samples
            const int leftoverSamples = totalAccumSamples[channel] - consumedSamples;
            m_inAccum[channel].remove(0, consumedSamples * static_cast<int>(sizeof(float)));
            Q_ASSERT(m_inAccum[channel].size()
                     == leftoverSamples * static_cast<int>(sizeof(float)));

            // 3. Convert the model output only for legacy24.
            m_channelOutput[channel] = m_down[channel]
                ? m_down[channel]->process(processed.data(), consumedSamples)
                : QByteArray(reinterpret_cast<const char*>(processed.data()),
                             consumedSamples * static_cast<int>(sizeof(float)));
            outputFrames = std::min(
                outputFrames,
                static_cast<int>(m_channelOutput[channel].size() / sizeof(float)));
        }

        const auto* left = reinterpret_cast<const float*>(m_channelOutput[0].constData());
        const auto* right = reinterpret_cast<const float*>(m_channelOutput[1].constData());
        const int start = m_outAccum.size() / static_cast<int>(sizeof(float));
        m_outAccum.resize((start + outputFrames * 2) * static_cast<int>(sizeof(float)));
        auto* stereo = reinterpret_cast<float*>(m_outAccum.data()) + start;
        for (int i = 0; i < outputFrames; ++i) {
            stereo[i * 2] = std::clamp(left[i], -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(right[i], -1.0f, 1.0f);
        }
    }

    // 4. Return exactly the same number of bytes as input
    const int needed = pcmStereo.size();
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
