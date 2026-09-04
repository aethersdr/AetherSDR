#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"

#include <QDebug>

#include <rtl-sdr.h>

#include <algorithm>
#include <cmath>

namespace AetherSDR::rtl {

namespace {

constexpr double kMinTuneHz = 24'000.0;
constexpr double kMaxTuneHz = 1'766'000'000.0;

double clampFrequency(double hz)
{
    return std::clamp(hz, kMinTuneHz, kMaxTuneHz);
}

bool isKnownMode(const QString& mode)
{
    static const QStringList modes{QStringLiteral("AM"), QStringLiteral("SAM"),
                                   QStringLiteral("FM"), QStringLiteral("FMN"),
                                   QStringLiteral("WFM"), QStringLiteral("USB"),
                                   QStringLiteral("LSB"), QStringLiteral("CW"),
                                   QStringLiteral("CWR")};
    return modes.contains(mode.trimmed().toUpper());
}

int nearestGainTenths(const QVector<int>& gains, int requested)
{
    if (gains.isEmpty()) {
        return requested;
    }
    return *std::min_element(gains.cbegin(), gains.cend(), [requested](int a, int b) {
        return std::abs(a - requested) < std::abs(b - requested);
    });
}

} // namespace

// Static convenience — returns the family string used by RadioModel::makeBackend().
QString RtlSdrBackend::familyName() { return QStringLiteral("rtl"); }

uint32_t RtlSdrBackend::clampSampleRate(uint32_t requestedHz)
{
    static const QVector<uint32_t> kSupportedRates = {
        225'001u, 250'000u, 300'000u, 1'000'000u,
        1'536'000u, 1'843'200u, 2'000'000u, 2'400'000u, 3'000'000u
    };

    uint32_t bestRate = kSupportedRates.front();
    int64_t minDiff = std::abs(static_cast<int64_t>(requestedHz) - static_cast<int64_t>(bestRate));

    for (uint32_t rate : kSupportedRates) {
        int64_t diff = std::abs(static_cast<int64_t>(requestedHz) - static_cast<int64_t>(rate));
        if (diff < minDiff) {
            minDiff = diff;
            bestRate = rate;
        }
    }

    return bestRate;
}

// ──────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ──────────────────────────────────────────────────────────────────────────────

RtlSdrBackend::RtlSdrBackend(QObject* parent)
    : IRadioBackend(parent)
{
}

RtlSdrBackend::~RtlSdrBackend()
{
    if (m_connected) {
        disconnectRadio();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend::capabilities
// ──────────────────────────────────────────────────────────────────────────────

RadioCapabilities RtlSdrBackend::capabilities() const
{
    RadioCapabilities c;
    c.family = QStringLiteral("rtl");
    c.model  = m_modelName;
    c.manufacturer = m_vendor.isEmpty() ? QStringLiteral("Realtek") : m_vendor;

    // TX — receive-only (Principle VI)
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = false;  // CRITICAL: must not open mic on connect (#4449)

    // Receiver limits
    c.maxSlices = 1;
    c.maxPanadapters = 1;

    // Tuning range — R820T: 24 MHz – 1.766 GHz (HF via direct sampling)
    c.tuningMinHz = 24'000;
    c.tuningMaxHz = 1'766'000'000;

    // Sample rates — non-contiguous legal windows for R820T
    c.sampleRatesHz = {
        225'001, 250'000, 300'000, 1'000'000,
        1'536'000, 1'843'200, 2'000'000, 2'400'000, 3'000'000
    };

    // Persistence — RTL-SDR has no radio-side memory
    c.persistsMemories = false;
    c.hasSupplyVoltageTelemetry = false;
    c.hasMultiClientSessions = false;

    // Client owns all state (RTL-SDR persists nothing)
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::Tuning
                            | RadioCapabilities::ClientSettingsDomain::Passband
                            | RadioCapabilities::ClientSettingsDomain::SpanRate
                            | RadioCapabilities::ClientSettingsDomain::RfGain
                            | RadioCapabilities::ClientSettingsDomain::Memories;

    // Vendor extensions
    c.extensions["rtl"] = QVariantMap{{"serial", m_serial}};
    c.extensionNamespaces = {"rtl"};

    return c;
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend::connectRadio / disconnectRadio
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::connectRadio(const RadioConnectRequest& request)
{
    if (m_connected) {
        disconnectRadio();
    }

    const auto params = request.params;
    const int deviceIdx = deviceIndexFromParams(params);
    QString targetSerial = request.serial.trimmed();
    if (targetSerial.isEmpty()) {
        targetSerial = serialFromParams(params);
    }

    // ── Discover the target device ──────────────────────────────────────────
    int count = rtlsdr_get_device_count();
    if (count == 0) {
        emit connectionError(tr("No RTL-SDR devices found"));
        return;
    }

    int idx = -1;
    if (!targetSerial.isEmpty()) {
        if (targetSerial.startsWith(QLatin1String("rtl:"))) {
            bool ok = false;
            int parsedIdx = targetSerial.mid(4).toInt(&ok);
            if (ok && parsedIdx >= 0 && parsedIdx < count) {
                idx = parsedIdx;
            }
        }
        if (idx < 0) {
            for (int i = 0; i < count; ++i) {
                char vendor[256] = {0};
                char product[256] = {0};
                char serial[256] = {0};
                if (rtlsdr_get_device_usb_strings(i, vendor, product, serial) == 0) {
                    QString s = QString::fromUtf8(serial).trimmed();
                    if (s == targetSerial || (s.isEmpty() && targetSerial == QStringLiteral("rtl:%1").arg(i))) {
                        idx = i;
                        break;
                    }
                }
            }
        }
        if (idx < 0) {
            emit connectionError(tr("RTL-SDR device with serial %1 not found").arg(targetSerial));
            return;
        }
    } else {
        idx = deviceIdx;
    }

    if (idx < 0 || idx >= count) {
        emit connectionError(tr("RTL-SDR device index %1 out of range (0-%2)").arg(idx).arg(count - 1));
        return;
    }

    // ── Open device ─────────────────────────────────────────────────────────
    rtlsdr_dev_t* devHandle = nullptr;
    int rc = rtlsdr_open(&devHandle, idx);
    if (rc < 0 || !devHandle) {
        // The two failures that actually happen in the field, named so the
        // operator can fix them without searching.  LIBUSB_ERROR_ACCESS is a
        // missing udev rule; LIBUSB_ERROR_BUSY is another process (or the
        // DVB-T kernel driver) still holding the device.
        QString hint;
        if (rc == -3) {
            hint = tr(" — no permission to open the USB device. Install the "
                      "udev rule (packaging/linux/70-rtl-sdr.rules) and "
                      "replug the dongle.");
        } else if (rc == -6) {
            hint = tr(" — the device is in use. Close any other SDR program, "
                      "or blacklist the dvb_usb_rtl28xxu kernel driver.");
        }
        emit connectionError(tr("Failed to open RTL-SDR device: error %1%2")
                                 .arg(rc).arg(hint));
        return;
    }
    m_device = devHandle;

    const auto failConfiguration = [this, devHandle](const QString& message) {
        rtlsdr_close(devHandle);
        m_device = nullptr;
        m_modelName.clear();
        m_vendor.clear();
        m_product.clear();
        m_serial.clear();
        m_tunerGainsTenths.clear();
        emit connectionError(message);
    };

    char vendorBuf[256] = {0};
    char productBuf[256] = {0};
    char serialBuf[256] = {0};
    if (rtlsdr_get_device_usb_strings(idx, vendorBuf, productBuf, serialBuf) == 0) {
        m_vendor  = QString::fromUtf8(vendorBuf);
        m_product = QString::fromUtf8(productBuf);
        m_serial  = QString::fromUtf8(serialBuf);
    } else {
        m_vendor  = tr("Realtek");
        m_product = tr("RTL2832U");
        m_serial  = QString::number(idx);
    }

    // ── Configure initial frequency, direct sampling, and gain ───────────────
    if (request.params.contains("initialFrequencyHz")) {
        m_panCenterHz = clampFrequency(request.params["initialFrequencyHz"].toDouble());
    } else if (m_panCenterHz <= 0) {
        m_panCenterHz = 101'700'000.0;
    }
    m_sliceFreqHz = m_panCenterHz;

    // Direct sampling mode MUST be configured before tuning center frequency,
    // because HF (< 24 MHz) requires direct sampling mode 2 before setting frequency.
    m_directSampling = m_panCenterHz < 24'000'000.0 ? 2 : 0;
    rc = rtlsdr_set_direct_sampling(devHandle, m_directSampling);
    if (rc < 0) {
        failConfiguration(tr("Failed to configure RTL-SDR direct sampling: error %1").arg(rc));
        return;
    }

    rc = rtlsdr_set_center_freq(devHandle, static_cast<uint32_t>(m_panCenterHz));
    if (rc < 0) {
        failConfiguration(tr("Failed to tune RTL-SDR to %1 Hz: error %2")
                              .arg(m_panCenterHz, 0, 'f', 0).arg(rc));
        return;
    }

    // Apply tuner gain (from request params or restored state)
    if (request.params.contains("gainDb")) {
        m_panRfGainDb = request.params["gainDb"].toInt();
    }
    m_tunerGainsTenths.clear();
    const int gainCount = rtlsdr_get_tuner_gains(devHandle, nullptr);
    if (gainCount > 0) {
        m_tunerGainsTenths.resize(gainCount);
        if (rtlsdr_get_tuner_gains(devHandle, m_tunerGainsTenths.data()) < 0) {
            m_tunerGainsTenths.clear();
        }
    }
    const int gainTenths = nearestGainTenths(m_tunerGainsTenths, m_panRfGainDb * 10);
    m_panRfGainDb = qRound(gainTenths / 10.0);
    rc = rtlsdr_set_tuner_gain_mode(devHandle, 1);
    if (rc < 0) {
        failConfiguration(tr("Failed to enable manual RTL-SDR tuner gain: error %1").arg(rc));
        return;
    }
    rc = rtlsdr_set_tuner_gain(devHandle, gainTenths);
    if (rc < 0) {
        failConfiguration(tr("Failed to configure RTL-SDR tuner gain: error %1").arg(rc));
        return;
    }
    // The RTL2832 digital AGC is independent of the tuner's gain mode. Keep it
    // disabled so it cannot ride on top of the operator's manual RF gain.
    rc = rtlsdr_set_agc_mode(devHandle, 0);
    if (rc < 0) {
        failConfiguration(tr("Failed to disable RTL-SDR digital AGC: error %1").arg(rc));
        return;
    }

    // ── Set sample rate (default 2.4 MSPS) ──────────────────────────────────
    uint32_t sampleRate = m_sampleRateHz;
    if (request.params.contains("sampleRateHz")) {
        sampleRate = request.params["sampleRateHz"].toUInt();
    }
    sampleRate = clampSampleRate(sampleRate);
    rc = rtlsdr_set_sample_rate(devHandle, sampleRate);
    if (rc < 0) {
        failConfiguration(tr("Failed to set RTL-SDR sample rate to %1 Hz: error %2")
                              .arg(sampleRate).arg(rc));
        return;
    }
    m_sampleRateHz = rtlsdr_get_sample_rate(devHandle);
    if (m_sampleRateHz == 0) {
        failConfiguration(tr("Failed to read back the applied RTL-SDR sample rate"));
        return;
    }

    // ── Set PPM frequency correction ────────────────────────────────────────
    if (m_ppmCorrection != 0) {
        rc = rtlsdr_set_freq_correction(devHandle, m_ppmCorrection);
        if (rc < 0) {
            failConfiguration(tr("Failed to set RTL-SDR frequency correction: error %1").arg(rc));
            return;
        }
    }

    // ── Identify tuner IC for model name ────────────────────────────────────
    m_modelName = m_product;

    // ── Instantiate Worker (owns RtlSdrDdc processing engine) ─────────────
    m_worker = std::make_unique<RtlSdrWorker>(m_device);

    if (RtlSdrDdc* ddcEngine = m_worker->ddc()) {
        ddcEngine->setSampleRate(m_sampleRateHz);
        ddcEngine->setCenterFrequency(m_panCenterHz);
        ddcEngine->setSliceFrequency(m_sliceFreqHz);
        ddcEngine->setSliceMode(m_sliceMode);
        ddcEngine->setSliceFilter(m_sliceFilterLow, m_sliceFilterHigh);
    }

    // Relay worker/DDC signals to IRadioBackend outputs via cross-thread queued connection
    connect(m_worker.get(), &RtlSdrWorker::spectrumFrameReady,
            this, &IRadioBackend::spectrumFrameReady);
    connect(m_worker.get(), &RtlSdrWorker::waterfallRowReady,
            this, &IRadioBackend::waterfallRowReady);
    connect(m_worker.get(), &RtlSdrWorker::audioFrameReady,
            this, [this](const QByteArray& pcm) {
                emit audioFrameReady(pcm);
                emit sliceAudioFrameReady(0, pcm);
            });
    connect(m_worker.get(), &RtlSdrWorker::readError,
            this, [this](const QString& err) {
                emit connectionError(err);
                disconnectRadio();
            });
    connect(m_worker.get(), &RtlSdrWorker::controlApplied,
            this, &RtlSdrBackend::handleControlApplied);
    connect(m_worker.get(), &RtlSdrWorker::controlFailed,
            this, &RtlSdrBackend::handleControlFailed);

    m_worker->startReading();

    // ── Mark connected, emit signals ────────────────────────────────────────
    m_connected = true;
    emit connected();
    emitInitialState();
}

void RtlSdrBackend::disconnectRadio()
{
    if (!m_connected) {
        return;
    }

    for (auto it = m_pendingExtensionRequests.cbegin();
         it != m_pendingExtensionRequests.cend(); ++it) {
        for (quint64 requestId : it.value()) {
            emit extensionError(requestId, tr("RTL-SDR disconnected before the control completed"));
        }
    }
    m_pendingExtensionRequests.clear();

    // The worker owns the device handle and closes it only after read_async has
    // exited. If a broken USB stack ignores cancellation, leave the worker
    // alive until QThread::finished rather than destroying a live QThread or
    // closing a handle underneath libusb.
    if (m_worker) {
        if (m_worker->stopReading()) {
            m_worker.reset();
        } else {
            RtlSdrWorker* stranded = m_worker.release();
            connect(stranded, &QThread::finished, stranded, &QObject::deleteLater);
        }
        m_device = nullptr;
    }

    m_connected = false;
    m_modelName.clear();
    m_vendor.clear();
    m_product.clear();
    m_serial.clear();
    m_tunerGainsTenths.clear();

    emit disconnected();
}

bool RtlSdrBackend::isConnected() const
{
    return m_connected;
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — slice control
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::setSliceFrequency(int sliceId, double hz)
{
    Q_UNUSED(sliceId);  // Single slice

    if (!m_connected) {
        return;
    }

    m_sliceFreqHz = clampFrequency(hz);

    // Check if the requested slice frequency is outside the current 2.4 MHz panadapter window (±1.0 MHz)
    const double spanMarginHz = m_sampleRateHz * 0.45;
    if (std::abs(m_sliceFreqHz - m_panCenterHz) > spanMarginHz) {
        m_panCenterHz = m_sliceFreqHz;
        if (m_worker) {
            const int requiredDs = (m_panCenterHz < 24'000'000.0) ? 2 : 0;
            if (m_directSampling != requiredDs) {
                m_directSampling = requiredDs;
                m_worker->setDirectSampling(requiredDs);
            }
            m_worker->setCenterFrequency(static_cast<uint32_t>(m_panCenterHz));
        }
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setCenterFrequency(m_panCenterHz);
        }
        emit panCenterBandwidthChanged(QStringLiteral("0xe1000000"), m_panCenterHz / 1e6,
                                       m_sampleRateHz / 1e6);
    }

    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSliceFrequency(m_sliceFreqHz);
    }

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    emit sliceChanged(0, delta);
    emit operatingStateChanged();
}

void RtlSdrBackend::setSliceMode(int sliceId, const QString& mode)
{
    Q_UNUSED(sliceId);

    if (!m_connected) {
        return;
    }

    const QString canonicalMode = mode.trimmed().toUpper();
    if (!isKnownMode(canonicalMode)) {
        return;
    }
    m_sliceMode = canonicalMode;
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSliceMode(canonicalMode);
    }

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = canonicalMode;
    emit sliceChanged(0, delta);
    emit operatingStateChanged();
}

void RtlSdrBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Q_UNUSED(sliceId);

    if (!m_connected) {
        return;
    }

    if (lowHz >= highHz || lowHz < -100'000 || highHz > 100'000) {
        return;
    }
    m_sliceFilterLow = lowHz;
    m_sliceFilterHigh = highHz;
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSliceFilter(lowHz, highHz);
    }

