#include "core/backends/rtl/RtlSdrBackend.h"

#include <QPointer>
#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"

#include <QDebug>

#include <rtl-sdr.h>

#include <algorithm>
#include <cmath>
#include <utility>

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

RtlCaptureTransaction::Mode captureMode(const QString& mode)
{
    using Mode = RtlCaptureTransaction::Mode;
    if (mode == QLatin1String("AM")) { return Mode::Am; }
    if (mode == QLatin1String("SAM")) { return Mode::Sam; }
    if (mode == QLatin1String("FM")) { return Mode::Fm; }
    if (mode == QLatin1String("FMN")) { return Mode::Fmn; }
    if (mode == QLatin1String("WFM")) { return Mode::Wfm; }
    if (mode == QLatin1String("LSB")) { return Mode::Lsb; }
    if (mode == QLatin1String("CW")) { return Mode::Cw; }
    if (mode == QLatin1String("CWR")) { return Mode::Cwr; }
    return Mode::Usb;
}
QString modeName(RtlCaptureTransaction::Mode mode)
{
    static const QStringList names{QStringLiteral("AM"), QStringLiteral("SAM"),
        QStringLiteral("FM"), QStringLiteral("FMN"), QStringLiteral("WFM"),
        QStringLiteral("USB"), QStringLiteral("LSB"), QStringLiteral("CW"), QStringLiteral("CWR")};
    return names.at(static_cast<int>(mode));
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
    m_captureTimer.setInterval(10);
    connect(&m_captureTimer, &QTimer::timeout, this, &RtlSdrBackend::serviceCapture);
}

