#include "core/backends/rtl/RtlSdrBackend.h"

#include <QPointer>
#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/RadioStateMemory.h"

#include <QDebug>

#include <rtl-sdr.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR::rtl {

namespace {

constexpr double kMinTuneHz = 24'000.0;
constexpr double kMaxTuneHz = 1'766'000'000.0;

bool hasReceivingNarrowFm(const RtlCaptureTransaction::State& state)
{
    return std::ranges::any_of(state.receivers, [&state](const auto& receiver) {
        return (receiver.mode == RtlCaptureTransaction::Mode::Fm
                || receiver.mode == RtlCaptureTransaction::Mode::Fmn)
            && std::ranges::find(state.receivingIds, receiver.passband.stableId)
                != state.receivingIds.end();
    });
}

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
    // Receiver admission is an explicitly qualified profile. Settings-domain
    // ownership changes only after a successful accepted-state migration.
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
    c.panZoomModes = std::nullopt;      // no command plane, no per-pan zoom flags.
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
    c.canCreateSlices = m_receiverCapacity > 1;
    c.maxSlices = m_receiverCapacity;
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
    c.receiveFilterControl = ReceiveFilterControl{SliceFrequencyControl::Authority::Engine,
        {{QStringLiteral("FM"), -21600, -1, 1, 21600, 2, 43200},
         {QStringLiteral("FMN"), -21600, -1, 1, 21600, 2, 43200}}};
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receiveSquelchModel = ReceiveSquelchModel{
        {QStringLiteral("FM"), QStringLiteral("FMN")},
        RtlSquelchGate::kReferenceDb, RtlSquelchGate::kStepDb, QStringLiteral("dBFS/bin")};
    c.panSpanModel = PanSpanModel{false, false};
    if (m_lastPublished && hasReceivingNarrowFm(*m_lastPublished)) {
        c.receiveCapturePlacement = ReceiveCapturePlacement{
            qint64(RtlCaptureTransaction::kDcSeparationHz)};
    }
    c.receivePanCenterControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                       0, 1'766'000'000};
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
        m_viewport ? qFloor(m_viewport->minimumSpanHz) : 1757,
        m_viewport ? qCeil(m_viewport->maximumSpanHz) : 2'700'000};

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

    if (m_settingsActive) {
        c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RtlSlices
            | RadioCapabilities::ClientSettingsDomain::RfGain | RadioCapabilities::ClientSettingsDomain::Memories;
    }
    // Vendor extensions
    c.extensions["rtl"] = QVariantMap{{"serial", m_serial}, {"settingsVersion", 1}};
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

    verifyDeviceSettingsIdentity(m_serial);

    // All capture controls are requested together; only the worker touches
    // them, and only its confirmed result is exposed as connected state.
    m_requested = {};
    m_requested.hardware.centerHz = static_cast<std::uint32_t>(m_panCenterHz);
    m_requested.hardware.sampleRateHz = m_sampleRateHz;
    m_requested.hardware.ppm = m_ppmCorrection;
    m_requested.dcSuppression = m_dcSuppression;
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
    startCapture(std::make_unique<RtlSdrWorker>(m_device, nullptr, m_receiverCapacity));
}

