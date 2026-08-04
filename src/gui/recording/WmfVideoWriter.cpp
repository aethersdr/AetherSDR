#include "WmfVideoWriter.h"
#include <QDebug>
#include <QDir>
#include <cstdint>
#include <cstring>

namespace AetherSDR {

WmfVideoWriter::WmfVideoWriter() {
    // Balance CoUninitialize only when this thread's COM init succeeded for us.
    // S_OK / S_FALSE both require a matching uninit; RPC_E_CHANGED_MODE must not.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitializedByUs = SUCCEEDED(comHr);

    const HRESULT mfHr = MFStartup(MF_VERSION);
    m_mfStarted = SUCCEEDED(mfHr);
}

WmfVideoWriter::~WmfVideoWriter() {
    close();
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
    if (m_comInitializedByUs) {
        CoUninitialize();
        m_comInitializedByUs = false;
    }
}

bool WmfVideoWriter::open(const QString& filePath, int width, int height, int fps) {
    m_width = width;
    m_height = height;
    m_fps = fps > 0 ? fps : 25;
    m_videoStreamIndex = kInvalidStreamIndex;
    m_audioStreamIndex = kInvalidStreamIndex;

    if (!m_mfStarted) {
        qWarning() << "[WmfVideoWriter] MFStartup failed; cannot record.";
        return false;
    }

    std::wstring nativePath = QDir::toNativeSeparators(filePath).toStdWString();

    HRESULT hr = MFCreateSinkWriterFromURL(nativePath.c_str(), nullptr, nullptr, &m_sinkWriter);
    if (FAILED(hr)) {
        qWarning() << "[WmfVideoWriter] MFCreateSinkWriterFromURL failed:" << Qt::hex << static_cast<quint32>(hr);
        return false;
    }

    // Every HRESULT below is checked. An unchecked AddStream failure used to
    // leave both stream indices at 0, which routed RGB video samples into the
    // audio stream, and an unchecked MFCreateMediaType left a null pointer that
    // was dereferenced on the very next line.
    const auto fail = [this](const char* what, HRESULT result) {
        qWarning() << "[WmfVideoWriter]" << what << "failed:" << Qt::hex << static_cast<quint32>(result);
        close();
        return false;
    };

    // 1. Configure Video stream (H.264)
    ComScoped<IMFMediaType> videoOutType;
    hr = MFCreateMediaType(&videoOutType);
    if (FAILED(hr)) {
        return fail("MFCreateMediaType(videoOut)", hr);
    }
    hr = videoOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = videoOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(videoOutType.get(), MF_MT_FRAME_SIZE, width, height);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(videoOutType.get(), MF_MT_FRAME_RATE, m_fps, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = videoOutType->SetUINT32(MF_MT_AVG_BITRATE, 2000000); // 2 Mbps
    }
    if (SUCCEEDED(hr)) {
        hr = videoOutType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (FAILED(hr)) {
        return fail("video output media type setup", hr);
    }
    hr = m_sinkWriter->AddStream(videoOutType.get(), &m_videoStreamIndex);
    if (FAILED(hr)) {
        m_videoStreamIndex = kInvalidStreamIndex;
        return fail("AddStream(video)", hr);
    }

    ComScoped<IMFMediaType> videoInType;
    hr = MFCreateMediaType(&videoInType);
    if (FAILED(hr)) {
        return fail("MFCreateMediaType(videoIn)", hr);
    }
    hr = videoInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        // Accept QImage's raw ARGB32/RGB32 data.
        hr = videoInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(videoInType.get(), MF_MT_FRAME_SIZE, width, height);
    }
    if (SUCCEEDED(hr)) {
        hr = videoInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr)) {
        // Without an explicit positive stride, MF derives a NEGATIVE stride for
        // RGB32 (the bottom-up DIB convention) and the recording comes out
        // vertically mirrored, because we copy scanlines top-down.
        hr = videoInType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
    }
    if (FAILED(hr)) {
        return fail("video input media type setup", hr);
    }
    hr = m_sinkWriter->SetInputMediaType(m_videoStreamIndex, videoInType.get(), nullptr);
    if (FAILED(hr)) {
        return fail("SetInputMediaType(video)", hr);
    }