RtlSdrBackend::~RtlSdrBackend()
{
    if (m_worker) {
        disconnectRadio();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend::capabilities
// ──────────────────────────────────────────────────────────────────────────────

RadioCapabilities RtlSdrBackend::capabilities() const
{
    // #5594 (M1) item 4: this backend deliberately never emits
    // capabilitiesChanged, and that is the honest answer rather than a gap.
    //
    // Every field below is either a compile-time constant for the R820T/RTL2832U
    // pair or comes from the USB descriptor strings (m_vendor, m_product /
    // m_modelName, and m_serial), read during connectRadio() before connected()
    // and cleared on disconnect or a configuration failure before connection.
    // The declaration is fixed for the whole session: no mid-session revision, and
    // a synthetic emission would be noise dressed up as a contract.
    //
    // If a future tuner-dependent field is added here (a per-tuner gain table,
    // a direct-sampling range that depends on the IC), it becomes revisable and
    // this comment stops being true.
    RadioCapabilities c;
    // THE dBm AXIS IS UNCALIBRATED, and on this backend that is not a nuance:
    // RtlSdrDdc's FFT path computes `20 * log10(mag / kFftSize)` on raw ADC
    // magnitudes and emits that straight out as the spectrum frame. There is no
    // reference object, no offset and no per-unit figure anywhere in this
    // family -- the axis is dBFS relative to the converter's own full scale.
    //
    // A relative reading is still useful; an absolute one is not available, so
    // a level from this radio may not be published as a spot, held against
    // another station's report, or used as an absolute threshold.
    PanAmplitudeModel amplitude;
    amplitude.calibratedDbm = false;
    // The spectrum bins are computed on THIS host from raw ADC magnitudes and
    // carry no reference level: RtlSdrDdc::processSpectrum takes the FFT output,
    // forms `mag = sqrt(re*re + im*im) / kFftSize` and emits
    // `20 * log10(max(mag, 1e-6))` straight into the frame. Nothing in that
    // expression can move when the display reference level moves, so the
    // noise-floor auto-adjust has a fixed target and terminates — see
    // PanAmplitudeModel::binsAbsolute. radioOwnsDbmScale is deliberately
    // left at its permissive default here and NOT flipped in the same change:
    // this radio has no range command, but correcting that declaration is a
    // separate question from this one and belongs with its own reasoning.
    amplitude.binsAbsolute = true;
    c.panAmplitude = amplitude;
    c.family = QStringLiteral("rtl");
    c.model  = m_modelName;
    c.manufacturer = m_vendor.isEmpty() ? QStringLiteral("Realtek") : m_vendor;

    // TX — receive-only (Principle VI)
    c.canTransmit = false;
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = false;  // CRITICAL: must not open mic on connect (#4449)
    // transmitDriveControl absent: no transmitter, so no drive to own (#5518).
    c.hasRadioPttReadback = false;  // receive-only: nothing to key, nothing to read back
    c.hasFmRepeaterOffset = false;
    c.hasCwTune = false;
    c.twoToneGenerator = std::nullopt;  // receive only; there is no transmitter.
    c.hasAmCarrierLevel = false;
    c.hasVoxDelay = false;
    c.hasAgcThreshold = false;
    c.hasModeIndependentSquelch = false;
    c.agcModes = {QStringLiteral("off"), QStringLiteral("slow"),
                  QStringLiteral("med"), QStringLiteral("fast")};
    // Unused TX presentation retains the shared legacy shape; canTransmit
    // above keeps these controls unavailable on the receive-only backend.
    c.alcMeterUnit = QStringLiteral("dBFS");
    c.compressionMaximumDb = 25.0f;
    c.cwSpeedMinWpm = 5;
    c.cwSpeedMaxWpm = 100;
    c.cwPitchMinHz = 100;
    c.cwPitchMaxHz = 6000;
    c.cwPitchStepHz = 10;

    // Receiver limits
    c.canCreateSlices = false;
    c.maxSlices = 1;
    c.maxPanadapters = 1;

    // Tuning range — R820T: 24 MHz – 1.766 GHz (HF via direct sampling)
    c.tuningMinHz = 24'000;
    c.tuningMaxHz = 1'766'000'000;
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine,
                               24'000, 1'766'000'000};
    c.receiveModeControl = ReceiveModeControl{SliceFrequencyControl::Authority::Engine,
        {QStringLiteral("AM"), QStringLiteral("SAM"), QStringLiteral("FM"),
         QStringLiteral("FMN"), QStringLiteral("WFM"), QStringLiteral("USB"),
         QStringLiteral("LSB"), QStringLiteral("CW"), QStringLiteral("CWR")}};
    c.receiveFilterControl = std::nullopt; // DDC currently stores, but never consumes, filter edges
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receivePanCenterControl = std::nullopt; // setPanCenter also retunes slice 0
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                         225'001, 3'000'000};

    // Sample rates — non-contiguous legal windows for R820T
    c.sampleRatesHz = {
        225'001, 250'000, 300'000, 1'000'000,
        1'536'000, 1'843'200, 2'000'000, 2'400'000, 3'000'000
    };

    // Persistence — RTL-SDR has no radio-side memory
    c.persistsMemories = false;
    c.hasSupplyVoltageTelemetry = false;
    c.hasMultiClientSessions = false;
    c.hasAudioPeakingFilter = false;

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
    if (m_worker) {
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
        if ((request.serialIdentity.indexLocator
             || request.serialIdentity.reportedSerial.isEmpty())
            && targetSerial.startsWith(QLatin1String("rtl:"))) {
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

    char vendorBuf[256] = {0};
    char productBuf[256] = {0};
    char serialBuf[256] = {0};
    if (rtlsdr_get_device_usb_strings(idx, vendorBuf, productBuf, serialBuf) == 0) {
        m_vendor  = QString::fromUtf8(vendorBuf);
        m_product = QString::fromUtf8(productBuf);
        m_serial  = QString::fromUtf8(serialBuf).trimmed();
    } else {
        m_vendor  = tr("Realtek");
        m_product = tr("RTL2832U");
        m_serial.clear(); // Enumeration indices are connection locators, never serials.
    }

    // All capture controls are requested together; only the worker touches
    // them, and only its confirmed result is exposed as connected state.
    m_requested = {};
    m_requested.hardware.centerHz = static_cast<std::uint32_t>(m_panCenterHz);
    m_requested.hardware.sampleRateHz = m_sampleRateHz;
    m_requested.hardware.ppm = m_ppmCorrection;
    if (params.contains("initialFrequencyHz")) {
        const double hz = params.value("initialFrequencyHz").toDouble();
        if (!std::isfinite(hz)) {
            rtlsdr_close(devHandle); m_device = nullptr;
            emit connectionError(tr("Invalid RTL-SDR initial frequency")); return;
        }
        m_requested.hardware.centerHz = static_cast<std::uint32_t>(clampFrequency(hz));
    }
    if (params.contains("sampleRateHz")) {
        m_requested.hardware.sampleRateHz = clampSampleRate(params.value("sampleRateHz").toUInt());
    }
    const int requestedGain = std::clamp(params.value("gainDb", m_panRfGainDb).toInt(), -100, 100);
    m_tunerGainsTenths.clear();
    const int gainCount = rtlsdr_get_tuner_gains(devHandle, nullptr);
    if (gainCount > 0 && gainCount <= 256) {
        m_tunerGainsTenths.resize(gainCount);
        if (rtlsdr_get_tuner_gains(devHandle, m_tunerGainsTenths.data()) < 0) {
            m_tunerGainsTenths.clear();
        }
    }
    m_requested.hardware.gainTenths = nearestGainTenths(m_tunerGainsTenths, requestedGain * 10);
    m_requested.receivers = {{{0, double(m_requested.hardware.centerHz),
        double(m_sliceFilterLow), double(m_sliceFilterHigh), 0, 0, 0}, captureMode(m_sliceMode)}};
    m_modelName = m_product;
    startCapture(std::make_unique<RtlSdrWorker>(m_device));
}

void RtlSdrBackend::startCapture(std::unique_ptr<RtlSdrWorker> worker)
{
    m_worker = std::move(worker);
    m_capture.beginSession();
    m_published = {};
    m_connecting = true;
    m_pendingPanId = QStringLiteral("0xe1000000");
    m_receiveGain = 100;
    m_receiveMuted = false;
    const QPointer<RtlSdrWorker> producer(m_worker.get());
    connect(m_worker.get(), &RtlSdrWorker::spectrumFrameReady, this,
        [this, producer](quint64 session, quint64 revision, int panId, const QByteArray& frame) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                emit spectrumFrameReady(panId, frame);
            }
        });
    connect(m_worker.get(), &RtlSdrWorker::waterfallRowReady, this,
        [this, producer](quint64 session, quint64 revision, int panId, const QByteArray& frame) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                emit waterfallRowReady(panId, frame);
            }
        });
    connect(m_worker.get(), &RtlSdrWorker::audioFrameReady, this,
        [this, producer](quint64 session, quint64 revision, const QByteArray& pcm) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                publishLegacyAudio(pcm);
                publishLegacySliceAudio(0, pcm);
            }
        });
    connect(m_worker.get(), &RtlSdrWorker::readError, this,
        [this, producer](const QString& error) {
            if (!producer || producer.data() != m_worker.get()) { return; }
            emit connectionError(error);
            if (producer && producer.data() == m_worker.get()) { disconnectRadio(); }
        });
    if (!requestCapture(m_requested)) {
        emit connectionError(tr("RTL-SDR initial capture could not be prepared"));
        disconnectRadio();
        return;
    }
    m_captureTimer.start();
    m_worker->startReading();
}