void RtlSdrBackend::startCapture(std::unique_ptr<RtlSdrWorker> worker)
{
    m_worker = std::move(worker);
    m_diagnostics = {};
    m_capture.beginSession();
    m_published = {};
    m_lastPublished.reset();
    m_viewport.reset();
    m_pendingViewport = {};
    m_viewCenterRequestHz = m_requested.receivers.front().passband.carrierHz;
    m_viewSpanRequestHz = m_requested.hardware.sampleRateHz;
    m_monitors.fill({});
    m_connecting = true;
    m_pendingPanId = QStringLiteral("0xe1000000");
    m_receiveGain = 100;
    m_receiveMuted = false;
    const QPointer<RtlSdrWorker> producer(m_worker.get());
    connect(m_worker.get(), &RtlSdrWorker::spectrumFrameReady, this,
        [this, producer](quint64 session, quint64 revision, int panId, const QByteArray& frame) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                const QByteArray cropped = viewportFrame(frame);
                if (!cropped.isEmpty()) { emit spectrumFrameReady(panId, cropped); }
            }
        });
    connect(m_worker.get(), &RtlSdrWorker::waterfallRowReady, this,
        [this, producer](quint64 session, quint64 revision, int panId, const QByteArray& frame) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                const QByteArray cropped = viewportFrame(frame);
                if (!cropped.isEmpty()) { emit waterfallRowReady(panId, cropped); }
            }
        });
    connect(m_worker.get(), &RtlSdrWorker::audioFrameReady, this,
        [this, producer](quint64 session, quint64 revision, const QByteArray& pcm, const QByteArray& preMonitor) {
            if (producer && producer.data() == m_worker.get() && acceptsFrame(session, revision)) {
                publishLegacyPcm(pcm, preMonitor);
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
    retireNativeAudio();
    m_captureTimer.stop();
    m_capture.endSession();
    m_published = {};
    m_pendingViewport = {};
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

IRadioBackend::HealthSnapshot RtlSdrBackend::healthSnapshot() const
{
    if (!m_connected) { return {}; }
    HealthSnapshot snapshot;
    snapshot.order = {QStringLiteral("rtlQueueDrops"), QStringLiteral("rtlMixerLateFrames"),
        QStringLiteral("rtlMixerRejectedBlocks"), QStringLiteral("rtlMixerConfigurationFailures")};
    snapshot.sections.insert(snapshot.order.front(), tr("RTL receive pipeline (since connect)"));
    snapshot.labels = {{snapshot.order[0], tr("Audio queue dropped packets")},
        {snapshot.order[1], tr("Mixer missing receiver frames at deadline")},
        {snapshot.order[2], tr("Mixer rejected audio blocks")},
        {snapshot.order[3], tr("Mixer configuration failures")}};
    // Legacy-only sessions have not observed this pipeline. Missing values
    // report "not reported", never a fabricated successful zero measurement.
    if (m_diagnostics.observed) {
        const std::array<std::uint64_t, 4> counters{m_diagnostics.droppedPackets,
            m_diagnostics.mixerLateFrames, m_diagnostics.mixerRejectedBlocks,
            m_diagnostics.mixerConfigurationFailures};
        for (int i = 0; i < snapshot.order.size(); ++i) {
            snapshot.values.insert(snapshot.order[i], QVariant::fromValue<qulonglong>(counters[i]));
        }
    }
    if (m_lastPublished) {
        const auto& capture = m_lastPublished->capture;
        const QStringList keys{QStringLiteral("rtlCaptureCenterHz"), QStringLiteral("rtlCaptureRateHz"),
            QStringLiteral("rtlCaptureLowHz"), QStringLiteral("rtlCaptureHighHz"),
            QStringLiteral("rtlCaptureDcClear"), QStringLiteral("rtlCaptureRequest")};
        snapshot.sections.insert(keys.front(), tr("RTL accepted capture"));
        snapshot.order.append(keys);
        snapshot.labels.insert(keys[0], tr("Capture center / converter DC (Hz)"));
        snapshot.labels.insert(keys[1], tr("Captured IQ sample rate (Hz)"));
        snapshot.labels.insert(keys[2], tr("Usable capture low edge (Hz)"));
        snapshot.labels.insert(keys[3], tr("Usable capture high edge (Hz)"));
        snapshot.labels.insert(keys[4], tr("FM receivers clear of converter DC"));
        snapshot.labels.insert(keys[5], tr("Last capture request"));
        snapshot.values.insert(keys[0], capture.centerHz);
        snapshot.values.insert(keys[1], capture.achievedSampleRateHz);
        snapshot.values.insert(keys[2], std::max(0.0, capture.centerHz - capture.usableLeftHz));
        snapshot.values.insert(keys[3], capture.centerHz + capture.usableRightHz);
        snapshot.values.insert(keys[4], hasReceivingNarrowFm(*m_lastPublished)
            ? QVariant(RtlCaptureTransaction::dcClear(*m_lastPublished))
            : QVariant(tr("No receiving FM or FM-N slice")));
        snapshot.values.insert(keys[5], m_captureStatus);
    }
    return snapshot;
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — slice control
// ──────────────────────────────────────────────────────────────────────────────

bool RtlSdrBackend::createSlice(const QString& panId, double frequencyHz)
{
    if (!m_connected || !m_capture.confirmed() || !std::isfinite(frequencyHz) || m_capture.busy()
        || (panId != QStringLiteral("0xe1000000") && !panId.isEmpty())
        || m_requested.receivers.size() >= static_cast<std::size_t>(m_receiverCapacity)
        || std::ranges::any_of(m_requested.receivers, [](const auto& receiver) { return receiver.mode != RtlCaptureTransaction::Mode::Fm && receiver.mode != RtlCaptureTransaction::Mode::Fmn; })) { return false; }
    auto desired = m_requested;
    for (int id = 0; id < 8; ++id) {
        if (std::ranges::any_of(desired.receivers, [id](const auto& value) { return value.passband.stableId == id; })) { continue; }
        desired.receivers.push_back({{id, frequencyHz, -8000, 8000, 0, 3000, 3000}, RtlCaptureTransaction::Mode::Fm});
        // Creation must fit the accepted capture; adding a receiver cannot
        // recenter the tuner or displace an existing sibling.
        const auto& actual = m_capture.confirmed()->capture;
        const auto& passband = desired.receivers.back().passband;
        const SharedCapturePolicy::CenterDomain fixed{actual.centerHz, actual.centerHz, actual.centerHz, 1};
        const auto fit = SharedCapturePolicy::restoreFixedCapture(actual, std::span(&passband, 1), std::span(&fixed, 1), {8, 1});
        if (fit.accepted.empty()) { return false; }
        return requestCapture(desired);
    }
    return false;
}
bool RtlSdrBackend::removeSlice(int sliceId)
{
    if (!m_connected || m_capture.busy() || m_requested.receivers.size() <= 1) { return false; }
    auto desired = m_requested;
    const auto count = std::erase_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    return count != 0 && requestCapture(desired);
}

bool RtlSdrBackend::hasAcceptedSlice(int sliceId) const
{
    return m_connected && m_capture.confirmed() && m_lastPublished
        && std::ranges::any_of(m_lastPublished->receivers, [sliceId](const auto& receiver) {
            return receiver.passband.stableId == sliceId;
        });
}

void RtlSdrBackend::setSliceFrequency(int sliceId, double hz)
{
    requestReceiveTune(sliceId, hz, ReceiveTuneView::Preserve);
}

bool RtlSdrBackend::requestReceiveTune(int sliceId, double hz, ReceiveTuneView view)
{
    if (!hasAcceptedSlice(sliceId) || !std::isfinite(hz) || hz < kMinTuneHz || hz > kMaxTuneHz
        || view < ReceiveTuneView::Preserve || view > ReceiveTuneView::Center) { return false; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return false; }
    const bool outsideView = !m_viewport || hz < m_viewport->centerHz - m_viewport->spanHz / 2
        || hz > m_viewport->centerHz + m_viewport->spanHz / 2;
    const bool centerView = view == ReceiveTuneView::Center
        || (view == ReceiveTuneView::Reveal && outsideView);
    receiver->passband.carrierHz = hz;
    desired.automaticDirectSampling = true;
    desired.followReceiverId = sliceId;
    if (centerView) {
        desired.centeredView = RtlCaptureTransaction::Desired::CenteredView{hz, m_viewSpanRequestHz};
    } else {
        desired.centeredView.reset();
    }
    if (!requestCapture(desired) || !m_connected) { return false; }
    if (centerView) {
        m_viewCenterRequestHz = hz;
    } else if (m_viewport) {
        // A later tune supersedes an unadopted centering request together with
        // its receiver frequency. Preserve refers to the accepted view.
        m_viewCenterRequestHz = m_viewport->centerHz;
    }
    requestViewport();
    return true;
}

bool RtlSdrBackend::recenterReceiveCapture(const QString& panId)
{
    if (!m_connected || !m_lastPublished
        || (!panId.isEmpty() && panId != QLatin1String("0xe1000000"))) { return false; }
    const auto activeFm = std::ranges::find_if(m_lastPublished->receivers, [this](const auto& receiver) {
        return (receiver.mode == RtlCaptureTransaction::Mode::Fm
                || receiver.mode == RtlCaptureTransaction::Mode::Fmn)
            && std::ranges::find(m_lastPublished->receivingIds, receiver.passband.stableId)
                != m_lastPublished->receivingIds.end();
    });
    if (m_capture.busy() || activeFm == m_lastPublished->receivers.end()) {
        emit configurationWarning(tr("Capture DC placement requires an idle FM or FM-N receiver."));
        return false;
    }
    auto desired = m_requested;
    desired.followReceiverId = activeFm->passband.stableId;
    desired.avoidDc = true;
    return requestCapture(desired);
}

void RtlSdrBackend::setSliceMode(int sliceId, const QString& mode)
{
    const QString canonical = mode.trimmed().toUpper();
    if (!hasAcceptedSlice(sliceId) || !isKnownMode(canonical)) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return; }
    const auto modeValue = captureMode(canonical);
    const bool narrowFm = modeValue == RtlCaptureTransaction::Mode::Fm || modeValue == RtlCaptureTransaction::Mode::Fmn;
    if (!narrowFm && desired.receivers.size() != 1) { return; }
    if (narrowFm && (receiver->passband.filterLowHz < -21600 || receiver->passband.filterHighHz > 21600
        || receiver->passband.filterLowHz >= 0 || receiver->passband.filterHighHz <= 0)) {
        // The existing wide/sideband passband cannot describe the FM graph.
        // Mode transition selects the new receiver's documented 16 kHz FM
        // passband; restores and ordinary filter requests never resize it.
        receiver->passband.filterLowHz = -8000; receiver->passband.filterHighHz = 8000;
    }
    receiver->mode = modeValue;
    desired.followReceiverId = sliceId;
    desired.avoidDc = narrowFm;
    if (!narrowFm) { receiver->squelchEnabled = false; }
    receiver->passband.guardLowHz = narrowFm ? 3000 : 0;
    receiver->passband.guardHighHz = narrowFm ? 3000 : 0;
    requestCapture(desired);
}

void RtlSdrBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (!hasAcceptedSlice(sliceId) || lowHz >= highHz || lowHz < -100'000 || highHz > 100'000) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return; }
    if (receiver->mode == RtlCaptureTransaction::Mode::Wfm) {
        // Legacy WFM does not consume these edges. Do not acknowledge or save
        // a cosmetic filter change while its qualified DSP path is deferred.
        return;
    }
    if ((receiver->mode == RtlCaptureTransaction::Mode::Fm
        || receiver->mode == RtlCaptureTransaction::Mode::Fmn)
        && (lowHz < -21600 || highHz > 21600 || lowHz >= 0 || highHz <= 0)) { return; }
    receiver->passband.filterLowHz = lowHz;
    receiver->passband.filterHighHz = highHz;
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

void RtlSdrBackend::setSliceSquelch(int sliceId, bool enabled, int level)
{
    if (!hasAcceptedSlice(sliceId) || level < 0 || level > 100) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers,
        [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()
        || (receiver->mode != RtlCaptureTransaction::Mode::Fm
            && receiver->mode != RtlCaptureTransaction::Mode::Fmn)) { return; }
    if (receiver->squelchEnabled == enabled && receiver->squelchLevel == level) { return; }
    receiver->squelchEnabled = enabled; receiver->squelchLevel = level;
    requestCapture(desired);
}

// ──────────────────────────────────────────────────────────────────────────────
// IRadioBackend — pan control
// ──────────────────────────────────────────────────────────────────────────────

void RtlSdrBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    if (!m_connected || !std::isfinite(hz)
        || (!panId.isEmpty() && panId != QLatin1String("0xe1000000"))) { return; }
    // A zoom pair comes from the still-accepted axis. Keep an unadopted
    // deliberate Center reveal at its requested RF while its span coalesces.
    m_viewCenterRequestHz = intent == PanCenterIntent::Range && m_capture.busy()
        && m_requested.centeredView ? m_requested.centeredView->centerHz : hz;
    requestViewport(intent == PanCenterIntent::Drag);
}

void RtlSdrBackend::setPanBandwidth(const QString& panId, double hz)
{
    if (!m_connected || !std::isfinite(hz) || hz <= 0
        || (!panId.isEmpty() && panId != QLatin1String("0xe1000000"))) { return; }
    const double previousSpanHz = m_viewSpanRequestHz;
    m_viewSpanRequestHz = hz;
    if (m_capture.busy() && m_requested.centeredView) {
        // The latest zoom supersedes the width used to place a pending typed
        // Center tune. Replan capture before publication so a full-width view
        // cannot clamp the accepted center away from the requested RF.
        auto desired = m_requested;
        desired.centeredView->spanHz = hz;
        if (!requestCapture(desired)) {
            m_viewSpanRequestHz = previousSpanHz;
            return;
        }
        if (!m_connected) { return; }
    }
    requestViewport();
}

void RtlSdrBackend::requestViewport(bool followDrag)
{
    if (followDrag && m_capture.confirmed()) {
        const auto center = RtlViewport::captureCenterFor(m_capture.confirmed()->capture,
            RtlSdrDdc::kSpectrumBinCount, m_viewCenterRequestHz, m_viewSpanRequestHz);
        if (center) {
            const auto hardwareCenter = static_cast<std::uint32_t>(std::llround(
                std::clamp(*center, kMinTuneHz, kMaxTuneHz)));
            if (hardwareCenter != m_requested.hardware.centerHz
                || m_requested.followReceiverId.has_value()) {
                auto desired = m_requested;
                desired.hardware.centerHz = hardwareCenter;
                desired.followReceiverId.reset();
                desired.centeredView.reset();
                if (!requestCapture(desired)) {
                    if (m_viewport) {
                        m_viewCenterRequestHz = m_viewport->centerHz;
                        m_viewSpanRequestHz = m_viewport->spanHz;
                    }
                    return;
                }
            }
        }
    }
    if (m_capture.busy()) {
        // Typed tuning sends its slice intent first. Its display intent must
        // wait for that capture's adoption, including coalescing and rollback.
        m_pendingViewport = m_capture.requested();
        return;
    }
    publishViewport();
}

void RtlSdrBackend::publishViewport()
{
    if (!m_connected || !m_capture.confirmed()) { return; }
    const auto viewport = RtlViewport::fit(m_capture.confirmed()->capture,
        RtlSdrDdc::kSpectrumBinCount, m_viewCenterRequestHz, m_viewSpanRequestHz);
    if (!viewport) { return; }
    m_viewport = viewport;
    emit panCenterBandwidthChanged(QStringLiteral("0xe1000000"),
                                    viewport->centerHz / 1e6, viewport->spanHz / 1e6);
}

QByteArray RtlSdrBackend::viewportFrame(const QByteArray& frame) const
{
    if (!m_viewport || frame.size() != m_viewport->sourceBinCount * int(sizeof(float))) { return {}; }
    return frame.sliced(m_viewport->firstBin * int(sizeof(float)),
                         m_viewport->binCount * int(sizeof(float)));
}

void RtlSdrBackend::setPanFrameRate(const QString& panId, int fps)
{
    Q_UNUSED(panId);
    if (RtlSdrDdc* ddcEngine = ddc()) {
        ddcEngine->setSpectrumRateFps(fps);
    }
}

void RtlSdrBackend::updateMonitor(int sliceId)
{
    const auto& monitor = m_monitors[sliceId];
    m_worker->setMonitor(sliceId, monitor.gain, monitor.pan, monitor.mute);
    if (m_lastPublished && m_lastPublished->receivers.front().passband.stableId == sliceId) {
        ddc()->setAudioGain(monitor.gain); ddc()->setAudioPan(monitor.pan); ddc()->setAudioMute(monitor.mute);
        m_receiveGain = monitor.gain; m_receiveMuted = monitor.mute;
    }
}
void RtlSdrBackend::setSliceAudioMute(int sliceId, bool mute)
{
    if (!hasAcceptedSlice(sliceId)) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return; }
    receiver->audioMute = mute; requestCapture(desired);
}
void RtlSdrBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if (!hasAcceptedSlice(sliceId)) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return; }
    receiver->audioGain = std::clamp(gainPercent, 0, 100); requestCapture(desired);
}
void RtlSdrBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    if (!hasAcceptedSlice(sliceId)) { return; }
    auto desired = m_requested;
    const auto receiver = std::ranges::find_if(desired.receivers, [sliceId](const auto& value) { return value.passband.stableId == sliceId; });
    if (receiver == desired.receivers.end()) { return; }
    receiver->audioPan = std::clamp(panPercent, 0, 100); requestCapture(desired);
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
    if (verb == QLatin1String("settings.get")) {
        emit extensionResult(requestId, deviceSettingsStatus()); return;
    }
    bool ok = false;
    const qint64 value = arg.toLongLong(&ok);
    const int type = arg.metaType().id();
    const bool numeric = type == QMetaType::Int || type == QMetaType::UInt
        || type == QMetaType::LongLong || type == QMetaType::ULongLong
        || type == QMetaType::Double || type == QMetaType::Float;
    const double number = arg.toDouble();
    const bool integerPpm = numeric && std::isfinite(number) && std::trunc(number) == number
        && number >= RtlDeviceSettings::kMinPpm && number <= RtlDeviceSettings::kMaxPpm;
    auto desired = m_requested;
    if (ok && verb == QLatin1String("gain.set") && value >= -100 && value <= 100) {
        desired.hardware.gainTenths = nearestGainTenths(m_tunerGainsTenths, int(value) * 10);
    } else if (verb == QLatin1String("ppm.set") && integerPpm) {
        desired.hardware.ppm = int(number);
    } else if (verb == QLatin1String("dc_suppression.set") && type == QMetaType::Bool) {
        desired.dcSuppression = arg.toBool();
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

void RtlSdrBackend::configureSettingsScope(const RadioSettingsScope& scope, const RadioSerialIdentity& identity)
{
    m_settingsScope = scope; m_settingsIdentity = identity;
    m_lastPublished.reset();
    m_settingsActive = false; m_restoreAttempted = false; m_restoreToken = {};
    m_removedSettings.clear(); m_omittedSettings.clear();
    m_savedSettings = RtlSliceSettings(scope).load();
    m_deviceSettingsAllowed = scope.family() == QLatin1String("rtl")
        && !identity.reportedSerial.trimmed().isEmpty()
        && identity.reportedSerial.trimmed() == scope.radioId();
    m_savedDeviceSettings = m_deviceSettingsAllowed ? RtlDeviceSettings(scope).load()
        : RtlDeviceSettings::ReadResult{RtlDeviceSettings::ReadStatus::Refused, {},
            tr("Session only: no matching reported device serial.")};
    m_deviceSettingsSaved = false;
    m_deviceSettingsReason = m_savedDeviceSettings.reason;
}
QVector<RtlSliceSettings::Slice> RtlSdrBackend::acceptedSettings() const
{
    QVector<RtlSliceSettings::Slice> output;
    if (!m_lastPublished) { return output; }
    for (const auto& receiver : m_lastPublished->receivers) {
        const int id = receiver.passband.stableId;
        if (m_omittedSettings.contains(id)) { continue; }
        RtlSliceSettings::Slice slice;
        slice.id = id; slice.frequencyHz = receiver.passband.carrierHz; slice.mode = modeName(receiver.mode);
        slice.filterLowHz = receiver.passband.filterLowHz; slice.filterHighHz = receiver.passband.filterHighHz;
        if (m_savedSettings.document.slices.contains(id)) {
            const auto& saved = m_savedSettings.document.slices[id];
            slice.agcMode = saved.agcMode; slice.agcThreshold = saved.agcThreshold;
        }
        slice.audioGain = m_monitors[id].gain; slice.audioMute = m_monitors[id].mute; slice.audioPan = m_monitors[id].pan;
        slice.squelchEnabled = receiver.squelchEnabled; slice.squelchLevel = receiver.squelchLevel;
        output.append(slice);
    }
    return output;
}
std::optional<bool> RtlSdrBackend::storeOperatingState(const RadioSettingsScope& scope,
    const RestoredRadioState& state)
{
    // A failed/canceled initial capture has no accepted state to save. Keep
    // even the forced disconnect flush handled: generic fallback would replace
    // the operator's saved document with speculative defaults/restored intent.
    if (!m_lastPublished && m_settingsScope.isValid()) { return false; }
    if (!m_settingsActive) { return std::nullopt; }
    if (scope.family() != m_settingsScope.family() || scope.radioId() != m_settingsScope.radioId()
        || !m_lastPublished) { return false; }
    const bool gain = RadioStateMemory::storeRtlRfGainPreservingLegacy(scope, state);
    if (m_restoreToken.revision != 0) { return gain; }
    const bool stored = RtlSliceSettings(scope).patch(m_lastPublished->capture.centerHz,
        m_lastPublished->capture.achievedSampleRateHz, acceptedSettings(), m_removedSettings);
    if (stored) { m_removedSettings.clear(); }
    return gain && stored;
}
bool RtlSdrBackend::activateSettings()
{
    if (m_settingsActive || !m_settingsScope.hasRadioIdentity() || !m_lastPublished) { return false; }
    RtlSliceSettings settings(m_settingsScope);
    const auto migration = settings.migrateLegacy(m_settingsIdentity);
    if (migration == RtlSliceSettings::MigrationResult::Retry) { return false; }
    if (migration == RtlSliceSettings::MigrationResult::NoSource
        && !settings.patch(m_lastPublished->capture.centerHz,
            m_lastPublished->capture.achievedSampleRateHz, acceptedSettings())) { return false; }
    m_savedSettings = settings.load();
    if (m_savedSettings.status != RtlSliceSettings::ReadStatus::Ready) { return false; }
    m_settingsActive = true;
    return true;
}
void RtlSdrBackend::restoreAcceptedSlices()
{
    if (!m_settingsActive || m_restoreAttempted || !m_connected || m_capture.busy()) { return; }
    m_restoreAttempted = true;
    const auto& saved = m_savedSettings.document;
    if (saved.slices.isEmpty()) { return; }
    QSet<int> seen;
    auto desired = m_requested;
    desired.receivers.clear();
    for (const auto& slice : saved.slices) {
        const bool wide = slice.mode != QLatin1String("FM") && slice.mode != QLatin1String("FMN");
        const SharedCapturePolicy::SliceDescriptor descriptor{slice.id, slice.frequencyHz,
            slice.filterLowHz, slice.filterHighHz, 0, wide ? 0.0 : 3000.0,
            wide ? 0.0 : 3000.0};
        m_omittedSettings.insert(slice.id);
        if (slice.id < 0 || slice.id >= 8 || seen.contains(slice.id)
            || !SharedCapturePolicy::occupiedInterval(descriptor).interval
            || !isKnownMode(slice.mode)
            || desired.receivers.size() >= static_cast<std::size_t>(m_receiverCapacity)) { continue; }
        seen.insert(slice.id);
        if ((!wide && (slice.filterLowHz < -21600 || slice.filterHighHz > 21600
            || slice.filterLowHz >= 0 || slice.filterHighHz <= 0))
            || (wide && !desired.receivers.empty())
            || (!desired.receivers.empty() && desired.receivers.front().mode != RtlCaptureTransaction::Mode::Fm
                && desired.receivers.front().mode != RtlCaptureTransaction::Mode::Fmn)) { continue; }
        desired.receivers.push_back({descriptor, captureMode(slice.mode), slice.audioGain, slice.audioPan,
            slice.audioMute, !wide && slice.squelchEnabled, slice.squelchLevel});
    }
    // Preserve a valid configured receiver even when it is parked. The
    // transaction derives DSP membership from the confirmed capture; a
    // reconnect never silently retunes or loses an out-of-capture station.
    if (desired.receivers.empty()) { return; }
    if (requestCapture(desired)) { m_restoreToken = m_capture.requested(); }
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
    m_ppmCorrection = m_savedDeviceSettings.values.ppm;
    m_dcSuppression = m_savedDeviceSettings.values.dcSuppression;
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
    }    if (m_savedSettings.status == RtlSliceSettings::ReadStatus::Ready) {
        const auto& saved = m_savedSettings.document;
        m_panCenterHz = 95'200'000; m_sampleRateHz = 2'400'000;
        // Schema bounds deliberately exceed today's hardware. Reject before
        // narrowing a floating-point field to an integer; never silently pick
        // a nearby rate for a saved capture or overflow a future-format value.
        if (saved.captureCenterHz >= kMinTuneHz && saved.captureCenterHz <= kMaxTuneHz
            && saved.captureCenterHz == std::floor(saved.captureCenterHz)
            && saved.sampleRateHz >= 225001 && saved.sampleRateHz <= 3000000
            && saved.sampleRateHz == std::floor(saved.sampleRateHz)
            && clampSampleRate(static_cast<std::uint32_t>(saved.sampleRateHz)) == saved.sampleRateHz) {
            m_panCenterHz = saved.captureCenterHz;
            m_sampleRateHz = static_cast<std::uint32_t>(saved.sampleRateHz);
        }
        m_sliceFreqHz = m_panCenterHz;
        // Use a valid initial receiver before fitting the saved set. At VLF a
        // 200 kHz wide initial receiver would extend through negative RF.
        const bool narrowInitial = m_panCenterHz < 100000;
        m_sliceMode = narrowInitial ? QStringLiteral("FM") : QStringLiteral("WFM");
        m_sliceFilterLow = narrowInitial ? -8000 : -100000;
        m_sliceFilterHigh = narrowInitial ? 8000 : 100000;
    }

}

