#include "core/backends/colibri/ColibriBackend.h"

#include <QJsonObject>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

#include "core/backends/colibri/ColibriDevice.h"
#include "core/backends/colibri/ColibriLib.h"
#include "core/backends/colibri/ColibriRxDsp.h"
#include "core/backends/colibri/ColibriSettings.h"

Q_LOGGING_CATEGORY(lcColibri, "aether.colibri")

namespace AetherSDR::colibri {

namespace {

// Snap a requested span (Hz) to the nearest deliverable rate, in the LOG
// domain — the rates are octave-ish spaced and zoom is a multiplicative
// gesture, so ratio-nearest is what makes a wheel step land on the
// neighbouring rate instead of skipping one (same reasoning as HL2).
int nearestSampleRateHz(double requestedHz) noexcept
{
    if (!(requestedHz > 0.0))
        return kColibriSampleRatesHz[0];
    int best = kColibriSampleRatesHz[0];
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int rate : kColibriSampleRatesHz) {
        const double distance =
            std::abs(std::log(requestedHz / static_cast<double>(rate)));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = rate;
        }
    }
    return best;
}

// Neutral AGC vocabulary -> WDSP RXA AGC mode (same table as the HL2 chain).
int wdspAgcMode(const QString& mode) noexcept
{
    const QString m = mode.trimmed().toLower();
    if (m == QLatin1String("off"))  return 0;
    if (m == QLatin1String("slow")) return 2;
    if (m == QLatin1String("fast")) return 4;
    return 3;                                  // medium: WDSP's own default
}

WdspChannel::Mode modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    if (u == QLatin1String("CWL"))  return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM"))
        return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM"))
        return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;
}

bool isKnownModeString(const QString& mode) noexcept
{
    static const QStringList kKnown = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("DSB"),
        QStringLiteral("CWL"), QStringLiteral("CWU"), QStringLiteral("CW"),
        QStringLiteral("FM"),  QStringLiteral("NFM"), QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("SAM"),
        QStringLiteral("DRM"), QStringLiteral("WBFM"), QStringLiteral("WFM"),
    };
    return kKnown.contains(mode.toUpper());
}

// Default RX passband per mode (Hz, sign carries the sideband — SliceModel's
// convention). Same table as the HL2 backend, for the same reasons.
std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return {350, 850};
    if (u == QLatin1String("CWL"))  return {-850, -350};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

// Phase-1 data-plane payload: a raw little-endian float32 array.
QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

ColibriBackend::ColibriBackend(QObject* parent) : IRadioBackend(parent)
{
    // No parent: moveToThread() refuses an object that has one. Destroyed
    // explicitly in the destructor after the thread is joined.
    m_device = new ColibriDevice(nullptr);

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("colibri-io"));
    m_device->moveToThread(m_ioThread);
    m_ioThread->start();

    // Raw IQ -> the DSP chain. The signal is emitted from the LIBRARY's
    // thread; the receiver context is m_device (I/O thread), so this is a
    // QUEUED delivery and the lambda body runs on the I/O thread — where
    // m_ioDsp lives. Never DirectConnection here: that would run WDSP on the
    // library's own thread, unsynchronized against every control verb.
    connect(m_device, &ColibriDevice::iqBlockReady, m_device,
            [this](const std::vector<std::complex<float>>& iq) {
        if (m_ioDsp)
            m_ioDsp->processIqBlock(iq);
    }, Qt::QueuedConnection);

    connect(m_device, &ColibriDevice::opened, this, [this] {
        m_connected = true;
        m_linkBlocksAtLastTick = 0;
        m_linkStatsTimer->start();
        emit connected();
        // Publish initial pan/slice state AFTER connected() — RadioModel's
        // onConnected() stages existing models as previous-session leftovers,
        // so anything emitted earlier is wiped before the UI sees it. Pan
        // FIRST: the pan must exist before anything (zoom limits, slice)
        // describes it, or those reports are dropped (#4470 shape).
        emitPanState();
        emit panBandwidthLimitsChanged(
            panId(),
            static_cast<double>(kColibriSampleRatesHz[0]) / 1.0e6,
            static_cast<double>(
                kColibriSampleRatesHz[std::size(kColibriSampleRatesHz) - 1]) / 1.0e6);
        emit panRfGainInfoChanged(panId(), kPreampMinDb, kPreampMaxDb, kPreampStepDb);
        emit panRfGainChanged(panId(), static_cast<int>(std::lround(m_preampDb)));
        pushInitialState();
        emitSliceState();
        defineMeters();
    });
    connect(m_device, &ColibriDevice::openFailed, this, [this](const QString& reason) {
        m_connected = false;
        m_linkStatsTimer->stop();
        emit connectionError(QStringLiteral("ColibriNANO: %1").arg(reason));
    });
    connect(m_device, &ColibriDevice::adcOverloadChanged, this, [this](bool overload) {
        // Surfaced as vendor status rather than a core field: nothing in the
        // core profile models converter overload yet.
        emit extensionStatus(familyName(), QStringLiteral("adcOverload"),
                             {{QStringLiteral("overload"), overload}});
        if (overload)
            qCWarning(lcColibri) << "ADC overload — reduce preamp gain";
    });

    m_linkStatsTimer = new QTimer(this);
    m_linkStatsTimer->setInterval(kLinkStatsIntervalMs);
    connect(m_linkStatsTimer, &QTimer::timeout, this,
            &ColibriBackend::publishLinkStats);
}