void RtlSdrBackend::disconnectRadio()
{
    retirePcmStreams();
    m_captureTimer.stop();
    m_capture.endSession();
    m_published = {};
    if (!m_worker && !m_connected && !m_connecting) { return; }
    m_connected = false;
    m_connecting = false;
    const auto retiredSession = m_capture.requested().session;
    const auto requests = std::exchange(m_pendingExtensionRequests, {});
    auto worker = std::move(m_worker);
    m_device = nullptr;
    m_modelName.clear();
    m_vendor.clear();
    m_product.clear();
    m_serial.clear();
    m_tunerGainsTenths.clear();

    // Detach before notifying observers. A reentrant reconnect must never have
    // its new worker stopped by this old session's teardown.
    if (worker) {
        // Install retirement before waiting: the reader can finish immediately
        // after the bounded wait expires. Connecting afterwards can miss that
        // final signal and strand the device forever. Normal joined teardown
        // deletes the QObject below, also removing any deferred-delete event.
        connect(worker.get(), &QThread::finished, worker.get(), &QObject::deleteLater);
        if (!worker->stopReading()) {
            worker.release();
        }
    }
    worker.reset();
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        emit extensionError(it.value().requestId, tr("RTL-SDR disconnected before the control completed"));
        if (m_capture.requested().session != retiredSession) { return; }
    }
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
    if (!m_connected || sliceId != 0 || !std::isfinite(hz)) { return; }
    auto desired = m_requested;
    desired.receivers[0].passband.carrierHz = clampFrequency(hz);
    desired.automaticDirectSampling = true;
    requestCapture(desired);
}