RestoredRadioState RtlSdrBackend::currentOperatingState() const
{
    RestoredRadioState state;
    if (m_connecting) { return state; }
    state.rfFrequencyHz = m_sliceFreqHz;
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
        qWarning() << "RTL-SDR capture request refused by validated placement";
        m_captureStatus = desired.avoidDc
            ? tr("Refused: no legal DC-clear capture fits the selected receiver passband.")
            : tr("Refused: no legal capture fits the requested placement or receiver passband.");
        if (m_connected) { emit configurationWarning(m_captureStatus); }
        return false;
    }
    m_captureStatus = tr("Preparing; displayed capture remains the last accepted state.");
    QVector<quint64> superseded;
    const auto& old = m_requested.hardware;
    const auto& next = desired.hardware;
    for (auto it = m_pendingExtensionRequests.begin(); it != m_pendingExtensionRequests.end();) {
        const QString& key = it.key();
        const bool changed = key == extension
            || (key == QLatin1String("gain.set") && old.gainTenths != next.gainTenths)
            || (key == QLatin1String("ppm.set") && old.ppm != next.ppm)
            || (key == QLatin1String("dc_suppression.set")
                && m_requested.dcSuppression != desired.dcSuppression)
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
    m_requested.avoidDc = false;
    if (m_pendingViewport.revision) { m_pendingViewport = submitted.token; }
    // Dispatch only; completion is serviced by the timer to avoid reentering
    // publication while an extension promise is still being attached.
    if (const auto work = m_capture.takeWork()) {
        if (!m_worker->submit(*work)) {
            qFatal("RTL-SDR transaction mailbox ownership violated");
        }
    }
    emit extensionStatus(QStringLiteral("rtl"), QStringLiteral("settings"), deviceSettingsStatus());
    for (quint64 id : superseded) {
        if (m_capture.requested().session != submitted.token.session || !m_worker) { return true; }
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
    m_diagnostics = m_worker->diagnostics();
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
            m_requested.dcSuppression = state.dcSuppression;
            m_requested.followReceiverId.reset();
            m_requested.centeredView.reset();
            m_pendingViewport = {};
            if (m_viewport) {
                m_viewCenterRequestHz = m_viewport->centerHz;
                m_viewSpanRequestHz = m_viewport->spanHz;
            }
            if (m_restoreToken == result->token) { m_restoreToken = {}; }
            finishExtensions(false);
            qWarning() << "RTL-SDR capture request failed; previous capture restored";
            if (producer && producer.data() == m_worker.get()) {
                m_captureStatus = tr("Failed: previous capture and receiver state restored.");
                emit configurationWarning(m_captureStatus);
            }
        }
    }
    if (m_worker) {
        drainAudio();
        if (!m_capture.busy() && m_worker->needsRepair()) { requestCapture(m_requested); }
        if (const auto work = m_capture.takeWork()) {
            if (!m_worker->submit(*work)) { qFatal("RTL-SDR transaction mailbox ownership violated"); }
        }
    }
}