ColibriBackend::~ColibriBackend()
{
    if (m_ioThread) {
        // Stop the device ON its own thread and WAIT for it: quit() below
        // ends the event loop that a queued stop would need, and tearing the
        // descriptor down from this thread is an affinity bug.
        QMetaObject::invokeMethod(m_device, &ColibriDevice::closeDevice,
                                  Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    }
    // The thread is joined; nothing can be running in either of these.
    delete m_rxDsp;
    m_rxDsp = nullptr;
    m_ioDsp = nullptr;
    delete m_device;
}

RadioCapabilities ColibriBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = familyName();
    c.model = QStringLiteral("ColibriNANO");
    c.maxSlices = 1;          // one DDC in the hardware; not a policy choice
    c.maxPanadapters = 1;
    for (const int rate : kColibriSampleRatesHz)
        c.sampleRatesHz.append(rate);
    c.tuningMinHz = kTuningMinHz;
    c.tuningMaxHz = kTuningMaxHz;
    // RECEIVER. Constant false rather than a gate: there is no transmitter to
    // gate, and the engine TX guard refuses keying on this alone.
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    // RX-only must NOT open the mic on connect (#4449 review).
    c.hostModulates = false;
    c.persistsMemories = false;   // client-side memory bank engages
    c.canReboot = false;
    c.hasTuner = false;
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
    c.hasProfiles = false;        // no on-device configuration store
    c.hasDaxStreams = false;      // one raw IQ feed, demodulated here
    c.hasRadioSideDsp = false;    // every noise module runs on this host
    c.hasRadioSideWaterfallAutoBlack = false;
    c.hasWaveforms = false;
    c.hasMultiClientSessions = false;   // the DLL owns the device exclusively
    c.hasGpsLocation = false;
    c.hasSupplyVoltageTelemetry = false;
    // The device persists NOTHING across unplugs — the client is its memory
    // (RFC #4603). No TxSetpoints: there is no transmitter to have setpoints.
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::Tuning
                            | RadioCapabilities::ClientSettingsDomain::Passband
                            | RadioCapabilities::ClientSettingsDomain::SpanRate
                            | RadioCapabilities::ClientSettingsDomain::RfGain
                            | RadioCapabilities::ClientSettingsDomain::Memories;
    return c;
}

void ColibriBackend::applyRestoredState(const RestoredRadioState& state)
{
    // Validated at this boundary (Principle VII): a corrupt document must not
    // reach the UI and re-persist itself through capture.
    m_restoredState = RestoredRadioState{};
    m_haveRestoredState = !state.isEmpty();
    if (!m_haveRestoredState)
        return;

    RestoredRadioState& s = m_restoredState;
    if (state.rfFrequencyHz >= kTuningMinHz && state.rfFrequencyHz <= kTuningMaxHz)
        s.rfFrequencyHz = state.rfFrequencyHz;
    if (isKnownModeString(state.mode))
        s.mode = state.mode;
    // The passband pair is only meaningful together and bounded by what the
    // audio chain can pass.
    if (state.filterLowHz != 0.0 && state.filterHighHz != 0.0
        && state.filterLowHz < state.filterHighHz
        && std::abs(state.filterLowHz) <= 45000.0
        && std::abs(state.filterHighHz) <= 45000.0) {
        s.filterLowHz = state.filterLowHz;
        s.filterHighHz = state.filterHighHz;
    }
    if (colibriSampleRateIndex(state.sampleRateHz) >= 0)
        s.sampleRateHz = state.sampleRateHz;
    // rfGain extension: {"preampDb": double} — ours to write, ours to check.
    const QJsonObject rfGain =
        state.extension.value(QStringLiteral("rfGain")).toObject();
    if (rfGain.contains(QStringLiteral("preampDb"))) {
        const double db = rfGain.value(QStringLiteral("preampDb")).toDouble();
        if (db >= kPreampMinDb - 0.5 && db <= kPreampMaxDb) {
            QJsonObject mine;
            mine.insert(QStringLiteral("preampDb"), db);
            s.extension.insert(QStringLiteral("rfGain"), mine);
        }
    }
}