void RtlSdrBackend::setSliceMode(int sliceId, const QString& mode)
{
    const QString canonical = mode.trimmed().toUpper();
    if (!m_connected || sliceId != 0 || !isKnownMode(canonical)) { return; }
    auto desired = m_requested;
    desired.receivers[0].mode = captureMode(canonical);
    requestCapture(desired);
}

void RtlSdrBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (!m_connected || sliceId != 0 || lowHz >= highHz
        || lowHz < -100'000 || highHz > 100'000) { return; }
    auto desired = m_requested;
    desired.receivers[0].passband.filterLowHz = lowHz;
    desired.receivers[0].passband.filterHighHz = highHz;
    requestCapture(desired);
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

void RtlSdrBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(intent);
    if (!m_connected || !std::isfinite(hz)) { return; }
    auto desired = m_requested;
    desired.hardware.centerHz = static_cast<std::uint32_t>(clampFrequency(hz));
    // Retain the existing single-slice pan behavior until viewport work lands.
    desired.receivers[0].passband.carrierHz = desired.hardware.centerHz;
    desired.automaticDirectSampling = true;
    if (requestCapture(desired)) {
        m_pendingPanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;
    }
}

void RtlSdrBackend::setPanBandwidth(const QString& panId, double hz)
{
    if (!m_connected || !std::isfinite(hz)) { return; }
    auto desired = m_requested;
    desired.hardware.sampleRateHz = clampSampleRate(static_cast<std::uint32_t>(
        std::clamp(hz, 1.0, double(UINT32_MAX))));
    if (requestCapture(desired)) {
        m_pendingPanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;
    }
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
            m_receiveMuted = mute;
            SliceDelta delta;
            delta.audioMute = mute;
            emit sliceChanged(sliceId, delta);
        }
    }
}

void RtlSdrBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if (sliceId == 0) {
        if (RtlSdrDdc* ddcEngine = ddc()) {
            ddcEngine->setAudioGain(gainPercent);
            m_receiveGain = std::clamp(gainPercent, 0, 100);
            SliceDelta delta;
            delta.audioGain = m_receiveGain;
            emit sliceChanged(sliceId, delta);
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
    if (!m_connected) { return; }
    auto desired = m_requested;
    desired.hardware.gainTenths = nearestGainTenths(m_tunerGainsTenths,
                                                   std::clamp(gainDb, -100, 100) * 10);
    if (requestCapture(desired)) {
        m_pendingPanId = panId.isEmpty() ? QStringLiteral("0xe1000000") : panId;
    }
}

void RtlSdrBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
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
    if (ns != QLatin1String("rtl") || !m_connected || !m_worker) {
        emit extensionError(requestId, tr("RTL-SDR is unavailable")); return;
    }
    if (verb == QLatin1String("gain.list")) {
        emit extensionResult(requestId, QVariant::fromValue(m_tunerGainsTenths)); return;
    }
    bool ok = false;
    const qint64 value = arg.toLongLong(&ok);
    auto desired = m_requested;
    if (ok && verb == QLatin1String("gain.set") && value >= -100 && value <= 100) {
        desired.hardware.gainTenths = nearestGainTenths(m_tunerGainsTenths, int(value) * 10);
    } else if (ok && verb == QLatin1String("ppm.set") && value >= -1000 && value <= 1000) {
        desired.hardware.ppm = int(value);
    } else if (ok && verb == QLatin1String("direct_sampling.set") && value >= 0 && value <= 2) {
        desired.hardware.directSampling = int(value);
        desired.automaticDirectSampling = false;
    } else if (ok && verb == QLatin1String("offset_tuning.set") && (value == 0 || value == 1)) {
        desired.hardware.offsetTuning = int(value);
    } else if (ok && verb == QLatin1String("sample_rate.set") && value > 0 && value <= UINT32_MAX) {
        desired.hardware.sampleRateHz = clampSampleRate(static_cast<std::uint32_t>(value));
    } else {
        emit extensionError(requestId, tr("Invalid RTL-SDR control or value")); return;
    }
    if (!requestCapture(desired, verb, requestId)) {
        emit extensionError(requestId, tr("Requested RTL-SDR capture does not fit or is invalid"));
    }
}

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
    if (m_connecting) { return state; }
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

