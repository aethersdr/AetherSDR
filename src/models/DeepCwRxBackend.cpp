#include "DeepCwRxBackend.h"
#include "core/DeepCwCommitter.h"
#include "core/DeepCwEngine.h"
#include "core/LogManager.h"
#include "core/Resampler.h"
#include "core/deepfist/DeepFistModelAssets.h"
#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

namespace AetherSDR {
namespace {
// e04/deepcw-engine's only model revision (commit "init", 2026-06-15). The
// commit-pinned URL cannot move under us; size and SHA-256 are checked anyway.
constexpr auto kModelCommit = "9185d5da7d2344393d4e28352cd66c21ed83cad6";
const QString kModelFile = QStringLiteral("model.onnx");

QVector<DeepFistModelAssets::Asset> deepCwManifest()
{
    return {{kModelFile, 15139839,
             "ef120799457bca042d4690944f0faf93268eb4654e7f50f28784ad63bdc1fe02"}};
}
}

QString DeepCwRxBackend::modelDirectory()
{
    const QString overridePath = qEnvironmentVariable("AETHER_DEEPCW_MODEL_DIR");
    return overridePath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/models/deepcw-9185d5da") : overridePath;
}

QString DeepCwRxBackend::modelBaseUrl()
{
    return QStringLiteral("https://raw.githubusercontent.com/e04/deepcw-engine/%1/")
        .arg(QLatin1String(kModelCommit));
}

DeepCwRxBackend::DeepCwRxBackend(QObject* parent)
    : DeepCwRxBackend(modelDirectory(), modelBaseUrl(), nullptr, parent) {}

DeepCwRxBackend::DeepCwRxBackend(QString directory, QString baseUrl,
                                 QNetworkAccessManager* network, QObject* parent)
    : CwRxBackend(parent),
      m_assets(std::make_unique<DeepFistModelAssets>(directory, std::move(baseUrl),
                                                      deepCwManifest(), network)),
      m_directory(std::move(directory))
{
    connect(m_assets.get(), &DeepFistModelAssets::checking, this, [this] {
        setStatus(tr("Checking model…"));
    });
    connect(m_assets.get(), &DeepFistModelAssets::progress, this, [this](qint64 got, qint64 total) {
        setStatus(tr("Download %1%").arg(total > 0 ? got * 100 / total : 0));
    });
    connect(m_assets.get(), &DeepFistModelAssets::failed, this, [this](const QString& reason) {
        m_preparing = false;
        m_canRetry = true;
        m_detail = reason;
        setStatus(tr("Model unavailable"));
    });
    connect(m_assets.get(), &DeepFistModelAssets::ready, this, [this] {
        if (!m_started || !m_preparing) { return; }
        m_preparing = false;
        m_detail.clear();
        setStatus(tr("Loading DeepCW…"));
        launchWorker();
    });
}

DeepCwRxBackend::~DeepCwRxBackend()
{
    // Destruction must not publish status into a still-connected owner.
    disconnect();
    stop();
}

void DeepCwRxBackend::setStatus(const QString& status)
{
    if (status == m_status) { return; }
    m_status = status;
    emit statusChanged();
}

void DeepCwRxBackend::postStatus(quint64 runId, const QString& status, bool failure)
{
    QMetaObject::invokeMethod(this, [this, runId, status, failure] {
        if (runId != m_runId || !m_started) { return; }
        m_canRetry = failure;
        setStatus(status);
    }, Qt::QueuedConnection);
}

void DeepCwRxBackend::start()
{
    if (m_started) { return; }
    m_started = true;
    ++m_runId;
    if (m_loaded) {
        launchWorker();
        setStatus(tr("DeepCW ready"));
        return;
    }
    prepare();
}

void DeepCwRxBackend::prepare()
{
    m_preparing = true;
    m_canRetry = false;
    m_detail.clear();
    setStatus(tr("Checking model…"));
    m_assets->ensure();
}

void DeepCwRxBackend::stop()
{
    m_started = false;
    if (m_preparing) {
        m_assets->cancel();
        m_preparing = false;
    }
    m_canRetry = false;
    m_detail.clear();
    stopWorker();
    setStatus({});
}

void DeepCwRxBackend::cancelPreparation()
{
    if (!m_preparing) { return; }
    m_assets->cancel();
    m_preparing = false;
    m_canRetry = true;
    setStatus(tr("Download canceled"));
}

void DeepCwRxBackend::retry()
{
    if (!m_started || m_preparing || m_workerRun) { return; }
    stopWorker();  // joins a worker that exited on a failed load
    prepare();
}

void DeepCwRxBackend::launchWorker()
{
    if (m_worker) { return; }
    {
        QMutexLocker lock(&m_ringMutex);
        m_ring.clear();
    }
    m_resetRequested = false;
    m_workerRun = true;
    const quint64 runId = m_runId;
    const QString modelPath = QDir(m_directory).filePath(kModelFile);
    m_worker = QThread::create([this, runId, modelPath] { decodeLoop(runId, modelPath); });
    m_worker->setObjectName("DeepCwRx");
    m_worker->start();
}

void DeepCwRxBackend::stopWorker()
{
    if (!m_worker) { return; }
    m_workerRun = false;
    m_worker->wait();
    delete m_worker;
    m_worker = nullptr;
    QMutexLocker lock(&m_ringMutex);
    m_ring.clear();
}

void DeepCwRxBackend::reset()
{
    QMutexLocker lock(&m_ringMutex);
    m_ring.clear();
    m_resetRequested = true;
}

void DeepCwRxBackend::feedFixed24(const DecoderPcmBlock& block)
{
    if (!m_workerRun || !m_loaded || !block.current()) { return; }
    QMutexLocker lock(&m_ringMutex);
    if (block.discontinuity) {
        m_ring.clear();
        m_resetRequested = true;
    }
    m_ring.insert(m_ring.end(), block.samples.cbegin(), block.samples.cend());
    if (m_ring.size() > kRingCapacity) {
        m_ring.erase(m_ring.begin(),
                     m_ring.begin() + static_cast<std::ptrdiff_t>(m_ring.size() - kRingCapacity));
    }
}

// K5PTB's DeepCW worker loop (prototype CwDecoder::decodeLoopDeep), fed by the
// 24 kHz mono ring: resample to the model's 3200 Hz with an anti-aliased
// r8brain SRC (a 7.5x decimation; a naive resample folds energy into the
// 400-1200 Hz analysis band), then DeepCwCommitter: a sliding window re-decoded
// every 2 s whose characters are shown once they are 5 s behind the live edge.
void DeepCwRxBackend::decodeLoop(quint64 runId, const QString& modelPath)
{
    if (!m_loaded) {
        if (!m_engine) { m_engine = std::make_unique<DeepCwEngine>(); }
        m_loaded = m_engine->loadModel(modelPath.toStdString());
        qCInfo(lcDsp) << "DeepCwRxBackend: model load" << (m_loaded ? "ok" : "FAILED") << modelPath;
        if (!m_loaded) {
            m_workerRun = false;
            postStatus(runId, tr("Model load failed"), true);
            return;
        }
    }
    postStatus(runId, tr("DeepCW ready"), false);

    constexpr int kRate = DeepCwEngine::kModelSampleRate;
    constexpr double holdSec = 5.0;

    // Worker-local: neither the resampler nor the committer is thread-safe.
    auto resampler = std::make_unique<Resampler>(24000.0, static_cast<double>(kRate));
    auto committer = std::make_unique<DeepCwCommitter>(holdSec);

    while (m_workerRun) {
        if (m_resetRequested.exchange(false)) {
            resampler = std::make_unique<Resampler>(24000.0, static_cast<double>(kRate));
            committer = std::make_unique<DeepCwCommitter>(holdSec);
        }
        std::vector<float> in24k;
        {
            QMutexLocker lock(&m_ringMutex);
            in24k.swap(m_ring);
        }
        if (!in24k.empty()) {
            const QByteArray out = resampler->process(in24k.data(), static_cast<int>(in24k.size()));
            const auto* r = reinterpret_cast<const float*>(out.constData());
            const auto m = static_cast<std::size_t>(out.size() / static_cast<int>(sizeof(float)));
            const DeepCwCommitter::Result res = committer->push(r, m, *m_engine);
            // One colour per committed chunk: 1 - mean CTC confidence (lower is better).
            if (!res.text.empty()) {
                emit coloredTextDecoded(QString::fromStdString(res.text), 1.0f - res.meanConf);
            }
        }
        QThread::msleep(200);
    }
}

} // namespace AetherSDR