RestoredRadioState ColibriBackend::currentOperatingState() const
{
    RestoredRadioState s;
    s.rfFrequencyHz = m_sliceFreqHz;
    s.mode = m_mode;
    s.filterLowHz = m_filterLowHz;
    s.filterHighHz = m_filterHighHz;
    s.sampleRateHz = m_sampleRateHz;
    QJsonObject rfGain;
    rfGain.insert(QStringLiteral("preampDb"), m_preampDb);
    s.extension.insert(QStringLiteral("rfGain"), rfGain);
    s.extensionSchemaVersion = 1;
    return s;
}

void ColibriBackend::connectRadio(const RadioConnectRequest& request)
{
    m_passbandDerivedThisConnect = false;

    // The span the operator last chose (family-wide fallback), then the
    // per-radio restored rate, then the explicit automation/test param —
    // same precedence ladder as HL2.
    if (const double remembered = ColibriSettings::spanMhz(); remembered > 0.0)
        m_sampleRateHz = nearestSampleRateHz(remembered * 1.0e6);
    if (m_haveRestoredState && m_restoredState.sampleRateHz > 0)
        m_sampleRateHz = m_restoredState.sampleRateHz;
    if (request.params.contains(QStringLiteral("sampleRateHz"))) {
        const int hz = request.params.value(QStringLiteral("sampleRateHz")).toInt();
        if (colibriSampleRateIndex(hz) >= 0)
            m_sampleRateHz = hz;
    }

    if (m_haveRestoredState) {
        if (m_restoredState.rfFrequencyHz > 0.0) {
            m_sliceFreqHz = m_restoredState.rfFrequencyHz;
            m_ncoHz = m_sliceFreqHz;
        }
        if (!m_restoredState.mode.isEmpty())
            m_mode = m_restoredState.mode;
        if (m_restoredState.filterLowHz != 0.0 || m_restoredState.filterHighHz != 0.0) {
            m_filterLowHz = static_cast<int>(m_restoredState.filterLowHz);
            m_filterHighHz = static_cast<int>(m_restoredState.filterHighHz);
            // The restored passband IS this connect's passband; the per-mode
            // derivation must not overwrite it (#4484 shape).
            m_passbandDerivedThisConnect = true;
        }
        const QJsonObject rfGain =
            m_restoredState.extension.value(QStringLiteral("rfGain")).toObject();
        if (rfGain.contains(QStringLiteral("preampDb")))
            m_preampDb = rfGain.value(QStringLiteral("preampDb")).toDouble();
    }
    if (request.params.contains(QStringLiteral("rxFrequencyHz"))) {
        m_sliceFreqHz =
            request.params.value(QStringLiteral("rxFrequencyHz")).toDouble();
        m_ncoHz = m_sliceFreqHz;
    }
    if (request.params.contains(QStringLiteral("preampDb")))
        m_preampDb = request.params.value(QStringLiteral("preampDb")).toDouble();
    m_preampDb = std::clamp(m_preampDb,
                            static_cast<double>(kPreampMinDb),
                            static_cast<double>(kPreampMaxDb));

    // ---- build the DSP BEFORE the stream starts ----
    //
    // Same order rule as HL2: opening a WDSP channel can be slow (first-run
    // FFTW wisdom), and the DSP must already expect the stream's rate before
    // blocks arrive. Configure blocking on the I/O thread, publish to the
    // sample path, THEN open the device.
    if (m_rxDsp) {
        // A reconnect without a teardown: withdraw from the sample path and
        // replace, never reconfigure a chain that might still be fed.
        publishIoDsp(nullptr);
        m_rxDsp->deleteLater();
        m_rxDsp = nullptr;
    }
    auto* dsp = new ColibriRxDsp(nullptr);   // no parent: moveToThread refuses one
    dsp->moveToThread(m_ioThread);

    connect(dsp, &ColibriRxDsp::spectrumReady, this,
            [this](const std::vector<float>& bins) {
        // dBFS -> displayed dBm: one constant offset minus the preamp, so a
        // gain change cannot move the trace (the signals did not move).
        const double off = kFullScaleDbm - m_preampDb;
        std::vector<float> dbm(bins.size());
        for (std::size_t i = 0; i < bins.size(); ++i)
            dbm[i] = static_cast<float>(bins[i] + off);
        // The seam's spectrum feed is keyed by the slice/pan NUMBER (0), the
        // same convention Hl2Backend uses for its first receiver.
        emit spectrumFrameReady(0, floatBytes(dbm));
    });
    connect(dsp, &ColibriRxDsp::audioReady, this,
            [this](const std::vector<float>& pcm) { routeAudio(pcm); });
    connect(dsp, &ColibriRxDsp::meterUpdate, this, [this](float dbfs) {
        const double dbm = dbfsToDbm(dbfs);
        // Smooth EVERY sample, publish only on the tick — the published value
        // then represents the whole interval rather than one instant, and the
        // widget is not repainted ~47 times a second.
        if (!m_haveSMeter) {
            m_sMeterDbm = dbm;
            m_haveSMeter = true;
        } else {
            const double alpha = (dbm > m_sMeterDbm) ? kMeterAttackAlpha
                                                     : kMeterDecayAlpha;
            m_sMeterDbm = alpha * dbm + (1.0 - alpha) * m_sMeterDbm;
        }
        if (m_sMeterClock.isValid()
            && m_sMeterClock.elapsed() < kMeterPublishIntervalMs)
            return;
        m_sMeterClock.restart();
        emit meterUpdate(QStringLiteral("SLC:LEVEL"), m_sMeterDbm);
    });

    ColibriRxDsp::Config dc;
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 24000;   // AudioEngine's native RX rate
    dc.mode = modeFromString(m_mode);
    dc.filterLowHz = m_filterLowHz;
    dc.filterHighHz = m_filterHighHz;
    dc.agcMode = wdspAgcMode(m_agcMode);
    dc.maximumAgcGainDb = m_agcThresholdDb * kAgcCeilingDbPerUnit;
    dc.wireAnalytic = ColibriSettings::wireAnalytic();

    std::string err;
    bool ok = false;
    QMetaObject::invokeMethod(dsp, [dsp, &dc, &err, &ok] {
        ok = dsp->configure(dc, &err);
    }, Qt::BlockingQueuedConnection);
    if (!ok) {
        dsp->deleteLater();
        emit connectionError(QStringLiteral("ColibriNANO: DSP chain failed — %1")
                                 .arg(QString::fromStdString(err)));
        return;
    }
    m_rxDsp = dsp;
    publishIoDsp(dsp);

    // The slice starts wherever the NCO starts; no shift until the operator
    // tunes off-centre.
    QMetaObject::invokeMethod(dsp, "setShift", Qt::QueuedConnection,
        Q_ARG(double, m_sliceFreqHz - m_ncoHz));

    ColibriDevice::Params p;
    p.dllPath = ColibriSettings::dllPath();
    if (request.params.contains(QStringLiteral("dllPath")))
        p.dllPath = request.params.value(QStringLiteral("dllPath")).toString();
    // "colibrinano-<n>" from discovery; a malformed serial falls back to 0.
    const QString serial = request.serial;
    const qsizetype dash = serial.lastIndexOf(QLatin1Char('-'));
    if (dash >= 0)
        p.deviceIndex = serial.mid(dash + 1).toUInt();
    p.sampleRateHz = m_sampleRateHz;
    p.frequencyHz = m_ncoHz;
    p.preampDb = m_preampDb;

    qCInfo(lcColibri) << "connecting: device" << p.deviceIndex << "rate"
                      << p.sampleRateHz << "Hz, NCO" << p.frequencyHz << "Hz";
    QMetaObject::invokeMethod(m_device, [dev = m_device, p] {
        dev->openDevice(p);
    }, Qt::QueuedConnection);
}