bool RtlSdrBackend::requestCapture(const RtlCaptureTransaction::Desired& desired,
                                  const QString& extension, quint64 requestId)
{
    const auto submitted = m_capture.submit(desired);
    if (!submitted) {
        qWarning() << "RTL-SDR capture request refused by whole-set validation";
        return false;
    }
    QVector<quint64> superseded;
    const auto& old = m_requested.hardware;
    const auto& next = desired.hardware;
    for (auto it = m_pendingExtensionRequests.begin(); it != m_pendingExtensionRequests.end();) {
        const QString& key = it.key();
        const bool changed = key == extension
            || (key == QLatin1String("gain.set") && old.gainTenths != next.gainTenths)
            || (key == QLatin1String("ppm.set") && old.ppm != next.ppm)
            || (key == QLatin1String("sample_rate.set") && old.sampleRateHz != next.sampleRateHz)
            || (key == QLatin1String("offset_tuning.set") && old.offsetTuning != next.offsetTuning)
            || (key == QLatin1String("direct_sampling.set")
                && (old.directSampling != next.directSampling
                    || desired.automaticDirectSampling != m_requested.automaticDirectSampling));
        if (changed) {
            superseded.append(it.value().requestId);
            it = m_pendingExtensionRequests.erase(it);
        } else {
            it.value().token = submitted.token;
            ++it;
        }
    }
    if (!extension.isEmpty()) {
        m_pendingExtensionRequests.insert(extension, {requestId, submitted.token});
    }
    m_requested = desired;
    // Dispatch only; completion is serviced by the timer to avoid reentering
    // publication while an extension promise is still being attached.
    if (const auto work = m_capture.takeWork()) {
        if (!m_worker->submit(*work)) {
            qFatal("RTL-SDR transaction mailbox ownership violated");
        }
    }
    for (quint64 id : superseded) {
        emit extensionError(id, tr("RTL-SDR request superseded"));
    }
    return true;
}

bool RtlSdrBackend::acceptsFrame(quint64 session, quint64 revision) const
{
    return m_connected && m_published == RtlCaptureTransaction::Token{session, revision};
}

void RtlSdrBackend::serviceCapture()
{
    if (!m_worker) { return; }
    const QPointer<RtlSdrWorker> producer(m_worker.get());
    m_worker->serviceCancellation();
    if (const auto result = m_worker->takeResult()) {
        const auto completion = m_capture.complete(*result);
        if (completion == RtlCaptureTransaction::Completion::Invalidated) {
            finishExtensions(false);
            if (!producer || producer.data() != m_worker.get()) { return; }
            emit connectionError(tr("RTL-SDR capture lost: configuration or rollback could not be verified"));
            if (producer && producer.data() == m_worker.get()) { disconnectRadio(); }
            return;
        }
        if (completion == RtlCaptureTransaction::Completion::Published) {
            publishCapture();
            finishExtensions(true, result->token);
        } else if (completion == RtlCaptureTransaction::Completion::Failed && !m_capture.busy()) {
            const auto& state = *m_capture.confirmed();
            m_requested.hardware = state.hardware;
            m_requested.receivers = state.receivers;
            m_requested.automaticDirectSampling = state.automaticDirectSampling;
            finishExtensions(false);
            qWarning() << "RTL-SDR capture request failed; previous capture restored";
        }
    }
    if (m_worker) {
        if (const auto work = m_capture.takeWork()) {
            if (!m_worker->submit(*work)) { qFatal("RTL-SDR transaction mailbox ownership violated"); }
        }
    }
}

