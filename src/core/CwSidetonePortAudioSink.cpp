#include "CwSidetonePortAudioSink.h"
#include "CwSidetoneDeviceMatch.h"
#include "CwSidetoneGenerator.h"
#include "LogManager.h"

#include <portaudio.h>
#if defined(Q_OS_LINUX) && __has_include(<pa_jack.h>)
#  include <pa_jack.h>  // PaJack_SetClientName — name the PipeWire/JACK node
#endif
#ifdef Q_OS_WIN
#  include <pa_win_wasapi.h>   // PaWasapi_GetIMMDevice — endpoint-identity match
#  include <mmdeviceapi.h>
#  include <combaseapi.h>
#endif

#include <QString>

#include <cstring>

namespace AetherSDR {

namespace {

#ifdef Q_OS_WIN
// The WASAPI endpoint ID string for a PortAudio device, empty when the
// device is not WASAPI (or anything fails). Windows friendly names are NOT
// unique — an NVIDIA HDMI card exposes several identically-named outputs,
// one per connector, so name matching can land on a live-but-unwired port
// that accepts a stream and plays it into nothing (#5200). The endpoint ID
// is the identity Qt's QAudioDevice::id() carries, so comparing IDs pins
// the exact endpoint the user selected.
QString wasapiEndpointId(PaDeviceIndex idx)
{
    void* raw = nullptr;
    if (PaWasapi_GetIMMDevice(idx, &raw) != paNoError || !raw)
        return {};
    auto* dev = static_cast<IMMDevice*>(raw);   // borrowed — do not Release
    LPWSTR id = nullptr;
    QString out;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        out = QString::fromWCharArray(id);
        CoTaskMemFree(id);
    }
    return out;
}
#endif

// Resolve the operator's explicit Qt output selection to a PortAudio device.
// The name rule lives in CwSidetoneDeviceMatch.h so it is testable without
// hardware.  `partialMatchName` (optional) receives the PortAudio name when
// the result came from a PARTIAL match rather than an exact one, so the
// caller can report the substitution in the sidetone summary instead of it
// living in one warning line (#5123).
PaDeviceIndex findPortAudioOutputDevice(const QAudioDevice& device,
                                        QString* partialMatchName = nullptr)
{
    if (device.description().trimmed().isEmpty())
        return paNoDevice;

    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_GetDeviceCount failed —"
                           << Pa_GetErrorText(count);
        return paNoDevice;
    }

#ifdef Q_OS_WIN
    // Identity first, names second: find the WASAPI device whose endpoint ID
    // equals the Qt device's id. Friendly names are non-unique on Windows
    // (multi-connector HDMI), so this is the only selection that provably
    // lands on the endpoint the user picked (#5200).
    const QString qtId = QString::fromUtf8(device.id()).toCaseFolded();
    if (!qtId.isEmpty()) {
        for (PaDeviceIndex i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (!info || info->maxOutputChannels <= 0)
                continue;
            const QString endpointId = wasapiEndpointId(i).toCaseFolded();
            if (!endpointId.isEmpty() && endpointId == qtId) {
                qCInfo(lcAudio) << "CwSidetonePortAudioSink: selected Qt output"
                                << device.description()
                                << "matched WASAPI endpoint by ID"
                                << endpointId;
                return i;
            }
        }
        qCInfo(lcAudio) << "CwSidetonePortAudioSink: no WASAPI endpoint-ID match for"
                        << device.description() << "id=" << qtId
                        << "- falling back to name matching";
    }
#endif