void ColibriBackend::disconnectRadio()
{
    QMetaObject::invokeMethod(m_device, &ColibriDevice::closeDevice,
                              Qt::BlockingQueuedConnection);
    publishIoDsp(nullptr);
    if (m_rxDsp) {
        m_rxDsp->deleteLater();   // lives on the I/O thread; its loop is running
        m_rxDsp = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        m_linkStatsTimer->stop();
        emit disconnected();
    }
}

bool ColibriBackend::isConnected() const
{
    return m_connected;
}

bool ColibriBackend::isOurPan(const QString& id) const
{
    // An EMPTY pan id addresses the (only) receiver — some seam callers omit
    // it for a single-pan radio.
    return id.isEmpty() || id == panId();
}

void ColibriBackend::publishIoDsp(ColibriRxDsp* dsp)
{
    if (m_device && m_ioThread && m_ioThread->isRunning()
        && QThread::currentThread() != m_ioThread) {
        // m_device is the handle onto the I/O thread's event loop. BLOCKS so
        // the caller may destroy the chain that was previously published.
        QMetaObject::invokeMethod(m_device, [this, dsp] { m_ioDsp = dsp; },
                                  Qt::BlockingQueuedConnection);
        return;
    }
    // Before the thread starts, after it is joined, or on it: nothing is
    // reading m_ioDsp concurrently in any of those.
    m_ioDsp = dsp;
}