void RtlSdrBackend::retireNativeAudio()
{
    m_speakerAudio = {};
    for (auto& stream : m_sliceAudio) { stream = {}; }
}
void RtlSdrBackend::publishLegacyPcm(const QByteArray& pcm, const QByteArray& preMonitor)
{
    if (!m_lastPublished || m_lastPublished->receivers.size() != 1
        || m_lastPublished->receivingIds.size() != 1
        || m_lastPublished->receivingIds.front()
            != m_lastPublished->receivers.front().passband.stableId
        || m_lastPublished->receivers.front().mode == RtlCaptureTransaction::Mode::Fm
        || m_lastPublished->receivers.front().mode == RtlCaptureTransaction::Mode::Fmn) { return; }
    const int id = m_lastPublished->receivers.front().passband.stableId;
    const auto token = m_published;
    for (int slot : {-1, id}) {
        if (!acceptsFrame(token.session, token.revision)) { return; }
        NativeAudio& output = slot < 0 ? m_speakerAudio : m_sliceAudio[slot];
        if (!output.producer || output.captureEpoch != m_published.revision) {
            output = {}; output.producer = std::make_unique<PcmProducer>();
            output.producer->start(slot < 0 ? PcmPurpose::Speaker : PcmPurpose::Slice,
                slot, {}, m_published.session, 1);
            output.captureEpoch = m_published.revision;
        }
        if (const auto frame = output.producer->legacyStereo24(slot < 0 ? pcm : preMonitor)) {
            if (slot < 0) { emit audioFrameReady(*frame); }
            else { emit sliceAudioFrameReady(slot, *frame); }
        }
    }
}
void RtlSdrBackend::drainAudio()
{
    const QPointer<RtlSdrWorker> worker(m_worker.get());
    RtlReceivePipeline::Packet packet;
    for (int count = 0; count < 128 && worker && worker.data() == m_worker.get()
         && worker->takeAudio(packet); ++count) {
        if (!acceptsFrame(packet.token.session, packet.token.revision)
            || packet.frames == 0 || packet.frames > 1024 || packet.slot < -1 || packet.slot >= 8) { continue; }
        if (!m_lastPublished || m_lastPublished->receivingIds.empty()
            || (packet.slot >= 0 && std::ranges::find(m_lastPublished->receivingIds, packet.slot)
                == m_lastPublished->receivingIds.end())) { continue; }
        NativeAudio& output = packet.slot < 0 ? m_speakerAudio : m_sliceAudio[packet.slot];
        if (!output.producer
            || (packet.slot < 0 && output.captureEpoch != packet.captureEpoch)
            || output.instance != packet.instance || output.receiverEpoch != packet.receiverEpoch) {
            output = {};
            output.producer = std::make_unique<PcmProducer>();
            output.producer->start(packet.slot < 0 ? PcmPurpose::Speaker : PcmPurpose::Slice,
                packet.slot, {48000, PcmLayout::Stereo}, packet.token.session, packet.instance);
            output.captureEpoch = packet.captureEpoch; output.instance = packet.instance;
            output.receiverEpoch = packet.receiverEpoch;
        }
        QVector<float> samples(static_cast<qsizetype>(2 * packet.frames));
        std::copy_n(packet.samples.begin(), samples.size(), samples.begin());
        const auto frame = output.producer->produce(std::move(samples), packet.firstSample,
            packet.discontinuity || packet.firstSample != output.nextSample);
        output.nextSample = packet.firstSample + packet.frames;
        if (frame) {
            if (packet.slot < 0) { emit audioFrameReady(*frame); }
            else { emit sliceAudioFrameReady(packet.slot, *frame); }
        }
    }
}
void RtlSdrBackend::emitSliceState(const RtlCaptureTransaction::Receiver& receiver)
{
    const int id = receiver.passband.stableId;
    SliceDelta delta;
    delta.frequency = receiver.passband.carrierHz / 1e6;
    delta.mode = modeName(receiver.mode); delta.filterLow = static_cast<int>(receiver.passband.filterLowHz);
    delta.filterHigh = static_cast<int>(receiver.passband.filterHighHz);
    delta.audioGain = m_monitors[id].gain; delta.audioMute = m_monitors[id].mute; delta.audioPan = m_monitors[id].pan;
    delta.squelchOn = receiver.squelchEnabled; delta.squelchLevel = receiver.squelchLevel;
    delta.panId = QStringLiteral("0xe1000000"); delta.active = id == m_requested.receivers.front().passband.stableId;
    delta.inCapture = m_lastPublished && std::ranges::find(m_lastPublished->receivingIds, id)
        != m_lastPublished->receivingIds.end();
    delta.modeList = capabilities().receiveModeControl->modes;
    emit sliceChanged(id, delta);
}

