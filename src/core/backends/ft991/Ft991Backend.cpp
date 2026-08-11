#include "core/backends/ft991/Ft991Backend.h"

#include "core/backends/ft991/Ft991Cat.h"
#include "core/backends/ft991/Ft991Settings.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lcFt991, "aether.ft991")

namespace AetherSDR::ft991 {

namespace {

// Default RX passband per mode (Hz, sign carries the sideband — SliceModel's
// convention). Display-side only on this radio: the FT-991's own filters are
// untouched (design doc, "Not in this cut").
std::pair<int, int> defaultPassbandForMode(const QString& mode)
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    if (u == QLatin1String("CW"))   return {350, 850};
    if (u == QLatin1String("CWL"))  return {-850, -350};
    if (u == QLatin1String("AM"))   return {-4000, 4000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM"))
        return {-8000, 8000};
    return {150, 3000};
}

// Phase-1 data-plane payload: a raw little-endian float32 array.
QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
            static_cast<qsizetype>(v.size() * sizeof(float))};
}

}  // namespace

Ft991Backend::Ft991Backend(QObject* parent) : IRadioBackend(parent)
{
    // No parent: moveToThread() refuses an object that has one. Destroyed
    // explicitly in the destructor after the thread is joined.
    m_device = new Ft991Device(nullptr);

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("ft991-io"));
    m_device->moveToThread(m_ioThread);
    m_ioThread->start();

    connect(m_device, &Ft991Device::opened, this,
            [this](const Ft991Device::OpenInfo& info) {
        m_openInfo = info;
        m_coveredSpanHz = info.coveredSpanHz > 0.0 ? info.coveredSpanHz
                                                   : kAudioSpanHz;
        m_connected = true;
        m_linkBlocksAtLastTick = 0;
        m_linkStatsTimer->start();
        emit connected();
        // Publish initial pan/slice state AFTER connected() — RadioModel's
        // onConnected() stages existing models as previous-session leftovers,
        // so anything emitted earlier is wiped before the UI sees it. Pan
        // FIRST: the pan must exist before anything describes it (#4470
        // shape). The dial and mode are already real: the device fetched
        // them before opened().
        emitPanState();
        emit panBandwidthLimitsChanged(panId(), m_coveredSpanHz * 2.0 / 1.0e6,
                                       m_coveredSpanHz * 2.0 / 1.0e6);
        const auto [lo, hi] = defaultPassbandForMode(m_mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
        pushPassbandToDevice();
        emitSliceState();
        defineMeters();
        emitTransmitState();
        if (m_openInfo.audioOutDesc.isEmpty())
            qCWarning(lcFt991) << "no TX audio device — session is receive-only";
    });
    connect(m_device, &Ft991Device::openFailed, this, [this](const QString& reason) {
        m_connected = false;
        m_linkStatsTimer->stop();
        emit connectionError(QStringLiteral("FT-991: %1").arg(reason));
    });
    connect(m_device, &Ft991Device::linkLost, this, [this](const QString& reason) {
        // The device already tore itself down; this side only reports.
        // disconnected() FIRST so the session state is consistent before
        // the error dialog names the cause.
        if (!m_connected)
            return;
        m_connected = false;
        m_moxRequested = false;
        m_tuneActive = false;
        m_linkStatsTimer->stop();
        emit disconnected();
        emit connectionError(QStringLiteral("FT-991: %1").arg(reason));
    });

    // ---- CAT state up. The radio is the authority; these follow it. ----
    connect(m_device, &Ft991Device::catFrequency, this, [this](double hz) {
        m_dialHz = hz;
        if (!m_connected)
            return;   // pre-opened() initial fetch; opened() will publish
        emitSliceState();
        emitPanState();
        emitTransmitState();
    });
    connect(m_device, &Ft991Device::catMode, this, [this](const QString& mode) {
        const bool changed = mode.compare(m_mode, Qt::CaseInsensitive) != 0;
        m_mode = mode;
        if (changed) {
            // The passband belongs to the mode; we own the audio filter, so
            // nothing heals a stale one for us.
            const auto [lo, hi] = defaultPassbandForMode(mode);
            m_filterLowHz = lo;
            m_filterHighHz = hi;
            pushPassbandToDevice();
        }
        if (!m_connected)
            return;
        emitSliceState();
        emitPanState();   // the sideband mapping may have flipped
    });
    connect(m_device, &Ft991Device::catTxState, this, [this](int state) {
        const bool mox = state != 0;
        if (mox == m_mox)
            return;
        m_mox = mox;
        if (m_connected)
            emitTransmitState();
    });
    connect(m_device, &Ft991Device::catPower, this, [this](int watts) {
        m_rfPowerWatts = watts;
        if (m_connected)
            emitTransmitState();
    });
    connect(m_device, &Ft991Device::catAgc, this, [this](const QString& agc) {
        m_agcMode = agc;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catWidth, this, [this](int shIndex) {
        m_radioShIndex = shIndex;
        applyRadioWidth(
            Ft991Cat::widthForIndex(m_mode, shIndex, m_radioNarrow));
    });
    connect(m_device, &Ft991Device::catNarrow, this, [this](bool narrow) {
        m_radioNarrow = narrow;
        if (m_radioShIndex >= 0)
            applyRadioWidth(
                Ft991Cat::widthForIndex(m_mode, m_radioShIndex, narrow));
    });
    connect(m_device, &Ft991Device::catNoiseBlanker, this, [this](bool on) {
        m_nbOn = on;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catNoiseBlankerLevel, this, [this](int level) {
        if (std::lround(m_nbLevelUi / 10.0) == level)
            return;   // our own echo, same radio step — keep the finer value
        m_nbLevelUi = std::clamp(level * 10, 0, 100);
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catNoiseReduction, this, [this](bool on) {
        m_nrOn = on;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catNoiseReductionLevel, this, [this](int level) {
        if (1 + std::lround(m_nrLevelUi * 14.0 / 100.0) == level)
            return;
        m_nrLevelUi = std::clamp(
            static_cast<int>(std::lround((level - 1) * 100.0 / 14.0)), 0, 100);
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catAutoNotch, this, [this](bool on) {
        m_anfOn = on;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catManualNotch, this, [this](bool on) {
        m_manualNotchOn = on;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catManualNotchHz, this, [this](int hz) {
        m_manualNotchHz = hz;
        // Report the radio's audio Hz back as the seam's 0..100 position,
        // measured across the CURRENT passband — the same conversion the
        // set path does, inverted.
        const int lo = std::min(std::abs(m_filterLowHz), std::abs(m_filterHighHz));
        const int hi = std::max(std::abs(m_filterLowHz), std::abs(m_filterHighHz));
        if (hi > lo)
            m_manualNotchPos = std::clamp((hz - lo) * 100 / (hi - lo), 0, 100);
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catSMeter, this, [this](int raw) {
        const double dbm = sMeterRawToDbm(raw);
        // Smooth every reading, publish on the tick (Colibri/Flex ballistics
        // so the needle behaves the same way across radios).
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

    connect(m_device, &Ft991Device::catClarifier, this,
            [this](bool ritOn, bool xitOn, int offsetHz) {
        m_ritOn = ritOn;
        m_xitOn = xitOn;
        m_clarifierHz = offsetHz;
        if (m_connected)
            emitSliceState();
    });
    connect(m_device, &Ft991Device::catTxPowerMeter, this, [this](int raw) {
        if (m_connected)
            emit meterUpdate(QStringLiteral("TX:FWDPWR"),
                             raw * kPoMeterFullScaleWatts / 255.0);
    });
    connect(m_device, &Ft991Device::catTxSwrMeter, this, [this](int raw) {
        if (m_connected)
            emit meterUpdate(QStringLiteral("TX:SWR"),
                             1.0 + raw * kSwrMeterSpanPerCount);
    });

    // ---- data plane up ----
    connect(m_device, &Ft991Device::spectrumFrame, this,
            [this](const std::vector<float>& binsDbfs) {
        if (!m_connected)
            return;
        // The symmetric window: 2N bins, dial at the seam between them.
        // dBFS -> displayed dBm is one constant, so nothing the client does
        // moves the trace. The audio bins land on the active sideband's
        // half (audio ascending = RF ascending on USB, descending on LSB);
        // AM/FM envelope audio has no sideband identity, so it is mirrored
        // onto both halves; a half with no data reads kPadFloorDbm.
        const std::size_t n = binsDbfs.size();
        if (n == 0)
            return;
        const Sideband sb = sideband();
        // The padded half sits at the frame's own measured floor WITH a
        // small deterministic texture. Both properties are load-bearing:
        //   - a level below any real floor (−160 was tried) poisons the
        //     display's auto-level statistics, which ramped without bound;
        //   - a CONSTANT level (the frame minimum was tried) is exactly the
        //     clipped-floor signature SpectrumWidget's headroom recovery
        //     looks for (bins at the frame min, in long identical runs) —
        //     it then requested 24 dB more range every second, forever.
        // floor + 0..3 dB of index-keyed ripple reads as a quiet floor to
        // both detectors, and is honest to the eye: visibly flat, visibly
        // below the real data.
        const float base = static_cast<float>(
            *std::min_element(binsDbfs.begin(), binsDbfs.end())
            + kFullScaleDbm);
        const auto padAt = [base](std::size_t i) {
            return base + 0.5f * static_cast<float>((i * 13u) % 7u);
        };
        std::vector<float> dbm(n * 2);
        if (sb != Sideband::High) {
            // Left half: RF below the dial — audio frequency DESCENDS
            // toward the pan centre, so the bins go in reversed.
            for (std::size_t i = 0; i < n; ++i)
                dbm[i] = static_cast<float>(binsDbfs[n - 1 - i] + kFullScaleDbm);
        } else {
            for (std::size_t i = 0; i < n; ++i)
                dbm[i] = padAt(i);
        }
        if (sb != Sideband::Low) {
            for (std::size_t i = 0; i < n; ++i)
                dbm[n + i] = static_cast<float>(binsDbfs[i] + kFullScaleDbm);
        } else {
            for (std::size_t i = 0; i < n; ++i)
                dbm[n + i] = padAt(n + i);
        }
        emit spectrumFrameReady(0, floatBytes(dbm));
    });
    connect(m_device, &Ft991Device::audioBlockReady, this,
            [this](const std::vector<float>& pcm) {
        if (m_connected)
            routeAudio(pcm);
    });

    m_linkStatsTimer = new QTimer(this);
    m_linkStatsTimer->setInterval(kLinkStatsIntervalMs);
    connect(m_linkStatsTimer, &QTimer::timeout, this,
            &Ft991Backend::publishLinkStats);
}

Ft991Backend::~Ft991Backend()
{
    if (m_ioThread) {
        // Close ON the device's own thread and WAIT: quit() below ends the
        // event loop a queued close would need, and tearing the port down
        // from this thread is an affinity bug. closeDevice() unkeys first.
        QMetaObject::invokeMethod(m_device, &Ft991Device::closeDevice,
                                  Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    }
    delete m_device;
}

RadioCapabilities Ft991Backend::capabilities() const
{
    RadioCapabilities c;
    c.family = familyName();
    c.model = QStringLiteral("FT-991");
    c.maxSlices = 1;          // one receiver behind one codec; not a policy
    c.maxPanadapters = 1;
    c.tuningMinHz = Ft991Cat::kTuningMinHz;
    c.tuningMaxHz = Ft991Cat::kTuningMaxHz;
    c.canTransmit = true;     // CAT TX1/TX0; the engine guard sits above
    c.txPowerMaxWatts = 100.0;
    // The RF modulator is in the radio, but what this capability gates —
    // the mic-source list, the PC-audio lock, TCI TX routing — is where the
    // TX AUDIO comes from, and that is this host (design doc).
    c.hostModulates = true;
    c.persistsMemories = false;   // client-side memory bank engages
    c.canReboot = false;
    c.hasTuner = false;           // the internal ATU has no status wire here (v1)
    c.hasAmplifier = false;
    c.hasExtendedDsp = false;
    c.hasProfiles = false;
    c.hasDaxStreams = false;
    c.hasRadioSideWaterfallAutoBlack = false;
    c.hasWaveforms = false;
    c.hasMultiClientSessions = false;   // one serial port, one owner
    c.hasGpsLocation = false;
    c.hasSupplyVoltageTelemetry = false;
    // The radio runs its own receive DSP and the seam verbs reach it:
    // NB+NL, DNR NR+RL, auto-notch BC, manual notch BP.
    c.hasRadioSideDsp = true;
    // …but nothing resembling WDSP's LMS/FFT family (NRL/ANFL/ANFT). This
    // is exactly the split hasLmsNoiseFilters was introduced for.
    c.hasLmsNoiseFilters = false;
    c.hasManualNotch = true;
    // EMPTY, deliberately: the FT-991 persists its own VFO/mode/settings
    // across power cycles — radio-authoritative (Constitution II/III), the
    // Flex rule. The client must restore nothing.
    c.clientSettingsDomains = {};
    return c;
}

void Ft991Backend::connectRadio(const RadioConnectRequest& request)
{
    // Identity is the discovery serial "ft991-<port>"; explicit automation
    // params override (same precedence shape as HL2/Colibri).
    m_portName.clear();
    const QString serial = request.serial;
    const qsizetype dash = serial.indexOf(QLatin1Char('-'));
    if (dash >= 0)
        m_portName = serial.mid(dash + 1);
    if (request.params.contains(QStringLiteral("serialPort")))
        m_portName = request.params.value(QStringLiteral("serialPort")).toString();
    if (m_portName.isEmpty()) {
        emit connectionError(QStringLiteral(
            "FT-991: no serial port in connect request (serial \"%1\")")
                .arg(serial));
        return;
    }

    m_baudRate = Ft991Settings::baudRate();
    if (request.params.contains(QStringLiteral("baudRate")))
        m_baudRate = request.params.value(QStringLiteral("baudRate")).toInt();

    Ft991Device::Params p;
    p.portName = m_portName;
    p.baudRate = m_baudRate;
    p.audioInHint = Ft991Settings::audioInHint();
    p.audioOutHint = Ft991Settings::audioOutHint();
    if (request.params.contains(QStringLiteral("audioIn")))
        p.audioInHint = request.params.value(QStringLiteral("audioIn")).toString();
    if (request.params.contains(QStringLiteral("audioOut")))
        p.audioOutHint = request.params.value(QStringLiteral("audioOut")).toString();
    p.spectrumSpanHz = kAudioSpanHz;

    m_moxRequested = false;
    m_tuneActive = false;
    m_haveSMeter = false;

    qCInfo(lcFt991) << "connecting:" << p.portName << "@" << p.baudRate
                    << "baud, audio in/out hint" << p.audioInHint;
    QMetaObject::invokeMethod(m_device, [dev = m_device, p] {
        dev->openDevice(p);
    }, Qt::QueuedConnection);
}

void Ft991Backend::disconnectRadio()
{
    QMetaObject::invokeMethod(m_device, &Ft991Device::closeDevice,
                              Qt::BlockingQueuedConnection);
    if (m_connected) {
        m_connected = false;
        m_linkStatsTimer->stop();
        emit disconnected();
    }
}

bool Ft991Backend::isConnected() const
{
    return m_connected;
}

bool Ft991Backend::isOurPan(const QString& id) const
{
    // An EMPTY pan id addresses the (only) receiver.
    return id.isEmpty() || id == panId();
}

// ---------------------------------------------------------------------------
// Slice verbs — optimistic local echo, the poll confirms (or corrects)
// ---------------------------------------------------------------------------

void Ft991Backend::setSliceFrequency(int sliceId, double hz)
{
    if (sliceId != 0)
        return;
    if (!(hz >= Ft991Cat::kTuningMinHz) || !(hz <= Ft991Cat::kTuningMaxHz)) {
        qCWarning(lcFt991) << "tune refused:" << hz << "Hz out of range";
        return;
    }
    // Integer Hz: CAT carries whole hertz, and an exactly-representable
    // dial is what makes the pan<->slice round trip a fixed point instead
    // of a sub-hertz oscillation.
    const double dial = static_cast<double>(std::llround(hz));
    if (dial == m_dialHz)
        return;   // change-gated: an echo of our own state must die here
    if (m_tuneVerbDepth >= kMaxTuneVerbDepth) {
        qCWarning(lcFt991) << "tune verb recursion truncated at depth"
                           << m_tuneVerbDepth << "(requested" << dial << "Hz)";
        return;
    }
    ++m_tuneVerbDepth;
    m_dialHz = dial;
    QMetaObject::invokeMethod(m_device, [dev = m_device, dial] {
        dev->setFrequencyHz(dial);
    }, Qt::QueuedConnection);
    emitSliceState();
    emitPanState();
    --m_tuneVerbDepth;
}

void Ft991Backend::setSliceMode(int sliceId, const QString& mode)
{
    if (sliceId != 0)
        return;
    if (Ft991Cat::modeToCat(mode).isNull()) {
        qCWarning(lcFt991) << "mode" << mode << "has no FT-991 mapping";
        return;
    }
    const bool changed = mode.compare(m_mode, Qt::CaseInsensitive) != 0;
    m_mode = mode.toUpper();
    if (changed) {
        const auto [lo, hi] = defaultPassbandForMode(m_mode);
        m_filterLowHz = lo;
        m_filterHighHz = hi;
        pushPassbandToDevice();
    }
    QMetaObject::invokeMethod(m_device, [dev = m_device, m = m_mode] {
        dev->setMode(m);
    }, Qt::QueuedConnection);
    emitSliceState();
    emitPanState();
}

void Ft991Backend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (sliceId != 0)
        return;
    // The handles are doubly real: the host biquad chain applies the exact
    // edges immediately, AND the radio's own DSP width follows — snapped to
    // the nearest CAT SH step. The width confirm-poll then reflects what
    // the radio actually took (applyRadioWidth), so the display converges
    // on radio truth rather than on the request.
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    pushPassbandToDevice();
    if (Ft991Cat::widthFamilyForMode(m_mode) != Ft991Cat::WidthFamily::Fixed) {
        const int width = std::abs(highHz - lowHz);
        QMetaObject::invokeMethod(m_device,
            [dev = m_device, m = m_mode, width] {
                dev->setRadioWidth(m, width);
            }, Qt::QueuedConnection);
    }
    emitSliceState();
}

void Ft991Backend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    if (sliceId != 0)
        return;
    m_agcMode = mode;
    m_agcThresholdDb = thresholdDb;   // no CAT threshold; kept for the UI echo
    QMetaObject::invokeMethod(m_device, [dev = m_device, m = mode] {
        dev->setAgc(m);
    }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setSliceNoiseBlanker(int sliceId, bool on, int level)
{
    if (sliceId != 0)
        return;
    m_nbOn = on;
    m_nbLevelUi = std::clamp(level, 0, 100);
    const int radioLevel =
        static_cast<int>(std::lround(m_nbLevelUi / 10.0));   // NL 0..10
    QMetaObject::invokeMethod(m_device, [dev = m_device, on, radioLevel] {
        dev->setRadioNoiseBlanker(on);
        dev->setRadioNoiseBlankerLevel(radioLevel);
    }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setSliceNoiseReduction(int sliceId, bool on, int level)
{
    if (sliceId != 0)
        return;
    m_nrOn = on;
    m_nrLevelUi = std::clamp(level, 0, 100);
    const int radioLevel = 1
        + static_cast<int>(std::lround(m_nrLevelUi * 14.0 / 100.0));  // RL 1..15
    QMetaObject::invokeMethod(m_device, [dev = m_device, on, radioLevel] {
        dev->setRadioNoiseReduction(on);
        dev->setRadioNoiseReductionLevel(radioLevel);
    }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setSliceAutoNotch(int sliceId, bool on)
{
    if (sliceId != 0)
        return;
    m_anfOn = on;
    QMetaObject::invokeMethod(m_device, [dev = m_device, on] {
        dev->setRadioAutoNotch(on);
    }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setSliceManualNotch(int sliceId, bool on, int position)
{
    if (sliceId != 0)
        return;
    // The seam's position is 0..100 ACROSS THE PASSBAND, not a frequency —
    // so the notch tracks the filter instead of being re-derived by every
    // caller. This radio places it in audio Hz (BP01, 10 Hz steps), and the
    // passband is where that conversion belongs.
    m_manualNotchPos = std::clamp(position, 0, 100);
    const int lo = std::min(std::abs(m_filterLowHz), std::abs(m_filterHighHz));
    const int hi = std::max(std::abs(m_filterLowHz), std::abs(m_filterHighHz));
    const int hz = lo + (hi - lo) * m_manualNotchPos / 100;
    m_manualNotchOn = on;
    m_manualNotchHz = hz;
    QMetaObject::invokeMethod(m_device, [dev = m_device, on, hz] {
        dev->setRadioManualNotch(on, hz);
    }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setRitEnabled(bool on)
{
    m_ritOn = on;
    pushClarifier();
}

void Ft991Backend::setXitEnabled(bool on)
{
    m_xitOn = on;
    pushClarifier();
}

void Ft991Backend::setRitOffset(int hz)
{
    // XIT routes here too (setXitOffset's default), which is the truth on
    // this radio: one clarifier register, two enables.
    m_clarifierHz = hz;
    pushClarifier();
}

void Ft991Backend::pushClarifier()
{
    QMetaObject::invokeMethod(m_device,
        [dev = m_device, r = m_ritOn, x = m_xitOn, o = m_clarifierHz] {
            dev->setRadioClarifier(r, x, o);
        }, Qt::QueuedConnection);
    emitSliceState();
}

void Ft991Backend::setSliceAudioMute(int sliceId, bool mute)
{
    if (sliceId != 0)
        return;
    m_audioMuted = mute;
    emitSliceState();
}

void Ft991Backend::setSliceAudioGain(int sliceId, int gainPercent)
{
    if (sliceId != 0)
        return;
    m_audioGain = static_cast<float>(std::clamp(gainPercent, 0, 100)) / 100.0f;
    emitSliceState();
}

void Ft991Backend::setSliceAudioPan(int sliceId, int panPercent)
{
    if (sliceId != 0)
        return;
    m_audioPanPercent = std::clamp(panPercent, 0, 100);
    emitSliceState();
}

void Ft991Backend::setActiveSlice(int sliceId)
{
    Q_UNUSED(sliceId);   // one slice; always active
}

void Ft991Backend::setTxSlice(int sliceId)
{
    Q_UNUSED(sliceId);   // one slice; transmit always lives on it
}

// ---------------------------------------------------------------------------
// Pan verbs — the lens
// ---------------------------------------------------------------------------

Ft991Backend::Sideband Ft991Backend::sideband() const
{
    const QString u = m_mode.toUpper();
    if (u == QLatin1String("AM") || u == QLatin1String("FM")
        || u == QLatin1String("NFM"))
        return Sideband::Both;
    return Ft991Cat::isLowSideband(m_mode) ? Sideband::Low : Sideband::High;
}

void Ft991Backend::setPanCenter(const QString& id, double hz,
                                PanCenterIntent intent)
{
    if (!isOurPan(id) || hz <= 0.0)
        return;
    // This window is the audio band around the dial — slaved to the VFO,
    // exactly the case the intent exists for. A DRAG is the operator asking
    // to listen elsewhere, so it retunes. A centre riding along with a zoom
    // must NOT walk the radio across the band, so it is answered with the
    // geometry we already have.
    if (intent == PanCenterIntent::Range) {
        emitPanState();
        return;
    }
    // The window is centred ON the dial, so moving the window IS retuning
    // the radio — and the GUI re-centring the pan onto the slice is the
    // identity, which the change gate in setSliceFrequency absorbs.
    const double dial = std::round(hz);
    if (dial == m_dialHz) {
        emitPanState();
        return;
    }
    setSliceFrequency(0, dial);
}

void Ft991Backend::setPanBandwidth(const QString& id, double hz)
{
    if (!isOurPan(id) || hz <= 0.0)
        return;
    // The span is the audio window; there is nothing to zoom. Re-emitting
    // the unchanged span is how the widget snaps back to what is real
    // (#4470 honesty rule).
    emitPanState();
}

void Ft991Backend::setPanFrameRate(const QString& id, int fps)
{
    if (!isOurPan(id))
        return;
    QMetaObject::invokeMethod(m_device, [dev = m_device, fps] {
        dev->setSpectrumRateFps(fps);
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

void Ft991Backend::setKeying(bool key)
{
    if (!m_connected) {
        if (key)
            qCWarning(lcFt991) << "keying refused: not connected";
        return;
    }
    m_moxRequested = key;
    QMetaObject::invokeMethod(m_device, [dev = m_device, key] {
        dev->setPtt(key);
    }, Qt::QueuedConnection);
    // Optimistic: the TX poll confirms (and reflects the radio's own
    // unkeying — TOT, front-panel PTT release — either way).
    m_mox = key;
    emitTransmitState();
}

void Ft991Backend::setTune(bool on, int tunePowerPercent)
{
    if (!m_connected)
        return;
    if (on == m_tuneActive)
        return;
    m_tuneActive = on;
    if (on) {
        // Swap the drive to TUNE power for the duration; restore after.
        m_preTunePowerWatts = m_rfPowerWatts;
        if (tunePowerPercent >= 0) {
            const int watts = std::clamp(tunePowerPercent, 5, 100);
            QMetaObject::invokeMethod(m_device, [dev = m_device, watts] {
                dev->setPowerWatts(watts);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(m_device, &Ft991Device::startTune,
                                  Qt::QueuedConnection);
        m_moxRequested = true;
        m_mox = true;
    } else {
        QMetaObject::invokeMethod(m_device, &Ft991Device::stopTune,
                                  Qt::QueuedConnection);
        if (m_preTunePowerWatts > 0) {
            const int watts = m_preTunePowerWatts;
            QMetaObject::invokeMethod(m_device, [dev = m_device, watts] {
                dev->setPowerWatts(watts);
            }, Qt::QueuedConnection);
            m_preTunePowerWatts = -1;
        }
        m_moxRequested = false;
        m_mox = false;
    }
    emitTransmitState();
}

void Ft991Backend::setTxPower(int percent)
{
    // On HF the FT-991's PC range is 5..100 W, so percent maps 1:1 onto
    // watts. Clamped low rather than refused: 0% means "as low as it goes".
    const int watts = std::clamp(percent, 5, 100);
    m_rfPowerWatts = watts;
    QMetaObject::invokeMethod(m_device, [dev = m_device, watts] {
        dev->setPowerWatts(watts);
    }, Qt::QueuedConnection);
    emitTransmitState();
}

void Ft991Backend::setMicGain(int level)
{
    // Applied digitally to the TX audio this host plays into the codec —
    // the radio's own mic preamp is not in this path.
    m_micGainPercent = std::clamp(level, 0, 100);
}

void Ft991Backend::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz)
{
    if (!m_connected || !m_moxRequested)
        return;
    const float gain = static_cast<float>(m_micGainPercent) / 100.0f;
    QMetaObject::invokeMethod(m_device,
        [dev = m_device, pcm = int16Stereo, sampleRateHz, gain] {
            dev->submitTxAudio(pcm, sampleRateHz, gain);
        }, Qt::QueuedConnection);
}

void Ft991Backend::invokeExtension(const QString& ns, const QString& verb,
                                   quint64 requestId, const QVariant& arg)
{
    Q_UNUSED(arg);
    if (requestId == 0)
        return;
    emit extensionError(requestId,
                        QStringLiteral("unknown extension %1.%2").arg(ns, verb));
}

// ---------------------------------------------------------------------------
// State publication
// ---------------------------------------------------------------------------

void Ft991Backend::applyRadioWidth(int widthHz)
{
    if (widthHz <= 0)
        return;
    if (Ft991Cat::widthFamilyForMode(m_mode) == Ft991Cat::WidthFamily::Fixed)
        return;
    // Same width as already displayed: keep the operator's exact edges —
    // the radio cannot represent an off-centre drag, and rewriting equal
    // width with anchored edges would fight every fine adjustment.
    if (std::abs(m_filterHighHz - m_filterLowHz) == widthHz)
        return;
    const bool cw = m_mode.compare(QLatin1String("CW"), Qt::CaseInsensitive) == 0
        || m_mode.compare(QLatin1String("CWL"), Qt::CaseInsensitive) == 0;
    int lo = 0;
    int hi = 0;
    if (cw) {
        // CW displays carrier-centred (SliceModel heals CW filters to that
        // convention anyway); the pitch offset is applied where the audio
        // band is computed, in pushPassbandToDevice().
        lo = -widthHz / 2;
        hi = lo + widthHz;
    } else {
        lo = std::max(kMinFilterEdgeHz, kSsbWidthAnchorHz - widthHz / 2);
        hi = lo + widthHz;
        if (Ft991Cat::isLowSideband(m_mode)) {
            const int t = lo;
            lo = -hi;
            hi = -t;
        }
    }
    m_filterLowHz = lo;
    m_filterHighHz = hi;
    pushPassbandToDevice();
    if (m_connected)
        emitSliceState();
}

void Ft991Backend::pushPassbandToDevice()
{
    int lo = m_filterLowHz;
    int hi = m_filterHighHz;
    // CW's filter convention is CARRIER-centred (a 100 Hz filter displays
    // as -50..+50), but the radio's demodulated audio puts the carrier at
    // the sidetone pitch — the audio band is pitch ± width/2, and feeding
    // the signed pair to the straddle logic below would low-pass at 50 Hz
    // and silence the tone entirely.
    const QString u = m_mode.toUpper();
    if (u == QLatin1String("CW") || u == QLatin1String("CWL")) {
        const int width = std::abs(hi - lo);
        lo = std::max(10, kCwWidthAnchorHz - width / 2);
        hi = lo + width;
    }
    QMetaObject::invokeMethod(m_device, [dev = m_device, lo, hi] {
        dev->setAudioPassband(lo, hi);
    }, Qt::QueuedConnection);
}

void Ft991Backend::routeAudio(const std::vector<float>& pcm)
{
    // THIS SLICE's audio, pre-mute and pre-gain — a decoder/TCI consumer
    // must not stop decoding because the operator muted the speaker.
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

double Ft991Backend::sMeterRawToDbm(int raw) const
{
    return kSMeterFloorDbm + std::clamp(raw, 0, 255) * kSMeterDbPerCount;
}

void Ft991Backend::emitSliceState()
{
    SliceDelta d;
    d.panId = panId();
    d.frequency = m_dialHz / 1.0e6;   // MHz — the dial IS the slice
    d.mode = m_mode;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    d.agcMode = m_agcMode;
    d.agcThreshold = m_agcThresholdDb;
    d.audioMute = m_audioMuted;
    d.audioPan = m_audioPanPercent;
    // Radio-side DSP mirror: what the CAT plane last confirmed (or our
    // optimistic set, corrected by the next poll).
    d.nb = m_nbOn;
    d.nbLevel = m_nbLevelUi;
    d.nr = m_nrOn;
    d.nrLevel = m_nrLevelUi;
    d.anf = m_anfOn;
    // The shared clarifier reported into BOTH halves — the model keeps
    // them separate, but on this radio they are one number.
    d.ritOn = m_ritOn;
    d.ritFreq = m_clarifierHz;
    d.xitOn = m_xitOn;
    d.xitFreq = m_clarifierHz;
    d.mn = m_manualNotchOn;
    d.mnLevel = m_manualNotchPos;
    // ONE slice: always active, always the TX slice — this radio's one
    // transmitter lives wherever the dial is.
    d.active = true;
    d.txSlice = true;
    emit sliceChanged(0, d);
}

void Ft991Backend::emitPanState()
{
    // Centre == dial, span == both halves of the symmetric window.
    emit panCenterBandwidthChanged(panId(), m_dialHz / 1.0e6,
                                   m_coveredSpanHz * 2.0 / 1.0e6);
}

void Ft991Backend::emitTransmitState()
{
    TransmitDelta d;
    d.mox = m_mox;
    d.tune = m_tuneActive;
    d.rfPower = m_rfPowerWatts;      // 1:1 watts<->percent on this radio
    d.maxPowerLevel = 100;
    d.transmitFreq = m_dialHz / 1.0e6;
    d.txSliceMode = m_mode;
    emit transmitChanged(d);
}

void Ft991Backend::defineMeters()
{
    MeterDef d;
    d.index = 1;
    d.source = QStringLiteral("SLC");
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = QStringLiteral("Receive signal level (radio S-meter, uncalibrated)");
    emit meterDefined(d);

    // TX meters, polled off RM5/RM6 while keyed. FWDPWR and SWR are the
    // names MeterModel resolves for the TX gauges.
    MeterDef po;
    po.index = 2;
    po.source = QStringLiteral("TX");
    po.name = QStringLiteral("FWDPWR");
    po.unit = QStringLiteral("W");
    po.low = 0.0;
    po.high = kPoMeterFullScaleWatts;
    po.description = QStringLiteral("Forward power (radio PO meter, uncalibrated)");
    emit meterDefined(po);

    MeterDef swr;
    swr.index = 3;
    swr.source = QStringLiteral("TX");
    swr.name = QStringLiteral("SWR");
    swr.unit = QStringLiteral("SWR");
    swr.low = 1.0;
    swr.high = 5.0;
    swr.description = QStringLiteral("Antenna SWR (radio meter, uncalibrated)");
    emit meterDefined(swr);
}

void Ft991Backend::publishLinkStats()
{
    LinkStats s = linkStats();
    // Fresh audio blocks since the last tick — the proof-of-life the
    // heartbeat runs on. The codec stream is the transport of consequence:
    // CAT alone answering while audio is dead IS a dead receiver.
    const quint64 blocks = m_device ? m_device->audioBlocks() : 0;
    s.alive = m_connected && blocks != m_linkBlocksAtLastTick;
    m_linkBlocksAtLastTick = blocks;
    emit linkStatsUpdated(s);
}

IRadioBackend::LinkStats Ft991Backend::linkStats() const
{
    LinkStats s;
    s.reported = m_connected;
    if (!m_connected || !m_device)
        return s;
    s.rxPackets = m_device->audioBlocks();
    s.rxBytes = static_cast<qint64>(m_device->serialRxBytes()
        + m_device->audioSamples() * sizeof(float));
    s.txBytes = static_cast<qint64>(m_device->serialTxBytes());
    // A real request/response RTT, from the CAT query clock.
    s.rttMs = m_device->catRttMs();
    s.jitterMs = -1;
    s.localEndpoint = m_portName;
    return s;
}

IRadioBackend::HealthSnapshot Ft991Backend::healthSnapshot() const
{
    HealthSnapshot h;
    auto put = [&h](const QString& key, const QVariant& value, const QString& label) {
        h.values.insert(key, value);
        h.order.append(key);
        h.labels.insert(key, label);
    };
    put(QStringLiteral("port"),
        QStringLiteral("%1 @ %2 baud (8N2)").arg(m_portName).arg(m_baudRate),
        QStringLiteral("CAT port"));
    if (m_device) {
        const int rtt = m_device->catRttMs();
        if (rtt >= 0)
            put(QStringLiteral("catRtt"), QStringLiteral("%1 ms").arg(rtt),
                QStringLiteral("CAT round-trip"));
        put(QStringLiteral("catTimeouts"), m_device->catTimeouts(),
            QStringLiteral("CAT timeouts"));
    }
    if (m_connected) {
        put(QStringLiteral("audioIn"),
            QStringLiteral("%1 @ %2 Hz")
                .arg(m_openInfo.audioInDesc)
                .arg(m_openInfo.audioInRateHz),
            QStringLiteral("RX audio"));
        // "none" is information on a transceiver: it means TX is silent.
        put(QStringLiteral("audioOut"),
            m_openInfo.audioOutDesc.isEmpty()
                ? QStringLiteral("none — TX audio unavailable")
                : QStringLiteral("%1 @ %2 Hz")
                      .arg(m_openInfo.audioOutDesc)
                      .arg(m_openInfo.audioOutRateHz),
            QStringLiteral("TX audio"));
        put(QStringLiteral("lens"),
            QStringLiteral("%1 Hz audio window")
                .arg(static_cast<int>(std::lround(m_coveredSpanHz))),
            QStringLiteral("Panadapter span"));
        if (m_device)
            put(QStringLiteral("audioBlocksIn"),
                static_cast<qulonglong>(m_device->audioBlocks()),
                QStringLiteral("Audio blocks received"));
    }
    h.sections.insert(h.order.isEmpty() ? QString{} : h.order.first(),
                      QStringLiteral("FT-991"));
    return h;
}

}  // namespace AetherSDR::ft991