    // 2. Configure Audio stream (AAC)
    ComScoped<IMFMediaType> audioOutType;
    hr = MFCreateMediaType(&audioOutType);
    if (FAILED(hr)) {
        return fail("MFCreateMediaType(audioOut)", hr);
    }
    hr = audioOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) {
        hr = audioOutType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    }
    if (SUCCEEDED(hr)) {
        hr = audioOutType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = audioOutType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000); // Standard AAC rate
    }
    if (SUCCEEDED(hr)) {
        hr = audioOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        hr = audioOutType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000); // 96 kbps
    }
    if (FAILED(hr)) {
        return fail("audio output media type setup", hr);
    }
    hr = m_sinkWriter->AddStream(audioOutType.get(), &m_audioStreamIndex);
    if (FAILED(hr)) {
        m_audioStreamIndex = kInvalidStreamIndex;
        return fail("AddStream(audio)", hr);
    }

    ComScoped<IMFMediaType> audioInType;
    hr = MFCreateMediaType(&audioInType);
    if (FAILED(hr)) {
        return fail("MFCreateMediaType(audioIn)", hr);
    }
    hr = audioInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    }
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000); // Upsampled rate
    }
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    }
    if (SUCCEEDED(hr)) {
        hr = audioInType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
    }
    if (FAILED(hr)) {
        return fail("audio input media type setup", hr);
    }
    hr = m_sinkWriter->SetInputMediaType(m_audioStreamIndex, audioInType.get(), nullptr);
    if (FAILED(hr)) {
        return fail("SetInputMediaType(audio)", hr);
    }

    hr = m_sinkWriter->BeginWriting();
    if (FAILED(hr)) {
        return fail("BeginWriting", hr);
    }

    m_initialized = true;
    return true;
}

void WmfVideoWriter::close() {
    if (m_sinkWriter) {
        if (m_initialized) {
            m_sinkWriter->Finalize();
        }
        m_sinkWriter->Release();
        m_sinkWriter = nullptr;
    }
    m_videoStreamIndex = kInvalidStreamIndex;
    m_audioStreamIndex = kInvalidStreamIndex;
    m_initialized = false;
}

bool WmfVideoWriter::writeVideoFrame(const QImage& frame, qint64 ptsUs) {
    if (!m_initialized || !m_sinkWriter || m_videoStreamIndex == kInvalidStreamIndex) {
        return false;
    }
    if (frame.width() < m_width || frame.height() < m_height) {
        qWarning() << "[WmfVideoWriter] frame smaller than configured size; dropping";
        return true;
    }

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    const int numBytes = m_width * m_height * 4;
    BYTE* dstData = nullptr;

    HRESULT hr = MFCreateMemoryBuffer(numBytes, &buffer);
    if (FAILED(hr)) {
        return false;
    }

    hr = buffer->Lock(&dstData, nullptr, nullptr);
    if (FAILED(hr)) {
        // Writing an unlocked/uninitialised buffer would emit garbage frames.
        buffer->Release();
        return false;
    }
    for (int y = 0; y < m_height; ++y) {
        std::memcpy(dstData + (static_cast<size_t>(y) * m_width * 4), frame.constScanLine(y), m_width * 4);
    }
    buffer->Unlock();
    hr = buffer->SetCurrentLength(numBytes);
    if (FAILED(hr)) {
        buffer->Release();
        return false;
    }

    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(ptsUs * 10); // MF units are 100ns intervals
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration((1000000 / m_fps) * 10);
        if (SUCCEEDED(hr)) hr = m_sinkWriter->WriteSample(m_videoStreamIndex, sample);
        sample->Release();
    }
    buffer->Release();

    return SUCCEEDED(hr);
}

bool WmfVideoWriter::writeAudioSamples(const QByteArray& pcmData, qint64 ptsUs) {
    if (!m_initialized || !m_sinkWriter || m_audioStreamIndex == kInvalidStreamIndex) {
        return false;
    }

    // Linear Resampler: Simple on-the-fly upsampling from 24 kHz to 48 kHz
    const int numInSamples = pcmData.size() / 4; // 2 channels * 2 bytes = 4 bytes per frame
    if (numInSamples <= 0) {
        return true;
    }
    QByteArray upsampled(numInSamples * 2 * 4, Qt::Uninitialized);
    const int16_t* src = reinterpret_cast<const int16_t*>(pcmData.constData());
    int16_t* dst = reinterpret_cast<int16_t*>(upsampled.data());

    for (int i = 0; i < numInSamples; ++i) {
        // Zero-order hold: each input frame becomes two output frames.
        dst[2 * i * 2]     = src[2 * i];       // Left
        dst[2 * i * 2 + 1] = src[2 * i + 1];   // Right
        dst[(2 * i + 1) * 2]     = src[2 * i]; // Left duplicate
        dst[(2 * i + 1) * 2 + 1] = src[2 * i + 1]; // Right duplicate
    }

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    BYTE* data = nullptr;

    HRESULT hr = MFCreateMemoryBuffer(upsampled.size(), &buffer);
    if (FAILED(hr)) {
        return false;
    }

    hr = buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(hr)) {
        buffer->Release();
        return false;
    }
    std::memcpy(data, upsampled.constData(), upsampled.size());
    buffer->Unlock();
    hr = buffer->SetCurrentLength(upsampled.size());
    if (FAILED(hr)) {
        buffer->Release();
        return false;
    }

    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(ptsUs * 10);
        // 48 kHz stereo int16 → 4 bytes per frame; duration in 100 ns units.
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration((static_cast<LONGLONG>(numInSamples) * 2 * 10000000) / 48000);
        if (SUCCEEDED(hr)) hr = m_sinkWriter->WriteSample(m_audioStreamIndex, sample);
        sample->Release();
    }
    buffer->Release();

    return SUCCEEDED(hr);
}

} // namespace AetherSDR