void ColibriBackend::setSliceFrequency(int sliceId, double hz)
{
    if (sliceId != 0)
        return;
    m_sliceFreqHz = hz;

    // Keep the NCO — and therefore the panadapter centre — where it is, and
    // put the slice at an offset inside the passband. Only when the target
    // would fall outside the usable window does the NCO move (and then it
    // re-centres on the target). Same decoupling as HL2: without it the whole
    // display slides under the cursor on every click.
    const double halfSpanHz = static_cast<double>(m_sampleRateHz) / 2.0;
    const double usableHz = halfSpanHz * kUsablePassbandFraction;
    if (std::abs(hz - m_ncoHz) > usableHz) {
        m_ncoHz = hz;
        QMetaObject::invokeMethod(m_device, [dev = m_device, hz] {
            dev->setFrequencyHz(hz);
        }, Qt::QueuedConnection);
    }

    if (m_rxDsp)
        QMetaObject::invokeMethod(m_rxDsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, m_sliceFreqHz - m_ncoHz));

    emitSliceState();
    emitPanState();
    notifyOperatingStateChanged();
}

void ColibriBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (sliceId != 0)
        return;
    const QString previous = m_mode;
    m_mode = mode;
    const WdspChannel::Mode wdsp = modeFromString(mode);

    // The passband belongs to the mode; we own the DSP, so nothing heals a
    // stale filter for us. Adopted on CHANGE only, so an operator's own edit
    // survives until they change mode again.
    if (!previous.isEmpty() && previous.compare(mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }

    // ORDER IS LOAD-BEARING: mode FIRST, then passband, re-pushed on EVERY
    // mode set — SetRXAMode rebuilds the bandpass from its own per-mode
    // notion, discarding any filter applied before it (see Hl2Backend for the
    // USB->DIGU failure this prevents).
    if (m_rxDsp) {
        QMetaObject::invokeMethod(m_rxDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, wdsp));
        QMetaObject::invokeMethod(m_rxDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(m_filterLowHz)),
            Q_ARG(double, static_cast<double>(m_filterHighHz)));
    }
    emitSliceState();
    notifyOperatingStateChanged();
}

void ColibriBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (sliceId != 0)
        return;
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    if (m_rxDsp)
        QMetaObject::invokeMethod(m_rxDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(lowHz)),
            Q_ARG(double, static_cast<double>(highHz)));
    emitSliceState();
    notifyOperatingStateChanged();
}

void ColibriBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    if (sliceId != 0)
        return;
    m_agcMode = mode;
    m_agcThresholdDb = thresholdDb;
    if (m_rxDsp)
        QMetaObject::invokeMethod(m_rxDsp, "setAgc", Qt::QueuedConnection,
            Q_ARG(int, wdspAgcMode(mode)),
            Q_ARG(double, thresholdDb * kAgcCeilingDbPerUnit));
    emitSliceState();
}

void ColibriBackend::setSliceAudioMute(int sliceId, bool mute)
{
    if (sliceId != 0)
        return;
    m_audioMuted = mute;
    emitSliceState();
}

void ColibriBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if (sliceId != 0)
        return;
    // 0..100 -> linear, unity at 100 — the fader's whole travel attenuates,
    // matching what SliceModel's control means for a host-mixed backend.
    m_audioGain = static_cast<float>(std::clamp(gainPercent, 0, 100)) / 100.0f;
    emitSliceState();
}

void ColibriBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    if (sliceId != 0)
        return;
    m_audioPanPercent = std::clamp(panPercent, 0, 100);
    emitSliceState();
}

