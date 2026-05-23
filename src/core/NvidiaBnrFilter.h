#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QMutex>

#ifdef HAVE_BNR
#include <memory>
#include <thread>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "bnr.grpc.pb.h"
#endif

namespace MasterSDR {

// gRPC client for NVIDIA NIM BNR (Background Noise Removal).
// All gRPC I/O runs on a dedicated worker thread to avoid blocking
// the audio callback or main thread. The audio thread pushes samples
// into an input buffer via process(), and reads denoised samples back.
//
// When built without HAVE_BNR, all methods are no-ops.
class NvidiaBnrFilter : public QObject {
    Q_OBJECT

public:
    explicit NvidiaBnrFilter(QObject* parent = nullptr);
    ~NvidiaBnrFilter() override;

    bool connectToServer(const QString& address = "localhost:8001");
    void disconnect();
    bool isConnected() const;

    // Non-blocking: pushes samples into input buffer, returns any available
    // denoised samples. Both input and output are 48kHz mono float32.
    QByteArray process(const float* samples, int numSamples);

    void setIntensityRatio(float ratio);
    float intensityRatio() const { return m_intensityRatio; }

signals:
    void connectionChanged(bool connected);
    void errorOccurred(const QString& message);

private:
    float m_intensityRatio{1.0f};

#ifdef HAVE_BNR
    void workerLoop();

    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<nvidia::maxine::bnr::v1::MaxineBNR::Stub> m_stub;
    std::unique_ptr<grpc::ClientContext> m_context;
    std::unique_ptr<grpc::ClientReaderWriter<
        nvidia::maxine::bnr::v1::EnhanceAudioRequest,
        nvidia::maxine::bnr::v1::EnhanceAudioResponse>> m_stream;

    // Worker thread handles all gRPC read/write
    std::thread m_workerThread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_configDirty{false};

    // Input buffer: audio thread writes, worker thread reads
    QMutex m_inMutex;
    QByteArray m_inBuf;

    // Output buffer: worker thread writes, audio thread reads
    QMutex m_outMutex;
    QByteArray m_outBuf;

    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = 480;  // 10ms at 48kHz
    static constexpr int kFrameBytes = kFrameSamples * sizeof(float);
#endif
};

} // namespace MasterSDR
