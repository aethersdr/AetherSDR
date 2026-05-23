#include "CwSidetonePortAudioSink.h"
#include "CwSidetoneGenerator.h"
#include "LogManager.h"

#include <portaudio.h>

#include <QString>

#include <cstring>

namespace AetherSDR {

namespace {

QString normalizedDeviceName(QString name)
{
    return name.simplified().toCaseFolded();
}

PaDeviceIndex findPortAudioOutputDevice(const QAudioDevice& device)
{
    const QString target = normalizedDeviceName(device.description());
    if (target.isEmpty())
        return paNoDevice;

    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_GetDeviceCount failed —"
                           << Pa_GetErrorText(count);
        return paNoDevice;
    }

    PaDeviceIndex partialMatch = paNoDevice;
    QString partialMatchName;
    bool partialMatchAmbiguous = false;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name)
            continue;

        const QString rawName = QString::fromUtf8(info->name);
        const QString candidate =
            normalizedDeviceName(rawName);
        if (candidate == target)
            return i;

        if (candidate.contains(target) || target.contains(candidate)) {
            if (partialMatch == paNoDevice) {
                partialMatch = i;
                partialMatchName = rawName;
            } else {
                partialMatchAmbiguous = true;
            }
        }
    }

    if (partialMatchAmbiguous) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                           << device.description()
                           << "matched multiple PortAudio outputs";
        return paNoDevice;
    }

    if (partialMatch != paNoDevice) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                           << device.description()
                           << "partially matched PortAudio output"
                           << partialMatchName
                           << "by name substring";
    }
    return partialMatch;
}

PaDeviceIndex defaultPortAudioOutputDevice()
{
    PaDeviceIndex devIdx = paNoDevice;
#ifdef Q_OS_LINUX
    {
        const PaHostApiIndex apiCount = Pa_GetHostApiCount();
        for (PaHostApiIndex i = 0; i < apiCount; ++i) {
            const PaHostApiInfo* api = Pa_GetHostApiInfo(i);
            if (!api || !api->name) continue;
            if (qstrncmp(api->name, "JACK", 4) == 0
                && api->defaultOutputDevice != paNoDevice) {
                devIdx = api->defaultOutputDevice;
                qCInfo(lcAudio) << "CwSidetonePortAudioSink: using JACK host API"
                                << "(device" << devIdx << ")";
                break;
            }
        }
    }
#endif
    if (devIdx == paNoDevice)
        devIdx = Pa_GetDefaultOutputDevice();
    return devIdx;
}

} // namespace

CwSidetonePortAudioSink::CwSidetonePortAudioSink() = default;

CwSidetonePortAudioSink::~CwSidetonePortAudioSink()
{
    stop();
    if (m_paInitialized) {
        Pa_Terminate();
        m_paInitialized = false;
    }
}