void ColibriBackend::setActiveSlice(int sliceId)
{
    // One slice; it is always the active one. Nothing to clear.
    Q_UNUSED(sliceId);
}

void ColibriBackend::setPanCenter(const QString& id, double hz,
                                  PanCenterIntent intent)
{
    // This receiver's window is its DDC, genuinely independent of the slice,
    // so a drag and a zoom-carried centre mean the same thing here.
    Q_UNUSED(intent);
    if (!isOurPan(id) || hz <= 0.0 || hz == m_ncoHz)
        return;
    // Moving the window means moving the NCO. The slice does NOT move with it
    // — its offset from the new centre is recomputed and re-applied.
    m_ncoHz = hz;
    QMetaObject::invokeMethod(m_device, [dev = m_device, hz] {
        dev->setFrequencyHz(hz);
    }, Qt::QueuedConnection);
    if (m_rxDsp)
        QMetaObject::invokeMethod(m_rxDsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, m_sliceFreqHz - m_ncoHz));
    emitPanState();
}

void ColibriBackend::setPanBandwidth(const QString& id, double hz)
{
    if (!isOurPan(id) || hz <= 0.0)
        return;

    // Coalesce a zoom sweep (#4470 shape): each span change is a stream
    // stop/start plus a blocking WDSP rebuild. Leading edge applies now so a
    // discrete step responds at once.
    if (!m_bandwidthThrottle) {
        m_bandwidthThrottle = new QTimer(this);
        m_bandwidthThrottle->setSingleShot(true);
        m_bandwidthThrottle->setInterval(kBandwidthThrottleMs);
        connect(m_bandwidthThrottle, &QTimer::timeout, this, [this] {
            if (m_pendingBandwidthHz <= 0.0)
                return;              // cooldown expired with nothing waiting
            const double pending = m_pendingBandwidthHz;
            m_pendingBandwidthHz = 0.0;
            applyPanBandwidth(pending);
            m_bandwidthThrottle->start();   // a sweep in progress keeps coalescing
        });
    }
    if (m_bandwidthThrottle->isActive()) {
        m_pendingBandwidthHz = hz;   // superseded by any later request
        return;
    }
    applyPanBandwidth(hz);
    m_bandwidthThrottle->start();
}

void ColibriBackend::applyPanBandwidth(double hz)
{
    const int rate = nearestSampleRateHz(hz);
    if (rate == m_sampleRateHz) {
        // Still re-publish: a zoom the hardware cannot honour must not leave
        // the display on the requested span — re-emitting the unchanged span
        // is how the widget snaps back to what is real (#4470).
        emitPanState();
        return;
    }
    if (!m_connected || !m_rxDsp) {
        m_sampleRateHz = rate;   // pre-connect: just adopt it for connect time
        emitPanState();
        return;
    }

    const int previousRate = m_sampleRateHz;
    m_sampleRateHz = rate;

    // ORDER: stop the stream, reconfigure the DSP, restart at the new rate —
    // all three serialized on the I/O thread's event loop, so blocks already
    // queued at the old rate drain into the old configuration first, and the
    // first block at the new rate meets a chain already expecting it.
    bool streamOk = false;
    QMetaObject::invokeMethod(m_device, [dev = m_device, rate, &streamOk] {
        streamOk = dev->restart(rate);
    }, Qt::BlockingQueuedConnection);

    ColibriRxDsp::Config dc;
    dc.inputSampleRateHz = m_sampleRateHz;
    dc.audioSampleRateHz = 24000;
    dc.mode = modeFromString(m_mode);
    dc.filterLowHz = m_filterLowHz;
    dc.filterHighHz = m_filterHighHz;
    // Carried through the rebuild rather than reapplied afterwards — a
    // reconfigured channel opens on Config defaults otherwise.
    dc.agcMode = wdspAgcMode(m_agcMode);
    dc.maximumAgcGainDb = m_agcThresholdDb * kAgcCeilingDbPerUnit;
    dc.wireAnalytic = ColibriSettings::wireAnalytic();
    std::string err;
    bool dspOk = false;
    ColibriRxDsp* dsp = m_rxDsp;
    QMetaObject::invokeMethod(dsp, [dsp, &dc, &err, &dspOk] {
        dspOk = dsp->configure(dc, &err);
    }, Qt::BlockingQueuedConnection);

    if (!streamOk || !dspOk) {
        // Fail back to the old rate so the wire and the DSP keep agreeing —
        // a rate split is silent wrong audio, not an error anything reports.
        qCWarning(lcColibri) << "span change to" << rate << "Hz failed"
                             << (streamOk ? "" : "(stream)")
                             << (dspOk ? "" : QString::fromStdString(err))
                             << "— staying at" << previousRate << "Hz";
        m_sampleRateHz = previousRate;
        QMetaObject::invokeMethod(m_device, [dev = m_device, previousRate] {
            dev->restart(previousRate);
        }, Qt::BlockingQueuedConnection);
        dc.inputSampleRateHz = previousRate;
        QMetaObject::invokeMethod(dsp, [dsp, &dc, &err] {
            std::string e;
            dsp->configure(dc, &e);
        }, Qt::BlockingQueuedConnection);
    } else {
        ColibriSettings::setSpanMhz(m_sampleRateHz / 1.0e6);
        qCInfo(lcColibri) << "span:" << m_sampleRateHz << "Hz";
    }
    emitPanState();
    notifyOperatingStateChanged();
}