    SliceDelta delta;
    delta.frequency = m_sliceFreqHz / 1e6;
    delta.mode = m_sliceMode;
    delta.filterLow = lowHz;
    delta.filterHigh = highHz;
    emit sliceChanged(0, delta);
    emit operatingStateChanged();
}

void RtlSdrBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Q_UNUSED(sliceId);
    Q_UNUSED(mode);
    Q_UNUSED(thresholdDb);
    // Phase 1: AGC is engine-side DSP, not hardware.
    // The DDC will apply AGC in Phase 2.
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — pan control
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::setPanCenter(const QString& panId, double hz,
                                  PanCenterIntent intent)
{
    Q_UNUSED(intent);
    if (!m_connected) {
        return;
    }

    hz = clampFrequency(hz);
    m_panCenterHz = hz;
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setCenterFrequency(hz);
    }

    // Dispatch blocking USB control transfer to worker thread (§ Hazards #2)
    if (m_worker) {
        const int requiredDs = (hz < 24'000'000.0) ? 2 : 0;
        if (m_directSampling != requiredDs) {
            m_directSampling = requiredDs;
            m_worker->setDirectSampling(requiredDs);
        }
        m_worker->setCenterFrequency(static_cast<uint32_t>(hz));
    }

    const QString effectivePanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;
    emit panCenterBandwidthChanged(effectivePanId, hz / 1e6, m_sampleRateHz / 1e6);
    // Update slice frequency to track pan center (single-slice design)
    m_sliceFreqHz = hz;
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSliceFrequency(hz);
    }

    SliceDelta delta;
    delta.frequency = hz / 1e6;
    delta.mode = m_sliceMode;
    emit sliceChanged(0, delta);
    emit operatingStateChanged();
}

