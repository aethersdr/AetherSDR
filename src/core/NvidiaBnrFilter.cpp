#include "NvidiaBnrFilter.h"

#include <QDebug>

#ifdef HAVE_BNR
#include <cstring>
#endif

namespace MasterSDR {

NvidiaBnrFilter::NvidiaBnrFilter(QObject* parent)
    : QObject(parent)
{}

NvidiaBnrFilter::~NvidiaBnrFilter()
{
    disconnect();
}

#ifdef HAVE_BNR

bool NvidiaBnrFilter::connectToServer(const QString& address)
{
    if (m_connected.load()) disconnect();

    m_stopping.store(false);

    m_channel = grpc::CreateChannel(address.toStdString(),
                                     grpc::InsecureChannelCredentials());
    m_stub = nvidia::maxine::bnr::v1::MaxineBNR::NewStub(m_channel);

    m_context = std::make_unique<grpc::ClientContext>();
    m_stream = m_stub->EnhanceAudio(m_context.get());

    if (!m_stream) {
        qWarning() << "NvidiaBnrFilter: failed to open gRPC stream to" << address;
        emit errorOccurred("Failed to open gRPC stream");
        return false;
    }

    // Send initial config
    nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
    auto* config = configReq.mutable_config();
    config->set_intensity_ratio(m_intensityRatio);

    if (!m_stream->Write(configReq)) {
        qWarning() << "NvidiaBnrFilter: failed to send config";
        m_stream.reset();
        m_context.reset();
        emit errorOccurred("Failed to send configuration");
        return false;
    }

    m_connected.store(true);
    {
        QMutexLocker lock(&m_inMutex);
        m_inBuf.clear();
    }
    {
        QMutexLocker lock(&m_outMutex);
        m_outBuf.clear();
    }

    // Start worker thread (handles all gRPC read/write)
    m_workerThread = std::thread(&NvidiaBnrFilter::workerLoop, this);

    qDebug() << "NvidiaBnrFilter: connected to" << address
             << "intensity:" << m_intensityRatio;
    emit connectionChanged(true);
    return true;
}

void NvidiaBnrFilter::disconnect()
{
    if (!m_connected.load() && !m_workerThread.joinable()) return;

    m_stopping.store(true);
    m_connected.store(false);

    // Cancel the gRPC context to unblock any pending Read/Write in the worker
    if (m_context)
        m_context->TryCancel();

    if (m_workerThread.joinable())
        m_workerThread.join();

    // Clean up after worker has exited
    if (m_stream) {
        m_stream.reset();
    }
    m_context.reset();

    {
        QMutexLocker lock(&m_inMutex);
        m_inBuf.clear();
    }
    {
        QMutexLocker lock(&m_outMutex);
        m_outBuf.clear();
    }

    qDebug() << "NvidiaBnrFilter: disconnected";
    emit connectionChanged(false);
}

bool NvidiaBnrFilter::isConnected() const
{
    return m_connected.load();
}

void NvidiaBnrFilter::setIntensityRatio(float ratio)
{
    m_intensityRatio = std::clamp(ratio, 0.0f, 1.0f);
    m_configDirty.store(true);
}

QByteArray NvidiaBnrFilter::process(const float* samples, int numSamples)
{
    if (!m_connected.load()) return {};

    // Non-blocking: push samples into input buffer for the worker thread
    {
        QMutexLocker lock(&m_inMutex);
        m_inBuf.append(reinterpret_cast<const char*>(samples),
                       numSamples * sizeof(float));

        // Cap input buffer at ~200ms to prevent runaway growth
        constexpr int maxInBytes = kFrameBytes * 20;
        if (m_inBuf.size() > maxInBytes)
            m_inBuf.remove(0, m_inBuf.size() - maxInBytes);
    }

    // Non-blocking: return any denoised data from the worker thread
    QMutexLocker lock(&m_outMutex);
    if (m_outBuf.isEmpty()) return {};

    QByteArray result;
    result.swap(m_outBuf);
    return result;
}

void NvidiaBnrFilter::workerLoop()
{
    // Worker thread: pulls from m_inBuf, writes to gRPC, reads responses,
    // pushes to m_outBuf. All gRPC I/O happens here, never on audio/main thread.

    while (!m_stopping.load()) {
        // Send config update if intensity changed
        if (m_configDirty.exchange(false)) {
            nvidia::maxine::bnr::v1::EnhanceAudioRequest configReq;
            auto* config = configReq.mutable_config();
            config->set_intensity_ratio(m_intensityRatio);
            m_stream->Write(configReq);
        }

        // Pull a frame from input buffer
        QByteArray frame;
        {
            QMutexLocker lock(&m_inMutex);
            if (m_inBuf.size() >= kFrameBytes) {
                frame = m_inBuf.left(kFrameBytes);
                m_inBuf.remove(0, kFrameBytes);
            }
        }

        if (frame.isEmpty()) {
            // No data yet — sleep briefly to avoid busy-wait
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // Write frame to gRPC stream
        nvidia::maxine::bnr::v1::EnhanceAudioRequest req;
        req.set_audio_stream_data(frame.constData(), kFrameBytes);

        if (!m_stream->Write(req)) {
            if (!m_stopping.load()) {
                qWarning() << "NvidiaBnrFilter: gRPC write failed";
                m_connected.store(false);
                QMetaObject::invokeMethod(this, [this]() {
                    emit connectionChanged(false);
                    emit errorOccurred("gRPC write failed");
                }, Qt::QueuedConnection);
            }
            return;
        }

        // Read denoised response (blocking, but on worker thread — not audio)
        nvidia::maxine::bnr::v1::EnhanceAudioResponse response;
        if (!m_stream->Read(&response)) {
            if (!m_stopping.load()) {
                qWarning() << "NvidiaBnrFilter: gRPC read failed";
                m_connected.store(false);
                QMetaObject::invokeMethod(this, [this]() {
                    emit connectionChanged(false);
                    emit errorOccurred("BNR container stream ended");
                }, Qt::QueuedConnection);
            }
            return;
        }

        if (response.has_audio_stream_data()) {
            const auto& data = response.audio_stream_data();
            QMutexLocker lock(&m_outMutex);
            m_outBuf.append(data.data(), data.size());

            // Cap output buffer at ~200ms
            constexpr int maxOutBytes = kFrameBytes * 20;
            if (m_outBuf.size() > maxOutBytes)
                m_outBuf.remove(0, m_outBuf.size() - maxOutBytes);
        }
    }
}

#else // !HAVE_BNR — stub implementations

bool NvidiaBnrFilter::connectToServer(const QString&) { return false; }
void NvidiaBnrFilter::disconnect() {}
bool NvidiaBnrFilter::isConnected() const { return false; }
QByteArray NvidiaBnrFilter::process(const float*, int) { return {}; }
void NvidiaBnrFilter::setIntensityRatio(float ratio)
{
    m_intensityRatio = std::clamp(ratio, 0.0f, 1.0f);
}

#endif // HAVE_BNR

} // namespace MasterSDR