void ColibriBackend::setPanRfGain(const QString& id, int gainDb)
{
    if (!isOurPan(id))
        return;
    applyPreampDb(std::clamp(gainDb, kPreampMinDb, kPreampMaxDb));
    notifyOperatingStateChanged();
}

void ColibriBackend::applyPreampDb(double db)
{
    if (db == m_preampDb)
        return;
    m_preampDb = db;
    QMetaObject::invokeMethod(m_device, [dev = m_device, db] {
        dev->setPreampDb(db);
    }, Qt::QueuedConnection);
    emit panRfGainChanged(panId(), static_cast<int>(std::lround(m_preampDb)));
}

void ColibriBackend::setPanFrameRate(const QString& id, int fps)
{
    if (!isOurPan(id) || !m_rxDsp)
        return;
    QMetaObject::invokeMethod(m_rxDsp, "setSpectrumRateFps", Qt::QueuedConnection,
        Q_ARG(int, fps));
}

void ColibriBackend::setKeying(bool key)
{
    // RX-only (RFC §5.5 Q3): the capability already made the engine guard
    // refuse this; getting here at all is a caller ignoring it.
    if (key)
        qCWarning(lcColibri) << "keying refused: the ColibriNANO is a receiver";
}

void ColibriBackend::invokeExtension(const QString& ns, const QString& verb,
                                     quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);
    if (requestId == 0)
        return;
    emit extensionError(requestId,
                        QStringLiteral("unknown extension %1.%2").arg(ns, verb));
}

void ColibriBackend::routeAudio(const std::vector<float>& pcm)
{
    // THIS SLICE's audio, pre-mute and pre-gain — a decoder consumer must not
    // stop decoding because the operator muted the speaker.
    emit sliceAudioFrameReady(0, floatBytes(pcm));

    if (m_audioMuted)
        return;
    if (m_audioGain == 1.0f && m_audioPanPercent == kAudioPanCentre) {
        emit audioFrameReady(floatBytes(pcm));   // steady state: no copy
        return;
    }
    // Balance is a BALANCE, not a constant-power pan: centre leaves both
    // channels at unity rather than dipping them 3 dB.
    const float left = m_audioPanPercent > kAudioPanCentre
        ? static_cast<float>(100 - m_audioPanPercent) / 50.0f : 1.0f;
    const float right = m_audioPanPercent < kAudioPanCentre
        ? static_cast<float>(m_audioPanPercent) / 50.0f : 1.0f;
    m_audioScratch.resize(pcm.size());
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        m_audioScratch[i] = pcm[i] * m_audioGain * left;
        m_audioScratch[i + 1] = pcm[i + 1] * m_audioGain * right;
    }
    emit audioFrameReady(floatBytes(m_audioScratch));
}

double ColibriBackend::dbfsToDbm(double dbfs) const
{
    return dbfs + kFullScaleDbm - m_preampDb;
}

void ColibriBackend::emitSliceState()
{
    SliceDelta d;
    d.panId = panId();
    d.frequency = m_sliceFreqHz / 1.0e6;   // MHz
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.agcMode = m_agcMode;
    d.agcThreshold = m_agcThresholdDb;
    d.audioMute = m_audioMuted;
    d.audioPan = m_audioPanPercent;
    // ONE slice: always active, never the TX slice — there is no transmitter
    // for an interlock to find. Explicit false rather than unset, so nothing
    // upstream is left resolving an absent answer.
    d.active = true;
    d.txSlice = false;
    emit sliceChanged(0, d);
}