void RtlSdrBackend::publishCapture()
{
    const auto state = *m_capture.confirmed();
    m_published = state.token;
    m_panCenterHz = state.hardware.centerHz;
    m_sampleRateHz = state.hardware.sampleRateHz;
    m_directSampling = state.hardware.directSampling;
    m_ppmCorrection = state.hardware.ppm;
    m_panRfGainDb = qRound(state.hardware.gainTenths / 10.0);
    m_sliceFreqHz = state.receivers[0].passband.carrierHz;
    m_sliceMode = modeName(state.receivers[0].mode);
    m_sliceFilterLow = int(state.receivers[0].passband.filterLowHz);
    m_sliceFilterHigh = int(state.receivers[0].passband.filterHighHz);
    m_requested.hardware = state.hardware;
    m_requested.receivers = state.receivers;
    m_requested.automaticDirectSampling = state.automaticDirectSampling;
    const auto current = [this, token = state.token] {
        return acceptsFrame(token.session, token.revision);
    };
    if (m_connecting) {
        m_connecting = false;
        m_connected = true;
        emit connected();
        if (!current()) { return; }
        emitInitialState();
    } else {
        emit panCenterBandwidthChanged(m_pendingPanId, m_panCenterHz / 1e6, m_sampleRateHz / 1e6);
        if (!current()) { return; }
        emit panRfGainChanged(m_pendingPanId, m_panRfGainDb);
        if (!current()) { return; }
        SliceDelta delta;
        delta.frequency = m_sliceFreqHz / 1e6;
        delta.mode = m_sliceMode;
        delta.filterLow = m_sliceFilterLow;
        delta.filterHigh = m_sliceFilterHigh;
        emit sliceChanged(0, delta);
    }
    if (current()) { emit operatingStateChanged(); }
}

void RtlSdrBackend::finishExtensions(bool success, RtlCaptureTransaction::Token token)
{
    QHash<QString, PendingExtension> requests;
    for (auto it = m_pendingExtensionRequests.begin(); it != m_pendingExtensionRequests.end();) {
        if (!success || it.value().token == token) {
            requests.insert(it.key(), it.value());
            it = m_pendingExtensionRequests.erase(it);
        } else { ++it; }
    }
    // Snapshot before any signal can reenter the backend. Newly submitted
    // promises remain in the bounded map and cannot consume this completion.
    const auto confirmed = m_capture.confirmed();
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        if (!success || !confirmed || confirmed->token != token) {
            emit extensionError(it.value().requestId, tr("RTL-SDR capture transaction failed")); continue;
        }
        const auto& hardware = confirmed->hardware;
        qint64 value = 0;
        if (it.key() == QLatin1String("gain.set")) { value = qRound(hardware.gainTenths / 10.0); }
        else if (it.key() == QLatin1String("ppm.set")) { value = hardware.ppm; }
        else if (it.key() == QLatin1String("direct_sampling.set")) { value = hardware.directSampling; }
        else if (it.key() == QLatin1String("offset_tuning.set")) { value = hardware.offsetTuning; }
        else if (it.key() == QLatin1String("sample_rate.set")) { value = hardware.sampleRateHz; }
        emit extensionResult(it.value().requestId, value);
    }
}

void RtlSdrBackend::emitInitialState()
{
    const auto token = m_published;
    const auto current = [this, token] { return acceptsFrame(token.session, token.revision); };
    if (!current()) { return; }
    // Emit the signals the UI expects from a freshly-connected radio.
    // Mirrors what the Flex backend does with initial status echoes.
    RadioDelta rDelta;
    rDelta.model = m_modelName;
    rDelta.nickname = m_product;
    emit radioChanged(rDelta);
    if (!current()) { return; }

    const QString kPanId = QStringLiteral("0xe1000000");

    // Pan 0 FIRST — center frequency and bandwidth limits so PanadapterModel materialises
    emit panCenterBandwidthChanged(kPanId, m_panCenterHz / 1e6, m_sampleRateHz / 1e6);
    if (!current()) { return; }
    emit panBandwidthLimitsChanged(kPanId, 0.225001, 3.0);
    if (!current()) { return; }

    // Tuner RF gain info
    if (!m_tunerGainsTenths.isEmpty()) {
        const auto [minIt, maxIt] = std::minmax_element(m_tunerGainsTenths.cbegin(),
                                                        m_tunerGainsTenths.cend());
        emit panRfGainInfoChanged(kPanId, qFloor(*minIt / 10.0), qCeil(*maxIt / 10.0), 1);
    }

    if (!current()) { return; }
    // RF gain
    emit panRfGainChanged(kPanId, m_panRfGainDb);
    if (!current()) { return; }

    // Slice 0 — initial frequency, mode, and filters
    SliceDelta sDelta;
    sDelta.frequency = m_sliceFreqHz / 1e6;
    sDelta.mode = m_sliceMode;
    sDelta.filterLow = m_sliceFilterLow;
    sDelta.filterHigh = m_sliceFilterHigh;
    sDelta.audioGain = m_receiveGain;
    sDelta.audioMute = m_receiveMuted;
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
