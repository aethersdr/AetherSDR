#include "DeepFistCwModel.h"
#include "core/Resampler.h"
#include "core/deepfist/DeepFistStream.h"
#include "core/deepfist/DeepFistModelAssets.h"
#include "DeepFistModel.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QPointer>
#include <algorithm>
#include <cstring>
#include <memory>

namespace AetherSDR {
namespace {
bool matches(const QString& path, qint64 size, const QByteArray& digest)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != size) { return false; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result().toHex() == digest;
}
}
DeepFistStream::Parameters DeepFistCwModel::appParameters()
{
    DeepFistStream::Parameters parameters;
    parameters.requireCompletedMark = true;
    parameters.carryPending = true;
    parameters.normalizeActivity = true;
    // The stream default (12) sits between dead air and a tuned-in signal (n9bc/DeepFist
    // tools/squelch.py); weak off-air CW scores in that gap and was gated out (#5950).
    // Steady carriers are held off by the completed-mark guard above, not by this value.
    parameters.activityThreshold = 3.f;
    return parameters;
}
DeepFistCwModel::DeepFistCwModel(QObject* parent)
    : DeepFistCwModel(modelDirectory(), DeepFistModelAssets::releaseBaseUrl(), nullptr, parent) {}
DeepFistCwModel::DeepFistCwModel(QString directory, QString baseUrl,
    QNetworkAccessManager* network, QObject* parent)
    : QObject(parent), m_assets(std::make_unique<DeepFistModelAssets>(directory, baseUrl,
          DeepFistModelAssets::manifest(), network)), m_directory(std::move(directory))
{
    m_parameters = appParameters();
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
        if (!m_running || !m_preparing) { return; }
        m_preparing = false;
        const quint64 generation = m_runId;
        const QPointer<DeepFistCwModel> guard(this);
        setStatus(tr("Loading DeepFist…"));
        if (!guard || !m_running || generation != m_runId) { return; }
        m_worker = std::thread([this, generation] { run(generation, m_directory); });
    });
}
DeepFistCwModel::DeepFistCwModel(DeepFistStream::Parameters parameters, QObject* parent)
    : DeepFistCwModel(parent)
{
    m_parameters = parameters;
}
DeepFistCwModel::~DeepFistCwModel()
{
    // Destruction must not publish status into a still-connected owner.
    disconnect();
    stop();
}
QString DeepFistCwModel::modelDirectory()
{
    // Development-only override in this opt-in prototype. No file picker or installer.
    const QString overridePath = qEnvironmentVariable("AETHER_DEEPFIST_MODEL_DIR");
    return overridePath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/models/deepfist-20260715-6d2d4e3d66f9") : overridePath;
}
void DeepFistCwModel::setStatus(const QString& status)
{
    if (status == m_status) { return; }
    m_status = status;
    emit statusChanged(status);
}
void DeepFistCwModel::postStatus(quint64 generation, const QString& status, bool failure)
{
    QMetaObject::invokeMethod(this, [this, generation, status, failure] {
        if (generation == m_runId && m_running) {
            m_canRetry = failure;
            setStatus(status);
        }
    }, Qt::QueuedConnection);
}
void DeepFistCwModel::start()
{
    if (m_running) { return; }
    m_running = true;
    m_stopping = false;
    m_acceptAudio = false;
    reset();
    ++m_runId;
    m_preparing = true;
    m_canRetry = false;
    m_detail.clear();
    const quint64 runId = m_runId;
    const QPointer<DeepFistCwModel> guard(this);
    setStatus(tr("Checking model…"));
    if (!guard || !m_running || !m_preparing || runId != m_runId) { return; }
    m_assets->ensure();
}
void DeepFistCwModel::cancelPreparation()
{
    if (!m_preparing) { return; }
    m_assets->cancel();
    m_preparing = false;
    m_canRetry = true;
    setStatus(tr("Download canceled"));
}
void DeepFistCwModel::retry()
{
    if (!m_running || !m_canRetry) { return; }
    const quint64 expectedRunId = m_runId + 1;
    const QPointer<DeepFistCwModel> guard(this);
    stop();
    if (guard && !m_running && m_runId == expectedRunId) { start(); }
}
void DeepFistCwModel::reset()
{
    std::lock_guard lock(m_mutex);
    ++m_generation;
    m_queue.clear();
    m_queuedFrames = 0;
    m_stream = {};
    m_nextSample = 0;
}
void DeepFistCwModel::stop()
{
    if (!m_running) { return; }
    m_running = false;
    m_assets->cancel();
    m_preparing = false;
    m_canRetry = false;
    m_detail.clear();
    m_acceptAudio = false;
    ++m_runId;
    m_stopping = true;
    reset();
    m_ready.notify_all();
    // Own state until the single current inference returns; never detach or free it early.
    if (m_worker.joinable()) { m_worker.join(); }
    setStatus({});
}
qsizetype DeepFistCwModel::queuedFrames()
{
    std::lock_guard lock(m_mutex);
    return m_queuedFrames;
}
void DeepFistCwModel::feed(const PcmFrame& frame)
{
    if (!m_running || !m_acceptAudio.load() || !m_gate.accept(frame)) { return; }
    std::lock_guard lock(m_mutex);
    const bool changed = m_stream != frame.stream() || frame.discontinuity()
        || frame.firstSample() != m_nextSample;
    const qsizetype capacity = frame.stream().format.sampleRateHz * 2;
    if (changed || m_queuedFrames + frame.frameCount() > capacity) {
        ++m_generation;
        m_queue.clear();
        m_queuedFrames = 0;
    }
    m_stream = frame.stream();
    m_nextSample = frame.firstSample() + frame.frameCount();
    if (frame.frameCount() > capacity) { return; }
    m_queue.push_back({frame, m_generation.load()});
    m_queuedFrames += frame.frameCount();
    m_ready.notify_one();
}
void DeepFistCwModel::run(quint64 runId, const QString& directory)
{
    // One manifest, owned by DeepFistModelAssets. Re-declaring the sizes and
    // hashes here let a model bump update one copy and not the other, which
    // downloads and verifies clean and then reports "Model unavailable" forever.
    for (const DeepFistModelAssets::Asset& asset : DeepFistModelAssets::manifest()) {
        if (asset.name == QLatin1String("LICENSE")) { continue; }
        if (!matches(QDir(directory).filePath(asset.name), asset.bytes, asset.sha256)) {
            postStatus(runId, tr("Model unavailable"), true);
            return;
        }
    }
    lyra::dsp::DeepFistModel model;
    if (!model.load(directory.toStdString())) {
        postStatus(runId, tr("Model load failed"), true);
        return;
    }
    m_acceptAudio = true;
    postStatus(runId, tr("DeepFist ready"));
    quint64 generation = 0;
    std::unique_ptr<Resampler> resampler;
    DeepFistStream stream(m_parameters);
    QByteArray converted;
    while (!m_stopping.load()) {
        Item item;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [this] { return m_stopping.load() || !m_queue.empty(); });
            if (m_stopping.load()) { break; }
            item = std::move(m_queue.front());
            m_queue.pop_front();
            m_queuedFrames -= item.frame.frameCount();
        }
        if (!item.frame.current() || item.generation != m_generation.load()) { continue; }
        if (!resampler || generation != item.generation) {
            generation = item.generation;
            resampler = std::make_unique<Resampler>(item.frame.stream().format.sampleRateHz, 3200, 4096);
            stream = DeepFistStream(m_parameters);
        }
        const int channels = item.frame.stream().format.channels();
        QString output;
        for (qsizetype offset = 0; offset < item.frame.frameCount(); offset += 4096) {
            if (m_stopping.load() || generation != m_generation.load()) { break; }
            const int count = static_cast<int>(std::min<qsizetype>(4096, item.frame.frameCount() - offset));
            const float* samples = item.frame.samples().constData() + offset * channels;
            converted = channels == 2 ? resampler->processStereoToMono(samples, count)
                                      : resampler->process(samples, count);
            // QByteArray storage is not a float object array; copy before accessing it.
            std::vector<float> mono(converted.size() / sizeof(float));
            std::memcpy(mono.data(), converted.constData(), converted.size());
            m_processing = true;
            output += stream.process(mono.data(), static_cast<int>(mono.size()), model);
            m_processing = false;
            if (stream.failed()) {
                m_acceptAudio = false;
                postStatus(runId, tr("Decode failed"), true);
                return;
            }
        }
        if (output.isEmpty()) { continue; }
        const PcmFrame frame = item.frame;
        QMetaObject::invokeMethod(this, [this, generation, frame, output] {
            if (m_running && generation == m_generation.load() && frame.current()) {
                emit textDecoded(output);
            }
        }, Qt::QueuedConnection);
    }
}
}