void RtlSdrBackend::setPanBandwidth(const QString& panId, double hz)
{
    if (!m_connected || !m_worker) {
        return;
    }
    const uint32_t rate = clampSampleRate(
        static_cast<uint32_t>(std::clamp(hz, 1.0, static_cast<double>(UINT32_MAX))));
    m_pendingPanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;
    m_worker->setSampleRate(rate);
}

void RtlSdrBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSpectrumRateFps(fps);
    }
}

void RtlSdrBackend::setSliceAudioMute(int sliceId, bool mute)
{
    if (sliceId == 0) {
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setAudioMute(mute);
        }
    }
}

void RtlSdrBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if (sliceId == 0) {
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setAudioGain(gainPercent);
        }
    }
}

void RtlSdrBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    if (sliceId == 0) {
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setAudioPan(panPercent);
        }
    }
}

void RtlSdrBackend::setPanRfGain(const QString& panId, int gainDb)
{
    if (!m_connected) {
        return;
    }

    const int gainTenths = nearestGainTenths(m_tunerGainsTenths, gainDb * 10);
    m_pendingPanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;

    // Dispatch blocking USB control transfer to worker thread (§ Hazards #2)
    if (m_worker) {
        m_worker->setTunerGain(gainTenths);
    }

}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — transmit (guarded — RX-only)
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::setKeying(bool key)
{
    Q_UNUSED(key);
    // RTL-SDR is receive-only. This is a no-op.
    // The bridge TX gate (AETHER_AUTOMATION_ALLOW_TX) is the real guard.
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — vendor extensions
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::invokeExtension(const QString& ns, const QString& verb,
                                    quint64 requestId, const QVariant& arg)
{
    if (ns != "rtl") {
        emit extensionError(requestId, tr("Unknown namespace %1").arg(ns));
        return;
    }

    if (!m_connected || !m_device) {
        emit extensionError(requestId, tr("Not connected"));
        return;
    }

    if (verb == "gain.set") {
        bool ok = false;
        const int gain = arg.toInt(&ok);
        if (!ok) {
            emit extensionError(requestId, tr("gain.set requires an integer dB value"));
            return;
        }
        queueExtensionRequest(QStringLiteral("gain"), requestId);
        setPanRfGain(QStringLiteral("0xe1000000"), gain);

    } else if (verb == "gain.list") {
        emit extensionResult(requestId, QVariant::fromValue(m_tunerGainsTenths));

    } else if (verb == "ppm.set") {
        bool ok = false;
        int ppm = arg.toInt(&ok);
        if (!ok || ppm < -1000 || ppm > 1000) {
            emit extensionError(requestId, tr("ppm.set requires an integer from -1000 to 1000"));
            return;
        }
        queueExtensionRequest(QStringLiteral("ppm"), requestId);
        if (m_worker) {
            m_worker->setPpmCorrection(ppm);
        }

    } else if (verb == "direct_sampling.set") {
        // Mode 0 = Off (tuner), Mode 1 = I-branch, Mode 2 = Q-branch (HF direct)
        bool ok = false;
        int mode = arg.toInt(&ok);
        if (!ok || mode < 0 || mode > 2) {
            emit extensionError(requestId, tr("direct_sampling.set requires 0, 1, or 2"));
            return;
        }
        queueExtensionRequest(QStringLiteral("direct_sampling"), requestId);
        if (m_worker) {
            m_worker->setDirectSampling(mode);
        }

    } else if (verb == "offset_tuning.set") {
        bool ok = false;
        int enable = arg.toInt(&ok);
        if (!ok || (enable != 0 && enable != 1)) {
            emit extensionError(requestId, tr("offset_tuning.set requires 0 or 1"));
            return;
        }
        queueExtensionRequest(QStringLiteral("offset_tuning"), requestId);
        if (m_worker) {
            m_worker->setOffsetTuning(enable);
        }

    } else if (verb == "sample_rate.set") {
        bool ok = false;
        uint32_t rate = clampSampleRate(arg.toUInt(&ok));
        if (!ok) {
            emit extensionError(requestId, tr("sample_rate.set requires an integer Hz value"));
            return;
        }
        queueExtensionRequest(QStringLiteral("sample_rate"), requestId);
        if (m_worker) {
            m_worker->setSampleRate(rate);
        }

    } else {
        emit extensionError(requestId, tr("Unknown RTL extension %1").arg(verb));
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — client-side state memory
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::applyRestoredState(const RestoredRadioState& state)
{
    // Restore is applied during connectRadio for RTL-SDR because the device
    // has no persistent state — frequency, gain, PPM are applied as setpoints
    // after rtlsdr_open(). The RadioModel calls applyRestoredState before
    // connectRadio, so we stash the values and use them in connectRadio.
    //
    // NOTE: restore never keys transmit (Principle VI).
    // Reset first: RadioModel can reuse this backend object for another dongle,
    // and an empty snapshot must not inherit the previous radio's settings.
    m_panCenterHz = 95'200'000.0;
    m_sliceFreqHz = m_panCenterHz;
    m_sliceMode = QStringLiteral("WFM");
    m_sliceFilterLow = -100'000;
    m_sliceFilterHigh = 100'000;
    m_sampleRateHz = 2'400'000;
    m_panRfGainDb = kDefaultRfGainDb;
    m_ppmCorrection = 0;
    m_directSampling = 0;

    if (state.rfFrequencyHz > 0) {
        const double clampedHz = clampFrequency(state.rfFrequencyHz);
        m_panCenterHz = clampedHz;
        m_sliceFreqHz = clampedHz;
    }
    if (isKnownMode(state.mode)) {
        m_sliceMode = state.mode.trimmed().toUpper();
    }
    if (state.filterLowHz < state.filterHighHz
        && state.filterLowHz >= -100'000.0 && state.filterHighHz <= 100'000.0) {
        m_sliceFilterLow = qRound(state.filterLowHz);
        m_sliceFilterHigh = qRound(state.filterHighHz);
    }
    if (state.sampleRateHz > 0) {
        m_sampleRateHz = clampSampleRate(static_cast<uint32_t>(state.sampleRateHz));
    }

    const QJsonObject rfGain =
        state.extension.value(QStringLiteral("rfGain")).toObject();
    if (rfGain.contains(QStringLiteral("gainDb"))) {
        m_panRfGainDb = std::clamp(rfGain.value(QStringLiteral("gainDb")).toInt(),
                                   -100, 100);
    }
}

RestoredRadioState RtlSdrBackend::currentOperatingState() const
{
    RestoredRadioState state;
    state.rfFrequencyHz = m_panCenterHz;
    state.mode = m_sliceMode;
    state.filterLowHz = m_sliceFilterLow;
    state.filterHighHz = m_sliceFilterHigh;
    state.sampleRateHz = static_cast<int>(m_sampleRateHz);
    state.extensionSchemaVersion = 1;

    state.extension[QStringLiteral("rfGain")] =
        QJsonObject{{QStringLiteral("gainDb"), m_panRfGainDb}};
    return state;
}

// ──────────────────────────────────────────────────────────────────────────────
// Private helpers
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::queueExtensionRequest(const QString& control, quint64 requestId)
{
    m_pendingExtensionRequests[control].append(requestId);
}

void RtlSdrBackend::handleControlApplied(const QString& control, qint64 value)
{
    if (!m_connected) {
        return;
    }

    QVariant result = value;
    if (control == QLatin1String("gain")) {
        m_panRfGainDb = qRound(value / 10.0);
        result = m_panRfGainDb;
        emit panRfGainChanged(m_pendingPanId, m_panRfGainDb);
        emit operatingStateChanged();
    } else if (control == QLatin1String("sample_rate")) {
        m_sampleRateHz = static_cast<uint32_t>(value);
        result = m_sampleRateHz;
        emit panCenterBandwidthChanged(m_pendingPanId, m_panCenterHz / 1e6,
                                       m_sampleRateHz / 1e6);
        emit operatingStateChanged();
    } else if (control == QLatin1String("center_frequency")) {
        m_panCenterHz = static_cast<double>(value);
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setCenterFrequency(m_panCenterHz);
        }
        emit panCenterBandwidthChanged(m_pendingPanId, m_panCenterHz / 1e6,
                                       m_sampleRateHz / 1e6);
        emit operatingStateChanged();
    } else if (control == QLatin1String("direct_sampling")) {
        m_directSampling = static_cast<int>(value);
    } else if (control == QLatin1String("ppm")) {
        m_ppmCorrection = static_cast<int>(value);
    }

    const QVector<quint64> requests = m_pendingExtensionRequests.take(control);
    for (quint64 requestId : requests) {
        emit extensionResult(requestId, result);
    }
}

void RtlSdrBackend::handleControlFailed(const QString& control, const QString& message)
{
    if (!m_connected) {
        return;
    }

    const QVector<quint64> requests = m_pendingExtensionRequests.take(control);
    for (quint64 requestId : requests) {
        emit extensionError(requestId, message);
    }
    if (requests.isEmpty()) {
        emit connectionError(message);
    }
}

void RtlSdrBackend::emitInitialState()
{
    // Emit the signals the UI expects from a freshly-connected radio.
    // Mirrors what the Flex backend does with initial status echoes.
    RadioDelta rDelta;
    rDelta.model = m_modelName;
    rDelta.nickname = m_product;
    emit radioChanged(rDelta);

    const QString kPanId = QStringLiteral("0xe1000000");

    // Pan 0 FIRST — center frequency and bandwidth limits so PanadapterModel materialises
    emit panCenterBandwidthChanged(kPanId, m_panCenterHz / 1e6, m_sampleRateHz / 1e6);
    emit panBandwidthLimitsChanged(kPanId, 0.225001, 3.0);

    // Tuner RF gain info
    if (!m_tunerGainsTenths.isEmpty()) {
        const auto [minIt, maxIt] = std::minmax_element(m_tunerGainsTenths.cbegin(),
                                                        m_tunerGainsTenths.cend());
        emit panRfGainInfoChanged(kPanId, qFloor(*minIt / 10.0), qCeil(*maxIt / 10.0), 1);
    }

    // RF gain
    emit panRfGainChanged(kPanId, m_panRfGainDb);

    // Slice 0 — initial frequency, mode, and filters
    SliceDelta sDelta;
    sDelta.frequency = m_sliceFreqHz / 1e6;
    sDelta.mode = m_sliceMode;
    sDelta.filterLow = m_sliceFilterLow;
    sDelta.filterHigh = m_sliceFilterHigh;
    // No txAntenna or rxAntenna list on hardware without software antenna switches (Constitution Principle II & VI)
    sDelta.modeList = QStringList{QStringLiteral("AM"), QStringLiteral("SAM"),
                                  QStringLiteral("FM"), QStringLiteral("FMN"),
                                  QStringLiteral("WFM"), QStringLiteral("USB"),
                                  QStringLiteral("LSB"), QStringLiteral("CW"),
                                  QStringLiteral("CWR")};
    sDelta.active = true;
    sDelta.panId = kPanId;
    emit sliceChanged(0, sDelta);
}

RtlSdrDdc* RtlSdrBackend::ddc()
{
    return m_worker ? m_worker->ddc() : nullptr;
}

int RtlSdrBackend::deviceIndexFromParams(const QVariantMap& params) const
{
    return params.value("rtl.deviceIndex", 0).toInt();
}

QString RtlSdrBackend::serialFromParams(const QVariantMap& params) const
{
    return params.value("rtl.serialNumber").toString();
}

}  // namespace AetherSDR::rtl