void RtlSdrBackend::publishCapture()
{
    const auto state = *m_capture.confirmed();
    if (m_lastPublished) {
        if (m_lastPublished->hardware != state.hardware
            || m_lastPublished->dcSuppression != state.dcSuppression) {
            retireNativeAudio();
        } else if (m_lastPublished->receivingIds != state.receivingIds) {
            // The mixed stream changes epoch, but an unchanged FM sibling's
            // per-slice producer and decoder tap remain valid.
            m_speakerAudio = {};
            for (const auto& previous : m_lastPublished->receivers) {
                const int id = previous.passband.stableId;
                if (std::ranges::find(state.receivingIds, id) == state.receivingIds.end()) {
                    m_sliceAudio[id] = {};
                }
            }
        }
        for (const auto& previous : m_lastPublished->receivers) {
            const int id = previous.passband.stableId;
            const auto current = std::ranges::find_if(state.receivers, [id](const auto& value) { return value.passband.stableId == id; });
            if (current == state.receivers.end() || current->passband != previous.passband || current->mode != previous.mode) { m_sliceAudio[id] = {}; }
        }
        if (std::ranges::any_of(state.receivers, [&state](const auto& value) {
                return std::ranges::find(state.receivingIds, value.passband.stableId)
                    != state.receivingIds.end()
                    && value.mode != RtlCaptureTransaction::Mode::Fm
                    && value.mode != RtlCaptureTransaction::Mode::Fmn;
            })) { retireNativeAudio(); }
        else { retirePcmStreams(); }
    }
    const auto prior = m_lastPublished;
    const bool restoring = m_restoreToken == state.token;
    if (restoring) {
        for (const auto& receiver : state.receivers) {
            const int id = receiver.passband.stableId;
            const auto& saved = m_savedSettings.document.slices[id];
            m_monitors[id] = {saved.audioGain, saved.audioPan, saved.audioMute};
            updateMonitor(id); m_omittedSettings.remove(id);
        }
        m_restoreToken = {};
    } else if (prior) {
        if (m_restoreToken.revision && state.token.revision > m_restoreToken.revision) { m_restoreToken = {}; }
        for (const auto& receiver : state.receivers) {
            m_removedSettings.removeAll(receiver.passband.stableId);
            const auto previous = std::ranges::find_if(prior->receivers, [&](const auto& value) { return value.passband.stableId == receiver.passband.stableId; });
            if (previous == prior->receivers.end() || *previous != receiver) { m_omittedSettings.remove(receiver.passband.stableId); }
        }
        for (const auto& receiver : prior->receivers) {
            const int id = receiver.passband.stableId;
            if (std::ranges::none_of(state.receivers, [id](const auto& value) { return value.passband.stableId == id; })) {
                if (!m_removedSettings.contains(id)) { m_removedSettings.append(id); }
                m_savedSettings.document.slices.remove(id);
                m_omittedSettings.remove(id);
            }
        }
    }
    m_lastPublished = state;
    for (const auto& receiver : state.receivers) {
        m_monitors[receiver.passband.stableId] = {receiver.audioGain, receiver.audioPan, receiver.audioMute};
        updateMonitor(receiver.passband.stableId);
    }
    m_published = state.token;
    m_panCenterHz = state.hardware.centerHz;
    m_sampleRateHz = state.hardware.sampleRateHz;
    m_directSampling = state.hardware.directSampling;
    m_ppmCorrection = state.hardware.ppm;
    m_dcSuppression = state.dcSuppression;
    if (!prior || prior->hardware.ppm != state.hardware.ppm
        || prior->dcSuppression != state.dcSuppression) {
        saveAcceptedDeviceSettings();
    }
    m_panRfGainDb = qRound(state.hardware.gainTenths / 10.0);
    m_sliceFreqHz = state.receivers[0].passband.carrierHz;
    m_sliceMode = modeName(state.receivers[0].mode);
    m_sliceFilterLow = int(state.receivers[0].passband.filterLowHz);
    m_sliceFilterHigh = int(state.receivers[0].passband.filterHighHz);
    m_requested.hardware = state.hardware;
    m_requested.receivers = state.receivers;
    m_requested.automaticDirectSampling = state.automaticDirectSampling;
    m_requested.dcSuppression = state.dcSuppression;
    m_requested.avoidDc = false;
    m_requested.followReceiverId.reset();
    m_requested.centeredView.reset();
    m_captureStatus = state.receivingIds.size() == state.receivers.size()
        ? tr("Accepted")
        : tr("Accepted; %1 configured slice(s) out of capture.")
              .arg(state.receivers.size() - state.receivingIds.size());
    if (!m_pendingViewport.revision && m_viewport) {
        m_viewCenterRequestHz = m_viewport->centerHz;
        m_viewSpanRequestHz = m_viewport->spanHz;
    }
    m_pendingViewport = {};
    m_viewport = RtlViewport::fit(state.capture, RtlSdrDdc::kSpectrumBinCount,
                                 m_viewCenterRequestHz, m_viewSpanRequestHz);
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
        publishViewport();
        if (!current()) { return; }
        if (m_viewport) {
            emit panBandwidthLimitsChanged(m_pendingPanId, m_viewport->minimumSpanHz / 1e6,
                                           m_viewport->maximumSpanHz / 1e6);
            if (!current()) { return; }
        }
        emit panRfGainChanged(m_pendingPanId, m_panRfGainDb);
        if (!current()) { return; }
        if (prior) {
            for (const auto& previous : prior->receivers) {
                const int id = previous.passband.stableId;
                if (std::ranges::none_of(state.receivers, [id](const auto& value) { return value.passband.stableId == id; })) {
                    m_monitors[id] = {}; emit sliceRemoved(id);
                    if (!current()) { return; }
                }
            }
        }
        for (const auto& receiver : state.receivers) {
            emitSliceState(receiver);
            if (!current()) { return; }
        }
    }
    if (current()) {
        const bool activated = activateSettings();
        if (activated || (prior && (prior->capture != state.capture
            || hasReceivingNarrowFm(*prior) != hasReceivingNarrowFm(state)))) {
            emit capabilitiesChanged();
        }
        if (!current()) { return; }
        restoreAcceptedSlices();
        if (current()) { emit operatingStateChanged(); }
        if (current() && hasReceivingNarrowFm(state) && !RtlCaptureTransaction::dcClear(state)
            && (!prior || RtlCaptureTransaction::dcClear(*prior))) {
            emit configurationWarning(tr("An FM receiver overlaps converter DC. Its frequency is preserved. "
                "Use Move capture away from DC in the spectrum menu; Radio Health shows the accepted capture."));
        }
    }
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
    const auto session = m_capture.requested().session;
    if (m_connected && m_worker && confirmed
        && (!token.revision || acceptsFrame(token.session, token.revision))) {
        emit extensionStatus(QStringLiteral("rtl"), QStringLiteral("settings"), deviceSettingsStatus());
    }
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        if (!m_connected || m_capture.requested().session != session) { return; }
        if (!success || !confirmed || confirmed->token != token) {
            emit extensionError(it.value().requestId, tr("RTL-SDR capture transaction failed")); continue;
        }
        if (it.key() == QLatin1String("dc_suppression.set")) {
            emit extensionResult(it.value().requestId, confirmed->dcSuppression); continue;
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

void RtlSdrBackend::verifyDeviceSettingsIdentity(const QString& serial)
{
    // Discovery identity is the sole scope owner. If enumeration changed,
    // refuse calibration inheritance rather than opening another writer here.
    if (serial.isEmpty() || serial != m_settingsScope.radioId()) {
        m_deviceSettingsAllowed = false;
        m_deviceSettingsSaved = false;
        m_deviceSettingsReason = tr("Session only: opened device serial does not match the settings identity.");
        m_ppmCorrection = 0;
        m_dcSuppression = false;
    }
}

void RtlSdrBackend::saveAcceptedDeviceSettings()
{
    m_deviceSettingsSaved = false;
    if (m_deviceSettingsAllowed) {
        m_deviceSettingsSaved = RtlDeviceSettings(m_settingsScope).saveAccepted(
            {m_ppmCorrection, m_dcSuppression}, m_deviceSettingsReason);
    }
}

QVariantMap RtlSdrBackend::deviceSettingsStatus() const
{
    return {{QStringLiteral("serial"), m_serial},
        {QStringLiteral("applied"), m_connected && m_lastPublished.has_value()},
        {QStringLiteral("ppm"), m_ppmCorrection},
        {QStringLiteral("dcSuppression"), m_dcSuppression},
        {QStringLiteral("pending"), m_capture.busy()},
        {QStringLiteral("requestedPpm"), m_requested.hardware.ppm},
        {QStringLiteral("requestedDcSuppression"), m_requested.dcSuppression},
        {QStringLiteral("saved"), m_deviceSettingsSaved},
        {QStringLiteral("saveReason"), m_deviceSettingsReason}};
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
    publishViewport();
    if (!current()) { return; }
    if (m_viewport) {
        emit panBandwidthLimitsChanged(kPanId, m_viewport->minimumSpanHz / 1e6,
                                       m_viewport->maximumSpanHz / 1e6);
    }
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

    for (const auto& receiver : m_capture.confirmed()->receivers) {
        emitSliceState(receiver);
        if (!current()) { return; }
    }
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