bool CwSidetonePortAudioSink::start(const QAudioDevice& device,
                                    int desiredRateHz,
                                    CwSidetoneGenerator* generator)
{
    if (m_stream) return true;
    if (!generator) return false;
    m_deviceDescription.clear();
    m_fallbackOccurred = false;
    m_fallbackReason.clear();

    if (!m_paInitialized) {
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_Initialize failed —"
                               << Pa_GetErrorText(err);
            return false;
        }
        m_paInitialized = true;
    }

    PaDeviceIndex devIdx = device.isNull()
        ? defaultPortAudioOutputDevice()
        : findPortAudioOutputDevice(device);
    if (!device.isNull() && devIdx == paNoDevice) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                           << device.description()
                           << "was not found in PortAudio; falling back to QAudioSink";
        return false;
    }
    if (devIdx == paNoDevice) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: no default output device";
        return false;
    }

    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(devIdx);
    if (!devInfo) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_GetDeviceInfo returned null";
        return false;
    }
    if (!device.isNull()) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: matched selected Qt output"
                           << device.description()
                           << "to PortAudio output" << devInfo->name;
    }
    m_deviceDescription = QString::fromLocal8Bit(devInfo->name ? devInfo->name : "");

    // Detect JACK-host-API selection from defaultPortAudioOutputDevice() so the
    // summary logger sees it as a backend-substituted fallback. (The selection
    // itself happens inside the namespace-scope helper, which can't touch
    // member state directly.)
    if (device.isNull()) {
        const PaHostApiInfo* api = Pa_GetHostApiInfo(devInfo->hostApi);
        if (api && api->name && qstrncmp(api->name, "JACK", 4) == 0) {
            m_fallbackOccurred = true;
            m_fallbackReason = QStringLiteral("backend selected JACK default output");
        }
    }

    // Prefer 48 kHz; fall back to the device's native rate only if the
    // device explicitly rejects 48 kHz.
    PaStreamParameters outParams{};
    outParams.device = devIdx;
    outParams.channelCount = 2;
    outParams.sampleFormat = paFloat32;
    outParams.hostApiSpecificStreamInfo = nullptr;

    double sampleRate = desiredRateHz > 0 ? desiredRateHz : 48000;
    outParams.suggestedLatency = 0.0;  // ask for smallest the host can deliver
    if (Pa_IsFormatSupported(nullptr, &outParams, sampleRate) != paFormatIsSupported) {
        sampleRate = devInfo->defaultSampleRate > 0
            ? devInfo->defaultSampleRate
            : 48000;
        m_fallbackOccurred = true;
        const QString detail = QStringLiteral("48000Hz unsupported -> %1Hz")
            .arg(static_cast<int>(sampleRate));
        m_fallbackReason = m_fallbackReason.isEmpty()
            ? detail
            : m_fallbackReason + QStringLiteral("; ") + detail;
        qCInfo(lcAudio) << "CwSidetonePortAudioSink: 48000 unsupported, using"
                        << sampleRate;
    }

    // Push for sub-5 ms total latency.  On JACK / PipeWire the actual
    // value is bounded by the server quantum — passing 0 + a small
    // framesPerBuffer asks the host for the smallest it can deliver per
    // client, which PipeWire honours as a per-stream latency request.
    constexpr unsigned long kFramesPerBuffer = 128;

    // Store generator BEFORE opening so the very first callback (which
    // can fire before Pa_OpenStream returns on some platforms) sees it.
    m_generator.store(generator, std::memory_order_release);
    generator->setSampleRateHz(static_cast<int>(sampleRate));

    PaError err = Pa_OpenStream(&m_stream,
                                /*input*/  nullptr,
                                /*output*/ &outParams,
                                sampleRate,
                                kFramesPerBuffer,
                                paNoFlag,
                                &CwSidetonePortAudioSink::paCallback,
                                this);
    if (err != paNoError) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_OpenStream failed —"
                           << Pa_GetErrorText(err);
        m_generator.store(nullptr, std::memory_order_release);
        return false;
    }

    err = Pa_StartStream(m_stream);
    if (err != paNoError) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_StartStream failed —"
                           << Pa_GetErrorText(err);
        Pa_CloseStream(m_stream);
        m_stream = nullptr;
        m_generator.store(nullptr, std::memory_order_release);
        return false;
    }

    m_actualRate = static_cast<int>(sampleRate);

    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(m_stream);
    qCInfo(lcAudio) << "CwSidetonePortAudioSink: started"
                    << "device=" << devInfo->name
                    << "rate=" << m_actualRate << "Hz"
                    << "outputLatency=" << (streamInfo ? streamInfo->outputLatency * 1000.0 : 0.0)
                    << "ms";
    return true;
}

int CwSidetonePortAudioSink::paCallback(const void* /*input*/,
                                        void* output,
                                        unsigned long frameCount,
                                        const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                        PaStreamCallbackFlags /*statusFlags*/,
                                        void* userData)
{
    auto* self = static_cast<CwSidetonePortAudioSink*>(userData);
    auto* dst = static_cast<float*>(output);

    // Always start from silence — PortAudio doesn't guarantee zeroed
    // buffers and the generator mixes additively.
    std::memset(dst, 0, frameCount * 2 * sizeof(float));

    auto* gen = self->m_generator.load(std::memory_order_acquire);
    if (gen) gen->process(dst, static_cast<int>(frameCount));

    return paContinue;
}

void CwSidetonePortAudioSink::stop()
{
    if (m_stream) {
        // Halt the callback before clearing the generator pointer so we
        // don't race with paCallback dereferencing a torn-down generator.
        Pa_StopStream(m_stream);
        m_generator.store(nullptr, std::memory_order_release);
        Pa_CloseStream(m_stream);
        m_stream = nullptr;
    } else {
        m_generator.store(nullptr, std::memory_order_release);
    }
    m_actualRate = 0;
    m_deviceDescription.clear();
    m_fallbackOccurred = false;
    m_fallbackReason.clear();
}

} // namespace AetherSDR