void ColibriBackend::emitPanState()
{
    // The pan centre is the NCO, NOT the slice — the display describes where
    // the receiver's window is, and the slice moves inside it.
    emit panCenterBandwidthChanged(panId(), m_ncoHz / 1.0e6,
                                   static_cast<double>(m_sampleRateHz) / 1.0e6);
}

void ColibriBackend::pushInitialState()
{
    // Derive the passband from the mode ONCE per connect (#4484): a fresh
    // connect must not come up with another mode's filter, and a restored
    // passband must not be overwritten by the derivation.
    if (!m_passbandDerivedThisConnect) {
        m_passbandDerivedThisConnect = true;
        const auto [lo, hi] = defaultPassbandForMode(m_mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
    }
    if (m_rxDsp) {
        QMetaObject::invokeMethod(m_rxDsp, "setMode", Qt::QueuedConnection,
            Q_ARG(WdspChannel::Mode, modeFromString(m_mode)));
        QMetaObject::invokeMethod(m_rxDsp, "setFilter", Qt::QueuedConnection,
            Q_ARG(double, static_cast<double>(m_filterLowHz)),
            Q_ARG(double, static_cast<double>(m_filterHighHz)));
    }
}

void ColibriBackend::defineMeters()
{
    // Indices are ours to choose — nothing on the device assigns meter ids.
    MeterDef d;
    d.index = 1;
    d.source = QStringLiteral("SLC");
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level (uncalibrated)");
    emit meterDefined(d);
}

void ColibriBackend::publishLinkStats()
{
    LinkStats s = linkStats();
    // Fresh blocks since the last tick — the transport proof-of-life the
    // heartbeat runs on. Computed HERE rather than in linkStats() because the
    // comparison CONSUMES the previous value, and linkStats() is a const
    // getter any caller may poll at any rate.
    const quint64 blocks = m_device ? m_device->blocksReceived() : 0;
    s.alive = m_connected && blocks != m_linkBlocksAtLastTick;
    m_linkBlocksAtLastTick = blocks;
    emit linkStatsUpdated(s);
}

IRadioBackend::LinkStats ColibriBackend::linkStats() const
{
    LinkStats s;
    s.reported = m_connected;
    if (!m_connected || !m_device)
        return s;
    s.rxPackets = m_device->blocksReceived();
    s.rxBytes = static_cast<qint64>(m_device->samplesReceived() * sizeof(float) * 2);
    s.txBytes = 0;
    // No request/response exchange to time on a one-way USB stream: NOT
    // MEASURED, which must not render as zero.
    s.rttMs = -1;
    s.jitterMs = -1;
    s.localEndpoint = QStringLiteral("usb");
    return s;
}

IRadioBackend::HealthSnapshot ColibriBackend::healthSnapshot() const
{
    HealthSnapshot h;
    auto put = [&h](const QString& key, const QVariant& value, const QString& label) {
        h.values.insert(key, value);
        h.order.append(key);
        h.labels.insert(key, label);
    };
    auto& lib = ColibriLib::instance();
    if (lib.isLoaded()) {
        std::uint32_t maj = 0, min = 0, pat = 0;
        lib.version(maj, min, pat);
        put(QStringLiteral("libVersion"),
            QStringLiteral("%1.%2.%3").arg(maj).arg(min).arg(pat),
            QStringLiteral("Library version"));
        put(QStringLiteral("libPath"), lib.libraryPath(),
            QStringLiteral("Library path"));
    }
    put(QStringLiteral("sampleRate"),
        QStringLiteral("%1 kHz").arg(m_sampleRateHz / 1000),
        QStringLiteral("IQ sample rate"));
    put(QStringLiteral("preamp"), QStringLiteral("%1 dB").arg(m_preampDb),
        QStringLiteral("Preamp"));
    if (m_device) {
        put(QStringLiteral("samples"),
            static_cast<qulonglong>(m_device->samplesReceived()),
            QStringLiteral("IQ samples received"));
        // "0" and "we never heard" differ on a health readout; the flag rides
        // every block, so while connected it is always current.
        if (m_connected)
            put(QStringLiteral("adcOverload"),
                m_device->adcOverload() ? QStringLiteral("OVERLOAD")
                                        : QStringLiteral("ok"),
                QStringLiteral("ADC"));
    }
    h.sections.insert(h.order.isEmpty() ? QString{} : h.order.first(),
                      QStringLiteral("ColibriNANO"));
    return h;
}

void ColibriBackend::notifyOperatingStateChanged()
{
    emit operatingStateChanged();
}

}  // namespace AetherSDR::colibri
