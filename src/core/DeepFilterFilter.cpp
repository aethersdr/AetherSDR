#ifdef HAVE_DFNR

#include "DeepFilterFilter.h"
#include "Resampler.h"
#include "deep_filter.h"

#include <cstring>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QStandardPaths>

namespace MasterSDR {

static constexpr const char* kModelFileName = "DeepFilterNet3_onnx.tar.gz";

static QByteArray findModelPath()
{
    QString exeDir = QCoreApplication::applicationDirPath();
    QStringList searched;

    // 1. Adjacent to the executable (Linux/Windows build dir, or installed)
    QString path = exeDir + "/" + kModelFileName;
    searched << path;
    if (QFile::exists(path)) {
        return path.toUtf8();
    }
    // 2. macOS app bundle: Contents/Resources/
    path = exeDir + "/../Resources/" + kModelFileName;
    searched << path;
    if (QFile::exists(path)) {
        return QDir(path).canonicalPath().toUtf8();
    }
    // 3. Dev builds: third_party dir relative to exe
    path = exeDir + "/../third_party/deepfilter/models/" + kModelFileName;
    searched << path;
    if (QFile::exists(path)) {
        return QDir(path).canonicalPath().toUtf8();
    }
    // 4. XDG data directory (Linux installed via package or cmake --install)
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!dataDir.isEmpty()) {
        path = dataDir + "/MasterSDR/" + kModelFileName;
        searched << path;
        if (QFile::exists(path)) {
            return path.toUtf8();
        }
    }
    // 5. System-wide install paths (Linux)
    for (const QString& prefix : {QStringLiteral("/usr/share"), QStringLiteral("/usr/local/share")}) {
        path = prefix + "/MasterSDR/" + kModelFileName;
        searched << path;
        if (QFile::exists(path)) {
            return path.toUtf8();
        }
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

} // namespace MasterSDR

#endif // HAVE_DFNR