    // Collect all partial-match candidates. On Windows a single physical
    // device appears under multiple host APIs (MME, DirectSound, WASAPI);
    // the candidate list lets us prefer WASAPI instead of giving up when
    // more than one partial match is found. (#3193)
    struct Candidate { PaDeviceIndex idx; QString rawName; PaHostApiTypeId apiType; };
    QList<Candidate> partials;
    QList<Candidate> exacts;

    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name)
            continue;

        const QString rawName = QString::fromUtf8(info->name);
        const DeviceNameMatch kind = classifyDeviceNameMatch(rawName, device.description());
        if (kind == DeviceNameMatch::None)
            continue;

        // paInDevelopment (0) is used as a safe "unknown" sentinel when
        // Pa_GetHostApiInfo returns null — it will never equal paWASAPI.
        PaHostApiTypeId apiType = paInDevelopment;
        if (const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi))
            apiType = api->type;

        // Do NOT return on the first exact match: on Windows the same
        // endpoint enumerates under several host APIs with the identical
        // friendly name, and enumeration order puts DirectSound before
        // WASAPI — returning early hands the sidetone to DirectSound,
        // which mangles small-buffer callback audio into garbage (#5200).
        // Collect all exacts and resolve by host-API preference below.
        if (kind == DeviceNameMatch::Exact)
            exacts.append({i, rawName, apiType});
        else
            partials.append({i, rawName, apiType});
    }

    if (!exacts.isEmpty()) {
#ifdef Q_OS_WIN
        // Prefer WASAPI (~10 ms shared-mode) over MME/DirectSound
        // (50–150 ms, and DS garbles the tiny-buffer stream this sink
        // opens). Same preference #3193 applies to partial matches. (#5200)
        for (const Candidate& c : exacts) {
            if (c.apiType == paWASAPI) {
                qCInfo(lcAudio) << "CwSidetonePortAudioSink: exact match for"
                                << device.description()
                                << "resolved to WASAPI output"
                                << c.rawName
                                << "(preferred over" << exacts.size() - 1
                                << "other exact host-API match(es))";
                return c.idx;
            }
        }
#endif
        return exacts[0].idx;
    }

    if (partials.isEmpty()) {
        // Name every output-capable candidate so field reports show what was
        // available to match, not just that nothing did (#4978). Failure-path
        // only — the second enumeration costs nothing on a successful match.
        QStringList candidates;
        for (PaDeviceIndex i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (!info || info->maxOutputChannels <= 0 || !info->name)
                continue;
            const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
            candidates << QStringLiteral("\"%1\" [%2]")
                              .arg(QString::fromUtf8(info->name),
                                   api && api->name ? QString::fromUtf8(api->name)
                                                    : QStringLiteral("?"));
        }
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: no PortAudio output matches"
                           << device.description()
                           << "- candidates:" << qUtf8Printable(candidates.join(QStringLiteral(", ")));
        return paNoDevice;
    }

    if (partials.size() == 1) {
        if (partialMatchName)
            *partialMatchName = partials[0].rawName;
        qCWarning(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                           << device.description()
                           << "only partially matched PortAudio output"
                           << partials[0].rawName;
        return partials[0].idx;
    }

#ifdef Q_OS_WIN
    // Multiple matches — the same physical device enumerated under different
    // host APIs. Prefer WASAPI (~10 ms shared-mode latency) over MME or
    // DirectSound (50–150 ms) to reduce CW timing jitter. (#3193)
    QList<Candidate> wasapiCandidates;
    for (const Candidate& c : partials) {
        if (c.apiType == paWASAPI)
            wasapiCandidates.append(c);
    }
    if (wasapiCandidates.size() == 1) {
        // Still a PARTIAL match: the host-API preference picked which of
        // several partial rows to open, not whether the name matched. The
        // operator did not choose this device, so it must be reported as a
        // substitution exactly like the single-partial path above (#5123) —
        // otherwise the summary and support bundle call it a clean match.
        if (partialMatchName)
            *partialMatchName = wasapiCandidates[0].rawName;
        qCInfo(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                        << device.description()
                        << "resolved to WASAPI output"
                        << wasapiCandidates[0].rawName
                        << "(preferred over" << partials.size() - 1 << "other host API(s))";
        return wasapiCandidates[0].idx;
    }
#endif

    QStringList matchedNames;
    for (const Candidate& c : partials)
        matchedNames << QStringLiteral("\"%1\"").arg(c.rawName);
    qCWarning(lcAudio) << "CwSidetonePortAudioSink: selected Qt output device"
                       << device.description()
                       << "matched multiple PortAudio outputs:"
                       << qUtf8Printable(matchedNames.join(QStringLiteral(", ")));
    return paNoDevice;
}

PaDeviceIndex defaultPortAudioOutputDevice()
{
    // No JACK preference here: a default selection must land on the same
    // output the rest of the app's audio uses (Pa_GetDefaultOutputDevice —
    // the ALSA `default` route on Linux, which follows the system mixer).
    // Preferring a reachable JACK server's device would silently split the
    // sidetone from RX audio; routing INTO a JACK graph should be an
    // explicit selection (see #4978's escape-hatch follow-up), not a
    // side effect of leaving the device unset.
    PaDeviceIndex devIdx = paNoDevice;
#ifdef Q_OS_WIN
    // Pa_GetDefaultOutputDevice() on Windows typically returns an MME device
    // (the first enumerated host API), which has 50–150 ms OS-level buffering.
    // Prefer WASAPI shared mode (~10 ms) to reduce CW timing jitter on fast
    // keying. (#3193)
    const PaHostApiIndex apiCount = Pa_GetHostApiCount();
    for (PaHostApiIndex i = 0; i < apiCount; ++i) {
        const PaHostApiInfo* api = Pa_GetHostApiInfo(i);
        if (!api || !api->name) continue;
        if (qstrncmp(api->name, "Windows WASAPI", 14) == 0
            && api->defaultOutputDevice != paNoDevice) {
            devIdx = api->defaultOutputDevice;
            qCInfo(lcAudio) << "CwSidetonePortAudioSink: using WASAPI host API"
                            << "(device" << devIdx << ")";
            break;
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
#if defined(Q_OS_LINUX) && __has_include(<pa_jack.h>)
        // Name the PortAudio->JACK/PipeWire client so the CW sidetone shows
        // as "AetherSDR CW Sidetone" in qpwgraph/JACK patchbays instead of
        // the bare PortAudio default. Must precede Pa_Initialize, and pa_jack
        // references (does not copy) the string -> static lifetime. CW sidetone
        // is AetherSDR's only PortAudio user, so naming the process-global JACK
        // client here is unambiguous.
        static const char kJackClientName[] = "AetherSDR CW Sidetone";
        // PaJack_SetClientName returns PaError; surface a failure like the
        // Pa_Initialize path below. Non-fatal — the node keeps its default
        // name — so log and continue rather than abort.
        const PaError nameErr = PaJack_SetClientName(kJackClientName);
        if (nameErr != paNoError) {
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: PaJack_SetClientName failed —"
                               << Pa_GetErrorText(nameErr)
                               << "(node keeps its default name)";
        }
#endif
        const PaError err = Pa_Initialize();
        if (err != paNoError) {
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: Pa_Initialize failed —"
                               << Pa_GetErrorText(err);
            return false;
        }
        m_paInitialized = true;
    }

    QString partialMatchName;
    PaDeviceIndex devIdx = device.isNull()
        ? defaultPortAudioOutputDevice()
        : findPortAudioOutputDevice(device, &partialMatchName);
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
        if (partialMatchName.isEmpty()) {
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: matched selected Qt output"
                               << device.description()
                               << "to PortAudio output" << devInfo->name;
        } else {
            // Not "matched": the operator did not pick this device (#5123).
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: opening PortAudio output"
                               << devInfo->name
                               << "in place of selected Qt output"
                               << device.description()
                               << "(partial name match)";
        }
    }
    if (!partialMatchName.isEmpty()) {
        // A partial name match is a substitution the operator did not make;
        // surface it in the summary and the support bundle, not only in the
        // warning above (#5123).
        m_fallbackOccurred = true;
        const QString detail = QStringLiteral("selected \"%1\" resolved by partial name match to \"%2\"")
                                   .arg(device.description(), partialMatchName);
        m_fallbackReason = m_fallbackReason.isEmpty()
            ? detail
            : m_fallbackReason + QStringLiteral("; ") + detail;
    }
    m_deviceDescription = QString::fromLocal8Bit(devInfo->name ? devInfo->name : "");

    // Prefer 48 kHz; fall back to the device's native rate only if the
    // device explicitly rejects 48 kHz.
    PaStreamParameters outParams{};
    outParams.device = devIdx;
    outParams.channelCount = 2;
    outParams.sampleFormat = paFloat32;
    outParams.hostApiSpecificStreamInfo = nullptr;

    double sampleRate = desiredRateHz > 0 ? desiredRateHz : 48000;
#ifdef Q_OS_WIN
    // 0.0 makes DirectSound/MME build a buffer ring far below what they can
    // service — the stream runs but the audio comes out garbled (#5200).
    // Ask for the device's own default-low latency instead; on WASAPI shared
    // mode that is the ~10 ms engine period.
    outParams.suggestedLatency = devInfo->defaultLowOutputLatency;
#else
    outParams.suggestedLatency = 0.0;  // ask for smallest the host can deliver
#endif
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

    m_cbCount.store(0, std::memory_order_relaxed);
    m_cbPeakMicro.store(0, std::memory_order_relaxed);
    m_cbUnderflows.store(0, std::memory_order_relaxed);
    m_cbOverflows.store(0, std::memory_order_relaxed);
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(m_stream);
    const PaHostApiInfo* hostApi = Pa_GetHostApiInfo(devInfo->hostApi);
    qCInfo(lcAudio) << "CwSidetonePortAudioSink: started"
                    << "device=" << devInfo->name
                    << "hostApi=" << (hostApi && hostApi->name ? hostApi->name : "?")
                    << "rate=" << m_actualRate << "Hz"
                    << "outputLatency=" << (streamInfo ? streamInfo->outputLatency * 1000.0 : 0.0)
                    << "ms";
    return true;
}

int CwSidetonePortAudioSink::paCallback(const void* /*input*/,
                                        void* output,
                                        unsigned long frameCount,
                                        const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                        PaStreamCallbackFlags statusFlags,
                                        void* userData)
{
    auto* self = static_cast<CwSidetonePortAudioSink*>(userData);
    auto* dst = static_cast<float*>(output);

    // Count the host's own deadline misses. Cheap (two predictable branches
    // on a value already in a register) and it is the only thing that can
    // tell an underflow apart from wake jitter after the fact: both show up
    // in the envelope as a displaced element and nothing else distinguishes
    // them. Relaxed ordering — these are diagnostics read after the stream
    // stops, never used to make a decision inside the callback.
    if (statusFlags & paOutputUnderflow)
        self->m_cbUnderflows.fetch_add(1, std::memory_order_relaxed);
    if (statusFlags & paOutputOverflow)
        self->m_cbOverflows.fetch_add(1, std::memory_order_relaxed);

    // Always start from silence — PortAudio doesn't guarantee zeroed
    // buffers and the generator mixes additively.
    std::memset(dst, 0, frameCount * 2 * sizeof(float));

    auto* gen = self->m_generator.load(std::memory_order_acquire);
    if (gen) gen->process(dst, static_cast<int>(frameCount));

    self->m_edgeProbe.scan(dst, static_cast<int>(frameCount));
    self->m_cbCount.fetch_add(1, std::memory_order_relaxed);
    float peak = 0.0f;
    for (unsigned long i = 0; i < frameCount * 2; ++i) {
        const float a = dst[i] < 0 ? -dst[i] : dst[i];
        if (a > peak) peak = a;
    }
    const auto peakMicro = static_cast<quint32>(peak * 1e6f);
    quint32 prev = self->m_cbPeakMicro.load(std::memory_order_relaxed);
    while (peakMicro > prev
           && !self->m_cbPeakMicro.compare_exchange_weak(
                  prev, peakMicro, std::memory_order_relaxed)) {}

    return paContinue;
}

void CwSidetonePortAudioSink::stop()
{
    if (m_stream) {
        const quint32 under = m_cbUnderflows.load(std::memory_order_relaxed);
        const quint32 over  = m_cbOverflows.load(std::memory_order_relaxed);
        qCInfo(lcAudio) << "CwSidetonePortAudioSink: stopping —"
                        << "callbacks=" << m_cbCount.load(std::memory_order_relaxed)
                        << "peak=" << (m_cbPeakMicro.load(std::memory_order_relaxed) / 1e6)
                        << "underflows=" << under
                        << "overflows=" << over;
        // Warn separately rather than only in the summary line: an underflow
        // is an audible gap in the sidetone, and a run that produced any is
        // not a clean timing measurement.
        if (under > 0 || over > 0)
            qCWarning(lcAudio) << "CwSidetonePortAudioSink: stream reported"
                               << under << "output underflow(s) and"
                               << over << "overflow(s) — element timing from"
                               << "this run is not clean";
        // Halt the callback before clearing the generator pointer so we
        // don't race with paCallback dereferencing a torn-down generator —
        // and before dumping the edge probe, which resets the same members
        // (m_count, m_samplePos, m_tone, m_quietRun) that scan() writes from
        // inside the callback.  The probe belongs on this side of the barrier
        // for exactly the reason the generator pointer does.
        Pa_StopStream(m_stream);
        m_edgeProbe.dump("PortAudio", m_actualRate);
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
