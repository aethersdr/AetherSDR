#include "core/backends/anan/AnanBackend.h"
#include "core/AppActivity.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/AnanDroopDefaults.h"
#include "core/backends/anan/AnanReceiverRoutes.h"
#include "core/backends/WdspNoiseBlanker.h"
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>

Q_LOGGING_CATEGORY(lcAnanDefaults, "aether.anan.droopdefaults", QtWarningMsg)
Q_LOGGING_CATEGORY(lcAnan, "aether.anan.backend", QtWarningMsg)

namespace AetherSDR::anan {

namespace {

// Placeholder until a bench measurement exists -- see the class comment.
// Named and zero rather than silently absent, so the next reader finds a
// TODO instead of an unexplained 1:1 dBFS/dBm mapping.
constexpr float kUncalibratedDbfsToDbmOffset = 0.0f;

// One FFT AVG slider step as analyzer averaging time. deskHPSDR sets its
// analyzer's averaging as a time in 10 ms steps (default 250 ms), so 0 = none,
// 25 = deskHPSDR's default, 100 = one second. Published in capabilities()
// as BackendPanAveraging::msPerAverageStep.
constexpr int kMsPerAverageStep = 10;

// S-meter: WDSP's RXA_S_AV is 10*log10 mean I^2+Q^2 at the P2 24-bit full scale
// (kFullScale24Bit), i.e. dBFS. deskHPSDR uses the same scaling (1/2^23) and a
// 0 dB ANAN offset, so dBFS reads as dBm; this follows it. onDspMeter() adds the
// ADC attenuation before smoothing. No per-radio rx_gain_calibration yet. Kept
// apart from kUncalibratedDbfsToDbmOffset, which labels the panadapter axis.
constexpr double kSMeterDbmOffset = 0.0;

QByteArray floatBytes(const std::vector<float>& v)
{
    return {reinterpret_cast<const char*>(v.data()),
           static_cast<qsizetype>(v.size() * sizeof(float))};
}

// The slice model's name for a WDSP AGC mode, the inverse of setSliceAgc()'s
// parse. WDSP's "long" (1) is never set here.
QString agcModeName(int wdspMode)
{
    switch (wdspMode) {
    case 0: return QStringLiteral("off");
    case 2: return QStringLiteral("slow");
    case 4: return QStringLiteral("fast");
    default: return QStringLiteral("med");
    }
}

// The WDSP AGC mode for a slice model AGC name: medium, WDSP's own default,
// for any name but the three others agcModeName() gives.
int agcModeFromName(const QString& name)
{
    const QString m = name.trimmed().toLower();
    if (m == QLatin1String("off"))  return 0;
    if (m == QLatin1String("slow")) return 2;
    if (m == QLatin1String("fast")) return 4;
    return 3;
}

}  // namespace

WdspChannel::Mode AnanBackend::modeFromString(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("LSB"))  return WdspChannel::Mode::Lsb;
    if (u == QLatin1String("USB"))  return WdspChannel::Mode::Usb;
    if (u == QLatin1String("DSB"))  return WdspChannel::Mode::Dsb;
    // "CW" is the spelling TciProtocol::tciToSmartSDR produces and the one a
    // Flex reports -- HERMES.md §16.7's own regression was this falling
    // through to the USB fallback because only "CWU" was mapped.
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")) return WdspChannel::Mode::Cwu;
    if (u == QLatin1String("CWL")) return WdspChannel::Mode::Cwl;
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return WdspChannel::Mode::Fm;
    if (u == QLatin1String("AM"))   return WdspChannel::Mode::Am;
    if (u == QLatin1String("DIGU")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("DIGL")) return WdspChannel::Mode::Digl;
    // WDSP has no RTTY demod: RTTY is FSK decoded externally from a wide passband,
    // so map it to DIGU rather than falling through to USB (HERMES.md 15.7/16.7).
    if (u == QLatin1String("RTTY")) return WdspChannel::Mode::Digu;
    if (u == QLatin1String("SAM"))  return WdspChannel::Mode::Sam;
    if (u == QLatin1String("DRM"))  return WdspChannel::Mode::Drm;
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return WdspChannel::Mode::Wbfm;
    return WdspChannel::Mode::Usb;   // unknown mode: same fallback as Hl2Backend's
}

bool AnanBackend::isKnownModeString(const QString& mode) noexcept
{
    // Every branch of modeFromString() above, aliases included.
    static const QStringList kKnown = {
        QStringLiteral("LSB"),  QStringLiteral("USB"),  QStringLiteral("DSB"),
        QStringLiteral("CWU"),  QStringLiteral("CW"),   QStringLiteral("CWL"),
        QStringLiteral("FM"),   QStringLiteral("NFM"),  QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("RTTY"),
        QStringLiteral("SAM"),  QStringLiteral("DRM"),  QStringLiteral("WBFM"),
        QStringLiteral("WFM"),
    };
    return kKnown.contains(mode.toUpper());
}

const QStringList& AnanBackend::publishedModeStrings() noexcept
{
    static const QStringList kPublished = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("DSB"),
        QStringLiteral("CWL"), QStringLiteral("CW"),  QStringLiteral("FM"),
        QStringLiteral("AM"),  QStringLiteral("SAM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"),
    };
    return kPublished;
}

bool AnanBackend::isOfferedModeString(const QString& mode) noexcept
{
    return isKnownModeString(mode) && publishedModeStrings().contains(canonicalOfferedMode(mode));
}

QString AnanBackend::canonicalOfferedMode(const QString& mode) noexcept
{
    // modeFromString()'s alias pairs, read off its NFM/FM, CWU/CW, WFM/WBFM and
    // RTTY/DIGU branches; a new alias there needs its pair here.
    const QString u = mode.toUpper();
    if (u == QLatin1String("NFM"))  return QStringLiteral("FM");
    if (u == QLatin1String("CWU"))  return QStringLiteral("CW");
    if (u == QLatin1String("WFM"))  return QStringLiteral("WBFM");
    if (u == QLatin1String("RTTY")) return QStringLiteral("DIGU");
    return u;
}

bool AnanBackend::isRestorableFrequencyHz(double hz) noexcept
{
    // Strictly inside (0, Nyquist): zero is the sentinel, and at or above
    // kDspClockHz/2 phaseWord() aliases rather than tuning.
    return std::isfinite(hz) && hz > 0.0
           && hz < static_cast<double>(kDspClockHz) / 2.0;
}

std::pair<int, int> AnanBackend::defaultPassbandForMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("USB"))  return {100, 2900};
    if (u == QLatin1String("LSB"))  return {-2900, -100};
    if (u == QLatin1String("DIGU")) return {150, 3000};
    if (u == QLatin1String("DIGL")) return {-3000, -150};
    // Same window as DIGU -- see modeFromString()'s own comment on why RTTY
    // maps to Digu -- made explicit rather than landing here only because it
    // happens to equal this function's own unknown-mode fallback below.
    if (u == QLatin1String("RTTY")) return {150, 3000};
    if (u == QLatin1String("CWU") || u == QLatin1String("CW")
        || u == QLatin1String("CWL")) return {-250, 250};
    if (u == QLatin1String("AM") || u == QLatin1String("SAM")) return {-4000, 4000};
    if (u == QLatin1String("DSB")) return {-3000, 3000};
    if (u == QLatin1String("FM") || u == QLatin1String("NFM")) return {-8000, 8000};
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")) return {-40000, 40000};
    if (u == QLatin1String("DRM")) return {-5000, 5000};
    return {150, 3000};   // matches modeFromString's USB fallback
}

double AnanBackend::cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("CWU") || u == QLatin1String("CW"))
        return static_cast<double>(pitchHz);
    if (u == QLatin1String("CWL"))
        return -static_cast<double>(pitchHz);
    return 0.0;
}

AnanBackend::AnanBackend(QObject* parent)
    : IRadioBackend(parent)
    , m_droopCalibrator(AnanDroopCalibrator::Hooks{
        [this] { return m_connected && !m_radioSerial.isEmpty()
                         && capabilities().hostDroopCalibration; },
        // The sweep zooms and measures RX1's pan (see the spectrum handler).
        [this](int rateKsps) { setPanBandwidth(rx1().panId, rateKsps * 1000.0); },
        [this](bool bypass) {
            for (const Receiver& rx : m_receivers) {
                QMetaObject::invokeMethod(rx.dsp, "setDroopCorrectionBypassed",
                    Qt::QueuedConnection, Q_ARG(bool, bypass));
            }
        },
        [this](const QMap<int, DroopCorrectionTable>& tables) {
            return applyDroopTables(tables);
        }})
{
    connect(&m_droopCalibrator, &AnanDroopCalibrator::started, this, [this] {
        m_droopMessage = QStringLiteral("Sweeping");
        m_droopPercent = 0;
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::progress, this,
            [this](int rateIndex, int totalRates, int percent) {
        m_droopPercent = percent;
        m_droopMessage = QStringLiteral("Sweeping — rate %1 of %2…")
                            .arg(rateIndex + 1).arg(totalRates);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [this](const QString& reason) {
        m_droopMessage = QStringLiteral("Error: %1").arg(reason);
        publishDroopStatus();
    });
    connect(&m_droopCalibrator, &AnanDroopCalibrator::finished, this, [this](bool applied) {
        if (!m_droopMessage.startsWith(QLatin1String("Error:"))) {
            m_droopMessage = applied
                ? QStringLiteral("Applied — the measured correction is now live and saved.")
                : (m_droopCalibrator.hasResult()
                    ? QStringLiteral("Sweep stopped — review the result, then Apply or Discard.")
                    : QStringLiteral("Sweep stopped — no result to apply."));
        }
        publishDroopStatus();
    });
    m_client = new P2Client(nullptr);   // nullptr parent: moveToThread requires it
    // One receiver per route, in route order, so index i is slice i and pan i.
    // RX1 is open from the start: its slice and pan are the radio's, and they
    // take settings before a connect.
    for (std::size_t i = 0; i < m_receivers.size(); ++i) {
        Receiver& rx = m_receivers[i];
        rx.index = static_cast<int>(i);
        rx.route = kReceiverRoutes[i];
        rx.adc = rx.route.adc;
        rx.panId = QStringLiteral("anan-%1").arg(rx.index);
        rx.open = i == 0;
        rx.dsp = new AnanRxDsp(nullptr);
    }
    m_speakerMix.setInputActive(rx1().index, true);

    // Leading+trailing throttle for setSliceFrequency()'s expensive side
    // effects -- see scheduleTuneApply()'s comment. Lives on this object
    // (the GUI thread), same as the backend itself; not moved to m_ioThread.
    m_tuneThrottleTimer = new QTimer(this);
    m_tuneThrottleTimer->setSingleShot(true);
    connect(m_tuneThrottleTimer, &QTimer::timeout, this, [this] {
        if (m_tunePendingApply) {
            m_tunePendingApply = false;
            applyTuneToRadioAndPan();
            m_tuneThrottleTimer->start(kTuneThrottleMs);   // cooldown for the trailing apply too
        }
        // else: nothing arrived during the cooldown -- let the timer sit
        // idle until the next scheduleTuneApply() restarts it.
    });

    m_ioThread = new QThread(this);
    m_ioThread->setObjectName(QStringLiteral("anan-io"));
    m_client->moveToThread(m_ioThread);
    for (Receiver& rx : m_receivers)
        rx.dsp->moveToThread(m_ioThread);
    AetherSDR::startStreamThread(m_ioThread);   // High QoS; see AppActivity.h

    // Build-only thread for a rate change's background DSP rebuild -- see
    // the member declaration comment. m_dspBuildContext owns no state; it
    // exists purely so QMetaObject::invokeMethod has a thread-affinity
    // target to hop the (slow) AnanRxDsp::buildChannel() call onto.
    m_dspBuildThread = new QThread(this);
    m_dspBuildThread->setObjectName(QStringLiteral("anan-dsp-build"));
    m_dspBuildContext = new QObject();   // nullptr parent: moveToThread requires it
    m_dspBuildContext->moveToThread(m_dspBuildThread);
    // Same High QoS as the io thread: this one holds the FFTW planner mutex the
    // io thread takes in WdspChannel::setShift, so lower would be a priority
    // inversion. Idle except during a rebuild.
    AetherSDR::startStreamThread(m_dspBuildThread);

    // Both live on the I/O thread -- a same-thread call either way, but
    // explicit to document intent and match Hl2Backend's own explicit
    // DirectConnection on the equivalent wire-to-DSP wiring: no queue, no
    // event-loop hop for the sample path. Each receiver takes its own DDC.
    for (const Receiver& rx : m_receivers) {
        connect(m_client, &P2Client::ddcIqReady, rx.dsp,
                [dsp = rx.dsp, ddc = rx.route.ddc](int ddcIndex,
                                                   const std::vector<std::complex<float>>& block) {
            if (ddcIndex == ddc) {
                dsp->processIqBlock(block);
            }
        }, Qt::DirectConnection);
    }

    // Everything below crosses from the I/O thread to whichever thread this
    // object lives on -- plain AutoConnection resolves to Queued, which is
    // why AnanRxDsp's constructor registers std::vector<float> as a Qt
    // metatype (the IQ connection above never needs that: it is only ever
    // DirectConnection'd).
    connect(m_client, &P2Client::linkUp, this, [this] {
        m_connected = true;
        // See m_rateChanging's declaration comment: a live rate change is a
        // real stop+restart of this session, but the operator only asked to
        // zoom, so connected() is suppressed for that one round trip.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        if (!wasRateChange) {
            emit connected();
            defineMeters();
            // A new session's needles start from their first reading, not from
            // where the last session's left off.
            for (Receiver& rx : m_receivers)
                rx.sMeter.reset();
            // Likewise the rail's window: counts from before this session
            // must not ride into its first published voltage.
            resetSupplyRailWindow();
        }
        emitSliceState();
        emitPanState();
        // Here as well as in connectRadio(): reconnecting over a live session, the
        // old session's queued linkDown lands after connectRadio()'s report, and
        // RadioModel clears the report on disconnected().
        emit radioSpeakerAudioReported(m_speakerAudioEnabled);
        // A rate change suppresses connected(), but a page opened during
        // its brief link restart still needs the fresh calibration status.
        publishDroopStatus();
        // Real zoom limits (HERMES.md §15.1): otherwise the GUI falls back to a FlexLib
        // model table (5.4 MHz for an unknown "ANAN-G2") and zooms past the 1.536 MHz
        // ceiling into unsampled black bars. Constant per connection (every DDC offers
        // the same rates), so emit on every linkUp, for every open pan.
        for (const Receiver& rx : m_receivers)
            emitPanLimits(rx);
        if (wasRateChange) {
            // Audio was muted in beginRateChange() before this session started (see there).
            // Unmute after a settle window: the rebuilt WdspChannel's AGC/filters/DC
            // blocker start from zero against live RF. The display is unaffected.
            const quint64 generation = m_connectGeneration;
            QTimer::singleShot(kRateChangeAudioSettleMs, this, [this, generation] {
                // Superseded by a newer connect/rate-change/disconnect --
                // that cycle's own beginRateChange()/disconnectRadio() owns
                // muting from here, not this timer.
                if (generation != m_connectGeneration)
                    return;
                for (const Receiver& rx : m_receivers) {
                    QMetaObject::invokeMethod(rx.dsp, "setAudioMuted", Qt::QueuedConnection,
                                              Q_ARG(bool, false));
                }
            });
            resumeAfterRebuild();
        }
    });
    connect(m_client, &P2Client::linkDown, this, [this] {
        m_connected = false;
        if (!m_rateChanging) {
            m_droopCalibrator.stop(false);
            m_droopCalibrator.setLandedRate(0);
            emit disconnected();
        }
    });
    connect(m_client, &P2Client::connectionError, this,
            [this](const QString& reason) { emit connectionError(reason); });
    // Drops reach the readout through the counters snapshot below, which
    // carries the same total on a fixed cadence. Not wired per-drop: the
    // LinkStats tick is deliberately periodic rather than event-driven, so a
    // link that has gone silent still reports.
    connect(m_client, &P2Client::linkCountersUpdated, this,
            &AnanBackend::onLinkCounters);
    // Queued onto this thread, like the snapshot above: the meter seam's
    // signals are emitted from the backend's own thread.
    connect(m_client, &P2Client::supplyRailSampled, this,
            &AnanBackend::onSupplyRailCounts);
    // A gap on a receiver's DDC goes to that receiver's DSP only. Like the IQ
    // connection above, notification and processing run directly on the I/O
    // thread, with the DSP as context. Invalidate its partial FFT before the
    // discontinuous block arrives.
    for (const Receiver& rx : m_receivers) {
        connect(m_client, &P2Client::ddcSequenceGap, rx.dsp,
                [dsp = rx.dsp, ddc = rx.route.ddc](int ddcIndex) {
            if (ddcIndex == ddc) {
                dsp->onSequenceGap();
            }
        }, Qt::DirectConnection);
    }
    connect(m_client, &P2Client::discoveryInfoReceived, this,
            [this](quint8 boardId, quint8 firmwareVer, quint8 numDdc) {
        // See capabilities()'s own comment for where these surface. Reported
        // real, not hardcoded, but numDdc does not touch maxSlices/
        // maxPanadapters: the route table sets those, not a capability the
        // radio gets to raise on our behalf.
        m_discoveredBoardId = boardId;
        m_discoveredFirmwareVer = firmwareVer;
        m_discoveredNumDdc = numDdc;
        m_discoveryInfoReceived = true;
        // The shipped droop defaults are derived from one gateware's filter
        // (AnanDroopDefaults.h) and still applied on a mismatch, so say so. Logged here
        // because connectRadio() zeroes m_discoveredFirmwareVer and the reply fills it
        // later. Keyed on firmwareVer only (board type must not gate behaviour), so a
        // non-Saturn board on the same build number goes unflagged. Informational.
        if (firmwareVer != kDefaultsGatewareVersion) {
            qCWarning(lcAnanDefaults).nospace()
                << "ANAN: radio reports gateware " << firmwareVer
                << ", shipped droop defaults were derived against "
                << kDefaultsGatewareVersion
                << " -- applying them anyway; run the in-app droop sweep if "
                   "the panadapter edges look wrong";
        }
        emit capabilitiesChanged();
    });

    for (const Receiver& rx : m_receivers)
        connectReceiverOutputs(rx.index);
}

void AnanBackend::connectReceiverOutputs(int index)
{
    AnanRxDsp* const dsp = m_receivers[static_cast<std::size_t>(index)].dsp;
    connect(dsp, &AnanRxDsp::pcmReady, this, [this, index](const PcmFrame& frame) {
        const Receiver& rx = m_receivers[static_cast<std::size_t>(index)];
        // A closed receiver's last blocks reach nothing: its slice is gone.
        if (!rx.open) {
            return;
        }
        QByteArray bytes = frame.legacyStereo24();
        if (bytes.isEmpty()) {
            return;
        }
        // THIS SLICE's audio for per-slice consumers (a TCI receiver channel, a
        // decoder), published BEFORE the receiver's audio stage. The seam
        // contract on IRadioBackend::sliceAudioFrameReady is pre-mute, pre-gain
        // and pre-balance, so muting a slice does not stop WSJT-X decoding on it
        // -- the same split a Flex gets from DAX and HL2 makes in its mixer.
        publishLegacySliceAudio(rx.index, bytes);

        // The receiver's own audio stage, applied HERE rather than in the DSP.
        //
        // legacyStereo24() already hands back a fresh buffer this backend owns,
        // and the non-const data() below detaches from anything the slice tap
        // above may still share, so the stage never reaches that tap. Doing it here also keeps the whole stage on one thread with
        // the state the three setters write, so a mute needs no queued hop to
        // take effect on the next block.
        applySliceAudioInPlace(reinterpret_cast<float*>(bytes.data()),
                               static_cast<std::size_t>(bytes.size())
                                   / (2 * sizeof(float)),
                               rx.audioMuted, rx.audioGainPercent,
                               rx.audioPanPercent);
        // This computer's speaker hears every open receiver, mixed; with RX1
        // alone the mixer hands its block back untouched, and that block is
        // published as it is.
        const std::span<const float> samples(reinterpret_cast<const float*>(bytes.constData()),
                                             static_cast<std::size_t>(bytes.size()) / sizeof(float));
        const std::span<const float> mixed = m_speakerMix.push(rx.index, samples);
        if (mixed.data() == samples.data()) {
            publishLegacyAudio(bytes);
        } else if (!mixed.empty()) {
            publishLegacyAudio(QByteArray(reinterpret_cast<const char*>(mixed.data()),
                                          static_cast<qsizetype>(mixed.size_bytes())));
        }
        // The radio's own speaker plays RX1 alone, after the same stage.
        if (&rx == &rx1()) {
            sendSpeakerAudioToRadio(bytes);
        }
    });
    // Ungated by design: the DSP emits on m_ioThread, the thread that emits linkDown,
    // so every frame is delivered before disconnected() (contract rule 6). Only an
    // open receiver's, though: a closed one's pan is gone.
    connect(dsp, &AnanRxDsp::spectrumReady, this, [this, index](const std::vector<float>& binsDbfs) {
        const Receiver& rx = m_receivers[static_cast<std::size_t>(index)];
        if (!rx.open) {
            return;
        }
        std::vector<float> dbm(binsDbfs.size());
        // Attenuation added back, as deskHPSDR does for its panadapter: a
        // signal keeps its level when the operator attenuates, and only the
        // part of the floor that was the ADC overloading actually drops.
        const float attenuation = static_cast<float>(attenuationDbForAdc(rx.adc));
        for (std::size_t i = 0; i < binsDbfs.size(); ++i) {
            dbm[i] = binsDbfs[i] + kUncalibratedDbfsToDbmOffset + attenuation;
        }
        // The droop sweep measures RX1's pan, the one its hook zooms.
        if (&rx == &rx1()) {
            m_droopCalibrator.onSpectrumFrame(dbm);
        }
        emit spectrumFrameReady(rx.index, floatBytes(dbm));
    });
    connect(dsp, &AnanRxDsp::meterUpdate, this,
            [this, index](float dbfs) { onDspMeter(index, dbfs); });
}

AnanBackend::~AnanBackend()
{
    m_droopCalibrator.stop(false);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup/rebuild callback

    // Join the build thread BEFORE deleting the DSPs/m_client: the build lambda's
    // tail posts invokeMethod(dsp/this, ...) after buildChannel() returns, so
    // deleting first is a use-after-free if the app closes mid-build. The wait is
    // bounded by the remaining FFTW planning.
    if (m_dspBuildThread) {
        m_dspBuildThread->quit();
        m_dspBuildThread->wait();
    }
    delete m_dspBuildContext;

    if (m_ioThread) {
        if (m_client)
            QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
        m_ioThread->quit();
        m_ioThread->wait();
    }
    for (Receiver& rx : m_receivers) {
        delete rx.dsp;
        rx.dsp = nullptr;
    }
    delete m_client;
}

AnanBackend::Receiver* AnanBackend::receiverForSlice(int sliceId) noexcept
{
    for (Receiver& rx : m_receivers) {
        if (rx.open && rx.index == sliceId)
            return &rx;
    }
    return nullptr;
}

AnanBackend::Receiver* AnanBackend::receiverForPan(const QString& panId) noexcept
{
    for (Receiver& rx : m_receivers) {
        if (rx.open && rx.panId.compare(panId, Qt::CaseInsensitive) == 0)
            return &rx;
    }
    return nullptr;
}

P2Client::Params AnanBackend::sessionParams() const
{
    P2Client::Params params = m_pendingParams;
    params.activeDdcs.clear();
    for (const Receiver& rx : m_receivers) {
        // A receiver whose open is still building its chain is not in the
        // session yet: an earlier close's resends must not enable its DDC.
        if (!rx.open || rx.openBuild != 0)
            continue;
        params.activeDdcs.push_back(
            DdcConfig{.ddc = rx.route.ddc, .rateKsps = rx.rateKsps, .adcIndex = rx.adc});
    }
    return params;
}

RadioCapabilities AnanBackend::capabilities() const
{
    RadioCapabilities c;
    c.cwTextControl = std::nullopt;
    c.hostCwKeyingControl = std::nullopt;
    c.broadcastFmReceive = std::nullopt;
    c.family = QStringLiteral("anan");
    c.cwReceiveControl = std::nullopt;
    c.panDisplayControl = PanDisplayControl{{PanDisplayField::Geometry, PanDisplayField::RfGain,
        PanDisplayField::FrameRate, PanDisplayField::Average, PanDisplayField::WeightedAverage,
        PanDisplayField::SpectrumPoints}, {PanGeometryComponent::Center, PanGeometryComponent::Bandwidth}};
    c.sessionControl = std::nullopt;  // no SmartSDR client session to bootstrap
    // A pan is a receiver: creating one opens RX2 on its own DDC, and the pan
    // and slice exist when createPanadapter() returns.
    c.panLifecycleControl = PanLifecycleControl{PanLifecycleControl::Creation::Native,
        {PanLifecycleAction::Create, PanLifecycleAction::Remove}};
    // No Flex stream plane: receive audio is native and graphic EQ is host-side.
    c.audioStreamControl = std::nullopt;
    c.radioGraphicEqControl = std::nullopt;
    // No setTune() implementation, so no tune generator to select a mode on.
    c.twoToneGenerator = std::nullopt;
    // Gateware is reported, but no support tier reads an ANAN board identity yet.
    c.boardIdentity = std::nullopt;
    c.firmwareUpdateSource = std::nullopt;  // gateware number; no published list covers it.
    // No VOX, monitor or speech-processor setters. PROC would be the host
    // ClientComp, but canTransmit is false below, so nothing serves it and the
    // drop notice correctly stands.
    c.voxControl = std::nullopt;
    c.speechProcessorControl = std::nullopt;
    c.txMonitorControl = std::nullopt;
    // The radio's speaker output: setLineoutMute() sends it silence and
    // setLineoutGain() scales the samples this host feeds it.
    c.lineoutControl = RadioCapabilities::LineoutControl{};
    c.radioSetupControl = RadioSetupControl{{RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
    // No client owner is declared for "Show TX in Waterfall" on an ANAN.
    c.txWaterfallClientFlag = std::nullopt;
    // The panadapter dBm axis is dBFS with a dBm label: kUncalibratedDbfsToDbmOffset
    // is 0.0f and bin levels depend on window/normalisation, unverified against a
    // known input. Internally consistent, but not comparable: never publish as a
    // spot, compare with another station, or use as an absolute threshold. The
    // S-meter (kSMeterDbmOffset) is separate and follows deskHPSDR's scaling.
    PanAmplitudeModel amplitude;
    amplitude.calibratedDbm = false;
    // The bins are raw dBFS + 0, so they are ABSOLUTE: nothing in them depends on
    // the display reference level, and the auto-floor loop converges.
    amplitude.binsAbsolute = true;
    // No register holds a display range: the client is its only memory.
    amplitude.clientPersistsDbmRange = true;
    c.panAmplitude = amplitude;
    // No measured squelch map; the SQL line and Auto SQL keep Flex's scale.
    c.squelchLevelScale = legacyDbmSquelchScale();

    // Span follows the sample rate: it snaps by ratio to one of the six DDC rates
    // (no continuous zoom); panBandwidthLimitsChanged clamps to that list's ends.
    // radioWide is false: each receiver's DDC runs its own rate, so no span
    // budget is shared (unlike the HL2's one DDC in front of every receiver) (#5725).
    PanSpanModel span;
    span.followsSampleRate = true;
    span.radioWide = false;
    c.panSpanModel = span;
    c.hasAgcThreshold = true; // Host receiver DSP implements threshold/off gain.
    // agcKnobDb()'s scale, so the AGC-T tooltips show the gain, not the knob.
    c.agcGainScale = RadioCapabilities::AgcGainScale{kAgcKnobMinDb, kAgcKnobDbPerStep};
    c.manufacturer = QStringLiteral("Apache Labs");
    c.model = QStringLiteral("ANAN-G2");
    // The session takes its receive front-end options from AnanSettings
    // (populateFamilyParams()), so the setup page that edits them applies.
    c.adcFrontEnd = RadioCapabilities::AdcFrontEnd{};

    // ---- PA telemetry ----
    // The DC supply rail, published as RAD:+13.8A from every status packet's
    // count (onSupplyRailCounts()). Claimed on a bench measurement, not on the
    // input's label: see kSupplyRailVoltsPerCount for the reading and for why
    // the input is not the one the spec calls "supply voltage".
    c.hasSupplyVoltageTelemetry = true;
    //
    // There is NO PA temperature in this protocol. The radio's whole analog
    // payload is six fields and none of them is a temperature, so the readout
    // can never resolve and is withdrawn rather than left showing a placeholder.
    // hasPaTemperatureTelemetry stays false on its own (nothing is received);
    // this says the stronger thing -- that nothing CAN be.
    c.paTelemetryAudit = RadioCapabilities::PaTelemetryAudit{
        /*temperatureAbsent=*/true};
    // One slice per receiver, each with its pan: slices come with pans, never
    // on their own.
    c.canCreateSlices = false;
    c.maxSlices = static_cast<int>(kReceiverRoutes.size());
    c.maxPanadapters = static_cast<int>(kReceiverRoutes.size());
    for (const int ksps : kDdc0RatesKsps)
        c.sampleRatesHz.append(ksps * 1000);
    // The desktop range is not reported: the headless span below is a measured
    // tuning bound, but the front end's sensitivity and roll-off across it are
    // not. RadioCapabilities.h's own convention: both zero means "not reported".
    c.tuningMinHz = 0.0;
    c.tuningMaxHz = 0.0;
    // Headless tuning bounds: the span a G2 (gateware 27) was measured to tune
    // and demodulate, kept below phaseWord()'s alias at kDspClockHz/2. A tuning
    // bound, not a sensitivity claim: front-end roll-off is unmeasured, so the
    // GUI range above stays unreported. The span follows from the 122.88 MHz
    // clock every Saturn gateware shares. The pan is the DDC, centered on the
    // slice, so a tune always moves it: PreservePan cannot hold here.
    c.sliceFrequencyControl = {SliceFrequencyControl::Authority::Engine,
                               kHeadlessTuneMinHz, kHeadlessTuneMaxHz};
    // Exactly the menu's modes, each its own canonical spelling: setSliceMode()
    // publishes the offered spelling back, and the receive target releases a
    // pending mode only on an equal observation, so an alias here would wedge it.
    c.receiveModeControl = ReceiveModeControl{SliceFrequencyControl::Authority::Engine,
                                              publishedModeStrings()};
    // Carrier-relative subdomains of the WDSP passband (48 kHz DSP rate, cuts
    // applied ahead of the detector and echoed unchanged). CW/CWL add the BFO,
    // clamped to 6000 Hz in setCwPitch(), so their edges stay inside +-12000 Hz.
    c.receiveFilterControl = ReceiveFilterControl{SliceFrequencyControl::Authority::Engine, {
        {QStringLiteral("USB"), 0, 11990, 10, 12000, 10, 12000},
        {QStringLiteral("DIGU"), 0, 11990, 10, 12000, 10, 12000},
        {QStringLiteral("LSB"), -12000, -10, -11990, 0, 10, 12000},
        {QStringLiteral("DIGL"), -12000, -10, -11990, 0, 10, 12000},
        {QStringLiteral("AM"), -12000, -10, 10, 12000, 20, 24000},
        {QStringLiteral("SAM"), -12000, -10, 10, 12000, 20, 24000},
        {QStringLiteral("DSB"), -12000, -10, 10, 12000, 20, 24000},
        {QStringLiteral("FM"), -12000, -10, 10, 12000, 20, 24000},
        {QStringLiteral("CW"), -6000, -10, 10, 6000, 20, 12000},
        {QStringLiteral("CWL"), -6000, -10, 10, 6000, 20, 12000}}};
    // The host DSP features this backend runs and echoes: AGC, the impulse
    // blankers, WDSP's ANR and RIT (a detector shift), in every offered mode.
    // ANF, APF and the manual notch are not run here, so they are not declared.
    {
        using Feature = ReceiveDspControl::Feature;
        ReceiveDspControl dsp;
        dsp.modes = publishedModeStrings();
        dsp.features = {Feature::Agc, Feature::Nb, Feature::Nr, Feature::Rit};
        dsp.authority = SliceFrequencyControl::Authority::Engine;
        dsp.maximumRitOffsetHz = kMaxRitOffsetHz;
        c.receiveDspControl = dsp;
    }
    // setPanRfGain() drives the step attenuator negated, and panRfGainChanged
    // reports what it applied.
    c.receivePanRfGainControl = ReceivePanRfGainControl{SliceFrequencyControl::Authority::Engine,
                                                        -kMaxStepAttenuationDb, 0, 1};
    // The G2's speaker plays RX1, sent from this host (sendSpeakerAudioToRadio()).
    c.radioSpeakerControl = RadioSpeakerControl{SliceFrequencyControl::Authority::Engine};
    // Gain and mute both act on this receiver's audio and are applied by this
    // backend, which is what the record promises. Engine authority: the state is
    // ours, the radio echoes nothing back, and there is no register to read.
    c.receiveAudioControl = ReceiveAudioControl{SliceFrequencyControl::Authority::Engine};
    c.receiveAudioExport = std::nullopt; // native DAX export not qualified for this backend
    c.receiveAudioPlayout = std::nullopt; // host primary-queue timing defaults apply
    c.dspParameterReset = std::nullopt;
    c.receivePanCenterControl = std::nullopt; // center also retunes the slice
    c.receivePanBandwidthControl = ReceivePanRangeControl{SliceFrequencyControl::Authority::Engine,
                                                         48'000, 1'536'000};
    c.canTransmit = false;         // P2Client has no PTT capability -- see class comment
    c.txPowerMaxWatts = 0.0;
    c.hostModulates = true;        // client-side WDSP, like the HL2
    c.takesTxAudioOverSeam = true; // moot while canTransmit is false
    // Host-modulated like the HL2, so when this backend gains a drive path it
    // will own the register and report intent, not a readback (#5518). Declared
    // rather than left absent because the ownership answer is already known; it
    // populates no TransmitDelta::rfPower today, so nothing publishes drive yet.
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine, /*tunePowerAppliesLive=*/false};
    c.hasRadioPttReadback = false; // no PTT at all, so no readback either
    c.radioPttObservation = std::nullopt;
    c.hasTuner = false;            // G2 has no internal ATU (Apache Labs spec)
    c.hasTunerMemories = false;    // no internal ATU, so no tuner-memory surface
    c.hasAmplifier = false;
    c.hasRadioSideDsp = false;     // DSP is engine-side (AnanRxDsp), not firmware
    c.hasAudioPeakingFilter = false; // no firmware APF verb on this path
    c.hasHostNoiseBlanker = true;  // WDSP ANB on the raw IQ, in AnanRxDsp
    c.radioOwnsDbmScale = false;   // client computes it from raw IQ
    c.hasDdcPanEdgeRolloff = true; // see RadioCapabilities.h's own comment
    c.backendPanAveraging = BackendPanAveraging{kMsPerAverageStep, false, {}, {}}; // AnanPanAnalyzer
    c.panFrameRateShaping = std::nullopt;  // no client owner is declared for FFT FPS
    // No protocol field holds a waterfall rate: the client keeps it.
    c.panWaterfallRateShaping =
        PanWaterfallRateShaping{/*clientPersistsWaterfallRate*/ true};
    // No band/segment zoom: the protocol carries no per-pan zoom flag.
    c.panZoomModes = std::nullopt;
    c.persistsMemories = false;    // default; stated explicitly
    // The radio reports no frequency, mode, output level or AGC of its own (the
    // AGC runs in AnanRxDsp), so the client is their only memory, per radio.
    // With hasAgcThreshold, Agc also keeps each receiver's AGC-off level.
    // Receivers: each receiver's RIT and noise reduction, which run here and
    // nowhere else, and RX2's own dial, mode and AGC and whether it was open.
    c.clientSettingsDomains = RadioCapabilities::ClientSettingsDomain::RfGain
                              | RadioCapabilities::ClientSettingsDomain::Tuning
                              | RadioCapabilities::ClientSettingsDomain::ReceiveOutputLevel
                              | RadioCapabilities::ClientSettingsDomain::Agc
                              | RadioCapabilities::ClientSettingsDomain::Receivers;
    c.hostDroopCalibration = true; // AnanDroopCorrection.h -- real DDC0 roll-off,
                                    // corrected client-side via AnanDroopCalibrator
    c.extensionNamespaces = {QStringLiteral("anan")};  // "frontend.set", "droop.apply" -- see invokeExtension()
    // From this session's own Discovery reply (P2Client::discoveryInfoReceived()).
    // Absent until it lands ("not reported yet", like tuningMinHz/MaxHz);
    // capabilitiesChanged() fires when it does. numDdc is informational:
    // maxSlices/maxPanadapters come from kReceiverRoutes, whose DDCs every G2
    // reports (Gap C, accepted for this G2-only phase on #6697).
    if (m_discoveryInfoReceived) {
        QVariantMap anan;
        anan[QStringLiteral("gatewareVersion")] = m_discoveredFirmwareVer;
        anan[QStringLiteral("numDdc")] = m_discoveredNumDdc;
        anan[QStringLiteral("boardId")] = m_discoveredBoardId;
        c.extensions[QStringLiteral("anan")] = anan;
    }
    return c;
}

void AnanBackend::applyRestoredState(const RestoredRadioState& state)
{
    // A missing document resets both ADCs on a same-family radio swap.
    // RadioStateMemory owns persistence; validate its opaque extension here.
    const QJsonObject gain = state.extension.value(QStringLiteral("rfGain")).toObject();
    m_pendingParams.adc0AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc0AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);
    m_pendingParams.adc1AttenuationDb = std::clamp(
        gain.value(QStringLiteral("adc1AttenuationDb")).toInt(0), 0, kMaxStepAttenuationDb);

    // Unconditional, like the ADC values above, so a same-family backend reuse
    // cannot carry radio A's dial or level onto radio B. A rejected value leaves
    // the sentinel, and connectRadio() falls to the first-connect default.
    m_restoredFreqHz = isRestorableFrequencyHz(state.rfFrequencyHz)
        ? state.rfFrequencyHz : 0.0;
    m_restoredMode = isOfferedModeString(state.mode) ? canonicalOfferedMode(state.mode) : QString();
    m_lineoutGainPercent = state.receiveOutputLevelPct >= 0
        ? state.receiveOutputLevelPct : kDefaultLineoutGainPercent;
    // RX1's AGC too, live: connectRadio() builds the DSP config from these. A
    // knob value outside 0..100 is not restored and the default stands; a mode
    // name setSliceAgc() would not set reads as medium, as it does there.
    Receiver& rx = rx1();
    rx.agcMode = agcModeFromName(state.agcMode);
    rx.agcThreshold = state.agcThreshold >= 0 && state.agcThreshold <= 100
        ? state.agcThreshold : kDefaultAgcThreshold;
    const int offLevel = state.agcOffLevels.value(0, -1);
    rx.agcOffLevel = offLevel >= 0 && offLevel <= 100 ? offLevel : kDefaultAgcOffLevel;
    // The Receivers domain: entry i of "receivers" is receiver i (receiverEntryJson()).
    // A missing entry reads as an empty one, so everything in it is the default.
    const QJsonArray remembered = state.extension.value(QStringLiteral("receivers")).toArray();
    // RX1's RIT and NR, unconditional like the rest.
    restoreRitAndNr(rx, remembered.at(0).toObject());

    // Every receiver after RX1: closed, with no dial of its own, at the default
    // AGC, RIT and NR, unless its entry says otherwise. The reset comes first so
    // radio B never opens radio A's RX2.
    for (std::size_t i = 1; i < m_receivers.size(); ++i) {
        Receiver& other = m_receivers[i];
        other.open = false;
        other.reopenFailed = false;
        other.freqHz = 0.0;   // seeded from RX1 when it opens
        other.agcMode = 3;
        other.agcThreshold = kDefaultAgcThreshold;
        const int level = state.agcOffLevels.value(static_cast<qsizetype>(i), -1);
        other.agcOffLevel = level >= 0 && level <= 100 ? level : kDefaultAgcOffLevel;
        const QJsonObject entry = remembered.at(static_cast<qsizetype>(i)).toObject();
        restoreRitAndNr(other, entry);
        // Without a dial and an offered mode there is no receiver to come back
        // to. The mode is taken as RX1's is: an alias under its offered spelling.
        const double hz = entry.value(QStringLiteral("freqHz")).toDouble();
        const QString saved = entry.value(QStringLiteral("mode")).toString();
        if (!isRestorableFrequencyHz(hz) || !isOfferedModeString(saved))
            continue;
        const QString mode = canonicalOfferedMode(saved);
        other.freqHz = hz;
        if (mode.compare(other.mode, Qt::CaseInsensitive) != 0) {
            const auto [lo, hi] = defaultPassbandForMode(mode);
            other.filterLowHz = lo;
            other.filterHighHz = hi;
        }
        other.mode = mode;
        other.agcMode = agcModeFromName(entry.value(QStringLiteral("agcMode")).toString());
        const int threshold = entry.value(QStringLiteral("agcThreshold")).toInt(-1);
        other.agcThreshold = threshold >= 0 && threshold <= 100 ? threshold : kDefaultAgcThreshold;
        other.open = entry.value(QStringLiteral("open")).toBool(false);
    }
}

RestoredRadioState AnanBackend::currentOperatingState() const
{
    RestoredRadioState state;
    // RX1's live dial and level, not the restored ones. A level of 0 is
    // reported: a silenced speaker is a choice, and "nothing to remember" is -1.
    const Receiver& rx = rx1();
    state.rfFrequencyHz = rx.freqHz;
    state.mode = rx.mode;
    state.receiveOutputLevelPct = m_lineoutGainPercent;
    state.agcMode = agcModeName(rx.agcMode);
    state.agcThreshold = rx.agcThreshold;
    // One per receiver, RX1 first.
    for (const Receiver& each : m_receivers)
        state.agcOffLevels.append(each.agcOffLevel);
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{
        {QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("adc0AttenuationDb"), m_pendingParams.adc0AttenuationDb},
            {QStringLiteral("adc1AttenuationDb"), m_pendingParams.adc1AttenuationDb}}}};
    // Entry i is receiver i, up to the last receiver with a dial of its own, so
    // a radio whose RX2 was never opened keeps RX1's one entry.
    std::size_t last = 0;
    for (std::size_t i = 1; i < m_receivers.size(); ++i) {
        if (m_receivers[i].freqHz > 0.0)
            last = i;
    }
    QJsonArray receivers;
    for (std::size_t i = 0; i <= last; ++i)
        receivers.append(receiverEntryJson(m_receivers[i]));
    state.extension.insert(QStringLiteral("receivers"), receivers);
    return state;
}

QJsonObject AnanBackend::receiverEntryJson(const Receiver& rx)
{
    QJsonObject entry{
        {QStringLiteral("ritOn"), rx.ritEnabled},
        {QStringLiteral("ritHz"), rx.ritOffsetHz},
        {QStringLiteral("nrOn"), rx.nrEnabled},
        {QStringLiteral("nrLevel"), rx.nrLevel}};
    if (rx.index == 0)
        return entry;   // RX1's dial, mode and AGC are the document's own fields
    // Whether it is open, so the next connect reopens it where it was. One
    // whose chain failed at connect still counts: the failure may not recur.
    entry.insert(QStringLiteral("open"), rx.open || rx.reopenFailed);
    if (rx.freqHz > 0.0) {
        entry.insert(QStringLiteral("freqHz"), rx.freqHz);
        entry.insert(QStringLiteral("mode"), rx.mode);
        entry.insert(QStringLiteral("agcMode"), agcModeName(rx.agcMode));
        entry.insert(QStringLiteral("agcThreshold"), rx.agcThreshold);
    }
    return entry;
}

void AnanBackend::restoreRitAndNr(Receiver& rx, const QJsonObject& entry)
{
    const int ritHz = entry.value(QStringLiteral("ritHz")).toInt(0);
    const bool ritOk = ritHz >= -kMaxRitOffsetHz && ritHz <= kMaxRitOffsetHz;
    rx.ritOffsetHz = ritOk ? ritHz : 0;
    rx.ritEnabled = ritOk && entry.value(QStringLiteral("ritOn")).toBool(false);
    const int nrLevel = entry.value(QStringLiteral("nrLevel")).toInt(-1);
    rx.nrLevel = nrLevel >= 0 && nrLevel <= 100 ? nrLevel : kDefaultNrLevel;
    rx.nrEnabled = entry.value(QStringLiteral("nrOn")).toBool(false);
}

void AnanBackend::connectRadio(const RadioConnectRequest& request)
{
    if (!m_busyProbe)
        m_busyProbe = new ReconnectBusyProbe(this);
    m_busyProbe->cancel();
    const QHostAddress host(request.host);
    if (request.automaticReconnect && !host.isNull()) {
        // The previous session first, as start() would have stopped it: a client
        // left running after a connect timeout keeps sending this radio its
        // high-priority settings, and a busy answer below starts nothing new.
        // Abandoned, not stopped: whether another client has the radio is what
        // the probe is about to find out, and a run=0 from us would stop it.
        if (m_client)
            QMetaObject::invokeMethod(m_client, "abandon", Qt::BlockingQueuedConnection);
        m_busyProbeRequest = request;
        const auto discovery = buildDiscovery();
        m_busyProbe->start(host, kRadioPort,
            QByteArray(reinterpret_cast<const char*>(discovery.data()),
                       static_cast<qsizetype>(discovery.size())),
            [](std::span<const std::uint8_t> datagram) -> std::optional<bool> {
                const auto reply = parseDiscoveryReply(datagram);
                return reply ? std::optional<bool>(reply->streaming) : std::nullopt;
            },
            [this](std::optional<bool> streaming) { finishBusyProbe(streaming); });
        return;
    }
    m_busyWarned = false;
    startConnect(request);
}

void AnanBackend::finishBusyProbe(std::optional<bool> streaming)
{
    if (!ReconnectBusyProbe::mayProceed(streaming)) {
        qCInfo(lcAnan) << "ANAN: not reconnecting to" << m_busyProbeRequest.host
                       << "- another client has the radio";
        if (!m_busyWarned) {
            m_busyWarned = true;
            emit configurationWarning(QStringLiteral(
                "ANAN: the radio at %1 is in use by another client. AetherSDR will "
                "reconnect once it is free, without taking it over.").arg(m_busyProbeRequest.host));
        }
        return;   // RadioModel's reconnect timer asks again
    }
    m_busyWarned = false;
    startConnect(m_busyProbeRequest);
}

void AnanBackend::startConnect(const RadioConnectRequest& request)
{
    const QHostAddress hostAddr(request.host);
    if (hostAddr.isNull()) {
        emit connectionError(QStringLiteral("ANAN: invalid host '%1'").arg(request.host));
        return;
    }
    if (m_connected)
        disconnectRadio();

    // A fresh connect (possibly to a different host) must not keep reporting
    // a prior radio's discovered identity until its own reply lands -- see
    // capabilities()'s own comment.
    m_discoveryInfoReceived = false;
    m_discoveredBoardId = 0;
    m_discoveredFirmwareVer = 0;
    m_discoveredNumDdc = 0;

    // Per-radio identity for RadioSettingsScope (droop calibration -- see
    // invokeExtension()'s "droop.apply" handler). Set BEFORE the seed load
    // just below, and before anything else in this function needs it,
    // matching Hl2Backend's own m_radioSerial assignment ordering.
    m_radioSerial = request.serial;
    {
        // Forget the PREVIOUS radio's tables before seeding this one's. The
        // seed below only inserts, and each DSP is constructed once for the
        // lifetime of this backend -- so without this, connecting a second,
        // uncalibrated G2 in the same session renders it through the first
        // one's per-bin corrections. See
        // AnanRxDsp::clearDroopCorrectionTables().
        for (const Receiver& rx : m_receivers) {
            QMetaObject::invokeMethod(rx.dsp, "clearDroopCorrectionTables",
                                      Qt::QueuedConnection);
        }

        // Every receiver gets the same tables: every DDC is the same gateware
        // block (AnanReceiverRoutes.h), so one rate's roll-off is the same on each.
        auto pushTable = [this](int rateKsps, const DroopCorrectionTable& t) {
            for (const Receiver& rx : m_receivers) {
                QMetaObject::invokeMethod(rx.dsp, "setDroopCorrectionTable",
                    Qt::QueuedConnection, Q_ARG(int, rateKsps),
                    Q_ARG(std::vector<float>, std::vector<float>(t.begin(), t.end())));
            }
        };

        // Shipped defaults FIRST, so an uncalibrated radio is corrected on first
        // connect (one gateware-derived curve for all rates, AnanDroopDefaults.h). Both
        // lists come from kDdc0RatesKsps, so the null test is a structural guard only.
        for (const int rateKsps : defaultDroopRatesKsps()) {
            if (const DroopCorrectionTable* t = defaultDroopTableForRate(rateKsps))
                pushTable(rateKsps, *t);
        }

        // Then THIS radio's own bench calibration on top, per rate. An
        // operator who measured their own hardware always outranks a shipped
        // default, and a partial sweep only overrides the rates it actually
        // covered rather than wiping the rest back to the default.
        const auto tables = AnanDroopCalibrator::loadTables(
            RadioSettingsScope(QStringLiteral("anan"), m_radioSerial));
        for (auto it = tables.constBegin(); it != tables.constEnd(); ++it)
            pushTable(it.key(), it.value());
    }

    m_pendingParams.host = request.host;
    Receiver& rx = rx1();
    rx.rateKsps = request.params.value(QStringLiteral("anan.ddc0RateKsps"), 48).toInt();
    // Connect-time-only ADC options -- see P2Client::Params' own comment
    // for why none of these have a live setter. Defaults match
    // AnanSettings' own defaults, so a caller that never populated these
    // params (a picker/auto-reconnect connect, same reasoning as
    // ddc0RateKsps above) still gets sane values, not zeroed-out ones.
    m_pendingParams.ditherEnabled =
        request.params.value(QStringLiteral("anan.ditherEnabled"), true).toBool();
    m_pendingParams.randomEnabled =
        request.params.value(QStringLiteral("anan.randomEnabled"), true).toBool();
    rx.adc = request.params.value(QStringLiteral("anan.ddc0AdcIndex"), rx.route.adc).toInt() == 1
        ? 1 : 0;
    m_pendingParams.bypassAdc0Filters =
        request.params.value(QStringLiteral("anan.bypassAdc0Filters"), true).toBool();
    m_pendingParams.bypassAdc1Filters =
        request.params.value(QStringLiteral("anan.bypassAdc1Filters"), true).toBool();
    // FALSE by default, unlike the options above, and for the opposite reason:
    // their default is the hardware's shipped state, while this one's is "the
    // operator has not asked for an outbound stream". A connect that never
    // populated this param must not start one.
    m_pendingParams.speakerAudioEnabled =
        request.params.value(QStringLiteral("anan.speakerAudioEnabled"), false).toBool();
    // Live once connected ("frontend.set" below), and seeded here so the
    // session's first High Priority packet already carries them. FALSE by
    // default: both defaults are the bytes this client sent before either
    // option existed.
    m_pendingParams.bandFiltersEnabled =
        request.params.value(QStringLiteral("anan.bandFiltersEnabled"), false).toBool();
    m_pendingParams.speakerAmpMuted =
        request.params.value(QStringLiteral("anan.speakerAmpMuted"), false).toBool();

    // An explicit connect frequency, then this radio's restored one, then the
    // first-connect default.
    if (request.params.contains(QStringLiteral("anan.rxFrequencyHz"))) {
        rx.freqHz = request.params.value(QStringLiteral("anan.rxFrequencyHz")).toDouble();
    } else if (m_restoredFreqHz > 0.0) {
        rx.freqHz = m_restoredFreqHz;
    } else {
        rx.freqHz = kFirstConnectFrequencyHz;
    }
    // Before the DSP configs are built below, which read the mode. No restored
    // mode means the default, never the previous radio's mode. The passband
    // resets only on a mode change, as in setSliceMode().
    const QString mode = m_restoredMode.isEmpty()
        ? QString::fromLatin1(kFirstConnectMode) : m_restoredMode;
    if (mode.compare(rx.mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        rx.filterLowHz = lo;
        rx.filterHighHz = hi;
    }
    rx.mode = mode;

    // Every other receiver starts at the connect form's rate too, and one that
    // comes back open (applyRestoredState()) with no dial of its own starts
    // where RX1 is.
    for (std::size_t i = 1; i < m_receivers.size(); ++i) {
        Receiver& other = m_receivers[i];
        other.rateKsps = rx.rateKsps;
        other.openBuild = 0;
        if (other.open && other.freqHz <= 0.0) {
            other.freqHz = rx.freqHz;
            other.mode = rx.mode;
            other.filterLowHz = rx.filterLowHz;
            other.filterHighHz = rx.filterHighHz;
        }
    }
    // The detector shift (CW BFO and a restored RIT) for each DSP the session
    // builds below: AnanRxDsp keeps it across configure(). A closed receiver
    // gets its own when openReceiver() pushes its mode and filter.
    for (const Receiver& receiver : m_receivers) {
        if (!receiver.open)
            continue;
        QMetaObject::invokeMethod(receiver.dsp, "setShift", Qt::QueuedConnection,
            Q_ARG(double, detectorShiftHz(receiver.mode, m_cwPitchHz,
                                          receiver.ritEnabled, receiver.ritOffsetHz)));
    }
    m_activeReceiver = rx.index;
    m_openingReceiver = -1;
    for (const Receiver& receiver : m_receivers)
        m_speakerMix.setInputActive(receiver.index, receiver.open);

    m_speakerAudioEnabled = m_pendingParams.speakerAudioEnabled;
    emit radioSpeakerAudioReported(m_speakerAudioEnabled);
    for (Receiver& receiver : m_receivers)
        prepareDspConfig(receiver);

    ++m_connectGeneration;
    beginDspSetup();
}

void AnanBackend::prepareDspConfig(Receiver& rx)
{
    AnanRxDsp::Config& config = rx.pendingDspConfig;
    config = AnanRxDsp::Config{};
    config.inputSampleRateHz = rx.rateKsps * 1000;
    config.audioSampleRateHz = 24000;
    config.dspBlockSize = 1024;
    // The panel's width in points (setPanPixelWidth()), or one point per
    // droop-table entry until the GUI has reported one; the analyzer's FFT
    // behind them is larger (see AnanPanAnalyzer). spectrumFps keeps Config's
    // default until RadioModel pushes the operator's rate through
    // setPanFrameRate().
    config.panPoints = rx.panPoints;
    // The AGC as applyRestoredState() set it for this radio, and the NB request
    // this backend retains across reconnects. emitSliceState() also supplies
    // them if a different radio requires a fresh slice.
    applyLiveRxState(rx, config);
}

void AnanBackend::applyLiveRxState(const Receiver& rx, AnanRxDsp::Config& config) const
{
    config.mode = modeFromString(rx.mode);
    config.filterLowHz = static_cast<double>(rx.filterLowHz) + cwBfoHz(rx);
    config.filterHighHz = static_cast<double>(rx.filterHighHz) + cwBfoHz(rx);
    config.agcMode = rx.agcMode;
    config.maximumAgcGainDb = agcKnobDb(rx.agcThreshold);
    config.agcFixedGainDb = agcKnobDb(rx.agcOffLevel);
    config.noiseBlanker = AetherSDR::toWdsp(rx.nbKind);
    config.noiseBlankerLevel = rx.nbLevel;
    config.noiseBlankerFill = AetherSDR::toWdsp(rx.nbFill);
    config.noiseReduction = rx.nrEnabled;
    config.noiseReductionLevel = rx.nrLevel;
}

void AnanBackend::beginDspSetup()
{
    const quint64 generation = m_connectGeneration;
    m_dspSetupError.clear();
    m_dspSetupPending = 0;
    for (const Receiver& rx : m_receivers)
        m_dspSetupPending += rx.open ? 1 : 0;
    for (const Receiver& rx : m_receivers) {
        if (rx.open)
            startDspBuild(rx, generation, &AnanBackend::finishDspSetup);
    }
}

void AnanBackend::startDspBuild(const Receiver& rx, quint64 token, DspBuildDone done)
{
    // Build on m_dspBuildThread, not m_ioThread: buildChannel() can take ~19 s cold
    // (FFTW PATIENT), and a disconnect/close in that window must not block on a
    // BlockingQueuedConnection to m_client queued behind configure().
    AnanRxDsp* const dsp = rx.dsp;
    const AnanRxDsp::Config cfg = rx.pendingDspConfig;
    const int index = rx.index;

    // Seed the requested startup state before the off-thread build. The
    // install path reapplies the object's current state so operator edits made
    // during a build win; without this seed it would reapply the Config member
    // defaults over cfg instead.
    QMetaObject::invokeMethod(dsp, [this, dsp, index, token, cfg, done]() {
        dsp->beginInitialBuild(cfg);

        QMetaObject::invokeMethod(m_dspBuildContext, [this, dsp, index, token, cfg, done]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP configure failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(dsp, [dsp, r = std::move(result)]() mutable {
                dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, index, token, ok, errStr, done]() {
                (this->*done)(index, token, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishDspSetup(int index, quint64 generation, bool ok, const QString& error)
{
    // A newer connectRadio() or a disconnectRadio() happened while
    // configure() was running on the I/O thread -- this result is stale.
    if (generation != m_connectGeneration)
        return;

    // One call per open receiver's build; the session starts after the last.
    // Another receiver's failure leaves it closed and the session to RX1, and
    // is reported as the mid-session open's is.
    if (!ok && index != rx1().index) {
        Receiver& other = m_receivers[static_cast<std::size_t>(index)];
        qCWarning(lcAnan).noquote() << QStringLiteral("ANAN: RX%1 not reopened -- %2")
                                           .arg(index + 1).arg(error);
        other.open = false;
        other.reopenFailed = true;
        m_speakerMix.setInputActive(other.index, false);
        emit sliceLifecycleFailed(QStringLiteral("create"), other.index, error);
    } else if (!ok && m_dspSetupError.isEmpty()) {
        m_dspSetupError = error.isEmpty() ? QStringLiteral("unknown error") : error;
    }
    if (--m_dspSetupPending > 0)
        return;

    if (!m_dspSetupError.isEmpty()) {
        emit connectionError(QStringLiteral("ANAN: DSP configure failed: %1")
                             .arg(m_dspSetupError));
        // Only ever reached from connectRadio()'s first-connect path now --
        // beginRateChange() builds the new channel off-thread and never
        // calls beginDspSetup()/finishDspSetup() -- so m_rateChanging is
        // always false here. Kept for symmetry with
        // startP2ClientSession()'s own failure handling rather than because
        // this branch can currently observe a rate change in flight.
        const bool wasRateChange = m_rateChanging;
        m_rateChanging = false;
        emitPanState();
        if (wasRateChange)
            retryPendingRateChange();
        return;
    }

    startP2ClientSession(generation);
}

void AnanBackend::startP2ClientSession(quint64 generation)
{
    // Rebuilt here rather than at connect, because this is the one place BOTH a
    // first connect and a rate change pass through. A converter carries filter
    // state, and the audio either side of a DSP rebuild is not one continuous
    // stream, so carrying that state across would ring the tail of the old rate
    // into the head of the new one.
    resetSpeakerResamplers();
    // The mix's queues are the last session's audio, for the same reason.
    m_speakerMix.reset();
    const P2Client::Params params = sessionParams();
    // Each open receiver's tune, pushed once the session is up (below).
    std::vector<std::pair<int, double>> tunes;
    for (const Receiver& rx : m_receivers) {
        if (rx.open && rx.freqHz > 0.0)
            tunes.emplace_back(rx.route.ddc, rx.freqHz);
    }
    const bool isRateChange = m_rateChanging;
    QMetaObject::invokeMethod(m_client, [this, generation, params, tunes, isRateChange]() {
        const bool started = isRateChange
            ? m_client->start(params, kRateChangeConnectTimeoutMs)
            : m_client->start(params);
        if (!started) {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (generation != m_connectGeneration)
                    return;
                emit connectionError(QStringLiteral("ANAN: could not open the UDP socket"));
                if (m_pendingBandwidthKsps != 0) {
                    // Same retry-window fix as finishRateChange()'s failure
                    // branch (see its comment) -- a socket-open failure
                    // right after a background rebuild can hit the exact
                    // same hazard: clearing m_rateChanging here, before the
                    // queued zoom retries, would let this failed attempt's
                    // own teardown emit an unsuppressed connected()/
                    // disconnected() pair before the retry re-arms it.
                    retryPendingRateChange();
                } else {
                    m_rateChanging = false;
                    emitPanState();
                }
            }, Qt::QueuedConnection);
            return;
        }
        // Push the operator's current tune once the session is up.
        // linkUp() (P2Client's own signal, connected in the constructor)
        // is what actually fires connected()/emitSliceState()/emitPanState()
        // once the first genuine IQ frame arrives -- not this call.
        for (const auto& [ddc, hz] : tunes)
            m_client->setDdcFrequencyHz(ddc, hz);
    }, Qt::QueuedConnection);
}

void AnanBackend::disconnectRadio()
{
    if (m_busyProbe)
        m_busyProbe->cancel();
    retirePcmStreams();
    m_droopCalibrator.stop(false);
    m_droopCalibrator.setLandedRate(0);
    ++m_connectGeneration;   // orphan any in-flight finishDspSetup callback
    // Blocking, like ~AnanBackend() and closeEvent()'s other worker stops: the caller
    // may tear down GUI objects or this backend next (the window X button runs this
    // synchronously with the event loop pumping), so the I/O thread must have closed
    // the socket first. disconnected() still arrives via linkDown().
    if (m_client)
        QMetaObject::invokeMethod(m_client, "stop", Qt::BlockingQueuedConnection);
    m_tuneThrottleTimer->stop();
    m_tunePendingApply = false;
    // A receiver open when the session ends stays open, so the next connect
    // reopens it; a build still in flight finishes into nothing.
    for (Receiver& rx : m_receivers) {
        rx.tuneDirty = false;
        rx.openBuild = 0;
    }
    m_openingReceiver = -1;
    m_activeReceiver = rx1().index;
    m_speakerMix.reset();
    m_pendingBandwidthKsps = 0;
    m_heldPanCreate = {};
    // The next session's counters restart from zero in P2Client::start(), so a
    // cached snapshot from this one would make the first tick of the next read
    // as a huge backward jump in every total.
    m_linkCounters = {};
    m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = 0;
    // A status packet can arrive before the next session's linkUp, so the
    // rail's window is emptied here as well as there.
    resetSupplyRailWindow();
    // A genuine operator-initiated disconnect always fires disconnected(),
    // even one that lands mid-rate-change -- this is not the zoom case
    // m_rateChanging exists to hide.
    m_rateChanging = false;
    for (const Receiver& rx : m_receivers) {
        // m_audioMuted lives on AnanRxDsp and survives configure() (see that
        // function's own comment), so a disconnect landing mid-rate-change --
        // while audio was deliberately muted for the settle window -- would
        // otherwise leave the NEXT session starting muted with nothing left to
        // ever clear it.
        QMetaObject::invokeMethod(rx.dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, false));
        // The droop tables are per-RADIO and each DSP outlives any one
        // connection, so they go with the radio they were measured on. Clearing
        // here (as well as before connectRadio()'s seed) means a disconnected
        // session cannot leave a stale correction armed for whatever connects
        // next, by any path. The bypass flag is cleared too: a disconnect
        // mid-sweep stops the calibrator (the backend's disconnect handler) but
        // its finishSweep() cannot reach a backend that is already gone.
        QMetaObject::invokeMethod(rx.dsp, "clearDroopCorrectionTables",
                                  Qt::QueuedConnection);
        QMetaObject::invokeMethod(rx.dsp, "setDroopCorrectionBypassed",
                                  Qt::QueuedConnection, Q_ARG(bool, false));
    }
    // linkDown() (constructor-wired) sets m_connected = false and emits
    // disconnected() once P2Client::stop() actually runs.
}

void AnanBackend::pushModeFilterShift(const Receiver& rx)
{
    const double bfo = cwBfoHz(rx);
    QMetaObject::invokeMethod(rx.dsp, "setMode", Qt::QueuedConnection,
        Q_ARG(WdspChannel::Mode, modeFromString(rx.mode)));
    QMetaObject::invokeMethod(rx.dsp, "setFilter", Qt::QueuedConnection,
        Q_ARG(double, static_cast<double>(rx.filterLowHz) + bfo),
        Q_ARG(double, static_cast<double>(rx.filterHighHz) + bfo));
    // No NCO-vs-slice offset in this backend (see the class comment), so the
    // shift is the CW BFO, negated (HERMES §5: the shift names the RF frequency
    // the detector treats as zero, so pushing that zero DOWN a pitch lifts the
    // marker UP onto it), plus RIT, which moves that zero UP by the offset.
    QMetaObject::invokeMethod(rx.dsp, "setShift", Qt::QueuedConnection,
        Q_ARG(double, detectorShiftHz(rx.mode, m_cwPitchHz, rx.ritEnabled, rx.ritOffsetHz)));
}

void AnanBackend::setSliceFrequency(int sliceId, double hz)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    rx->freqHz = hz;
    rx->tuneDirty = true;
    // Digit readout / slice model: unthrottled. Cheap (no radio round trip,
    // no pan re-layout), and it is what makes a drag gesture feel like it is
    // tracking the mouse at all.
    emitSliceState(*rx);
    // Everything that touches the radio or the pan's display geometry goes
    // through the coalescing throttle instead of firing here directly -- see
    // applyTuneToRadioAndPan()'s comment for why: a click/drag-tune gesture
    // calls this once per mouse-move event (tens of times a second), and
    // Phase 1b has no NCO-vs-slice decoupling (the class comment is explicit:
    // "the shift is ALWAYS exactly -cwBfoHz... because the NCO IS the slice
    // frequency, always") -- so unthrottled, EVERY one of those events would
    // retune the actual DDC hardware and re-broadcast the pan's geometry.
    scheduleTuneApply();
    // The Tuning capture rides the same throttle: applyTuneToRadioAndPan().
}

void AnanBackend::setSliceMode(int sliceId, const QString& requested)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    // A mode modeFromString() does not map would demodulate as USB, and one the
    // records do not offer (WBFM, DRM) would leave every headless verb refused,
    // so both are refused and the re-publish puts SliceModel's optimistic mode
    // back. An alias is stored under its offered spelling, so the combos can show it.
    if (!isOfferedModeString(requested)) {
        qCWarning(lcAnan) << "ANAN: refusing mode" << requested
                          << "- this radio demodulates only" << publishedModeStrings();
        emitSliceState(*rx);
        return;
    }
    const QString mode = canonicalOfferedMode(requested);
    // Idempotence: only reset the passband to the mode's default when the
    // mode actually changes, so a repeated set does not clobber an
    // operator's manual filter edit.
    if (mode.compare(rx->mode, Qt::CaseInsensitive) != 0) {
        const auto [lo, hi] = defaultPassbandForMode(mode);
        rx->filterLowHz = lo;
        rx->filterHighHz = hi;
    }
    rx->mode = mode;
    pushModeFilterShift(*rx);   // HERMES §16.7: mode changes re-push the passband, every time
    emitSliceState(*rx);
    emit operatingStateChanged();   // Tuning capture; a mode change is discrete
}

void AnanBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    rx->filterLowHz = lowHz;
    rx->filterHighHz = highHz;
    const double bfo = cwBfoHz(*rx);
    QMetaObject::invokeMethod(rx->dsp, "setFilter", Qt::QueuedConnection,
        Q_ARG(double, static_cast<double>(lowHz) + bfo),
        Q_ARG(double, static_cast<double>(highHz) + bfo));
    emitSliceState(*rx);
}

void AnanBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    // thresholdDb is the AGC-T knob's 0..100, not dB: agcKnobDb() maps it.
    const int wdspMode = agcModeFromName(mode);
    const int threshold = std::clamp(thresholdDb, 0, 100);
    const bool changed = wdspMode != rx->agcMode || threshold != rx->agcThreshold;
    // Live state for connectRadio() and beginRateChange() -- see the member
    // declaration comment.
    rx->agcMode = wdspMode;
    rx->agcThreshold = threshold;
    QMetaObject::invokeMethod(rx->dsp, "setAgc", Qt::QueuedConnection,
        Q_ARG(int, wdspMode), Q_ARG(double, agcKnobDb(rx->agcThreshold)));
    emitSliceState(*rx);
    if (changed) {
        emit operatingStateChanged();   // Agc capture
    }
}

void AnanBackend::requestSliceAgc(int sliceId, const SliceAgcRequest& request)
{
    if (request.field != SliceAgcRequest::Field::OffLevel) {
        IRadioBackend::requestSliceAgc(sliceId, request);
        return;
    }
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    // Sent in every AGC mode: WDSP applies the fixed gain only with AGC off
    // (wcpAGC.c xwcpagc), so a level set first takes effect when AGC goes off.
    const int offLevel = std::clamp(request.offLevel, 0, 100);
    const bool changed = offLevel != rx->agcOffLevel;
    rx->agcOffLevel = offLevel;
    QMetaObject::invokeMethod(rx->dsp, "setAgcFixedGain", Qt::QueuedConnection,
        Q_ARG(double, agcKnobDb(rx->agcOffLevel)));
    emitSliceState(*rx);
    if (changed) {
        emit operatingStateChanged();   // Agc capture
    }
}

ReceiveDispatch AnanBackend::requestSliceDsp(int sliceId, const SliceDspRequest& request)
{
    // #5824 already owns the worker and NB readback. The typed desktop route
    // must consume that implementation, not replace it or invent other DSP.
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx || !request.valid()) {
        return ReceiveDispatch::Unsupported;
    }
    if (request.feature == SliceDspRequest::Feature::Nr) {
        // WDSP's ANR on the receiver's channel; level rides along.
        if (request.field != SliceDspRequest::Field::Enabled
            && request.field != SliceDspRequest::Field::Level) {
            return ReceiveDispatch::Unsupported;
        }
        const bool wasOn = rx->nrEnabled;
        const int wasLevel = rx->nrLevel;
        if (request.field == SliceDspRequest::Field::Enabled)
            rx->nrEnabled = request.enabled;
        else
            rx->nrLevel = std::clamp(request.level, 0, 100);
        QMetaObject::invokeMethod(rx->dsp, "setNoiseReduction", Qt::QueuedConnection,
            Q_ARG(bool, rx->nrEnabled), Q_ARG(int, rx->nrLevel));
        emitSliceState(*rx);
        if (rx->nrEnabled != wasOn || rx->nrLevel != wasLevel) {
            emit operatingStateChanged();   // Receivers capture
        }
        return ReceiveDispatch::Dispatched;
    }
    if (request.feature != SliceDspRequest::Feature::Nb) {
        return ReceiveDispatch::Unsupported;
    }
    // Level and Fill ride along so the one verb always gets the whole state.
    setSliceNoiseBlanker(sliceId, request.requestedBlanker(), request.level, request.fill);
    return ReceiveDispatch::Dispatched;
}

ReceiveDispatch AnanBackend::requestSliceControl(int sliceId, const SliceControlRequest& request)
{
    if (request.valid() && request.field == SliceControlRequest::Field::Rit
        && receiverForSlice(sliceId)) {
        const IncrementalTuning tuning = std::get<IncrementalTuning>(request.value);
        setSliceRit(sliceId, tuning.enabled, tuning.hz);
        return ReceiveDispatch::Dispatched;
    }
    // Choosing a receiver sends nothing to the radio, so the answer stays the
    // seam's LocalOnly, but the radio echoes no deselection either: the old
    // receiver is cleared here, or both slices read active and every publish
    // puts the flag back on the receiver this backend still thinks is active.
    // setActiveSlice() ignores a closed or unknown receiver.
    if (request.valid() && request.field == SliceControlRequest::Field::ActiveSlice
        && std::get<bool>(request.value)) {
        setActiveSlice(sliceId);
    }
    return IRadioBackend::requestSliceControl(sliceId, request);
}

ReceiveDispatch AnanBackend::requestRadioSpeakerAudio(bool on)
{
    // Same thread as sendSpeakerAudioToRadio() (the DSP's pcmReady is queued to this
    // object), so the flag and the resamplers change between blocks, never inside one.
    m_pendingParams.speakerAudioEnabled = on;
    if (m_speakerAudioEnabled != on) {
        m_speakerAudioEnabled = on;
        resetSpeakerResamplers();
        if (m_client) {
            QMetaObject::invokeMethod(m_client, "setSpeakerAudioEnabled", Qt::QueuedConnection,
                                      Q_ARG(bool, on));
        }
    }
    emit radioSpeakerAudioReported(m_speakerAudioEnabled);
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceRit(int sliceId, bool on, int hz)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    // One shift push and one echo, so no intermediate offset/enable pair is
    // ever heard or published.
    const int offset = std::clamp(hz, -kMaxRitOffsetHz, kMaxRitOffsetHz);
    const bool changed = offset != rx->ritOffsetHz || on != rx->ritEnabled;
    rx->ritOffsetHz = offset;
    rx->ritEnabled = on;
    pushModeFilterShift(*rx);
    emitSliceState(*rx);
    if (changed) {
        emit operatingStateChanged();   // Receivers capture
    }
}

void AnanBackend::setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                                       int level, AetherSDR::NoiseBlankerFill fill)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    rx->nbKind = kind;
    rx->nbLevel = std::clamp(level, 0, 100);
    rx->nbFill = fill;
    QMetaObject::invokeMethod(rx->dsp, "setNoiseBlanker", Qt::QueuedConnection,
        Q_ARG(WdspChannel::NoiseBlanker, AetherSDR::toWdsp(rx->nbKind)),
        Q_ARG(int, rx->nbLevel),
        Q_ARG(WdspChannel::NoiseBlankerFill, AetherSDR::toWdsp(rx->nbFill)));
    // Published now, like the other receive setters, so the blanker's
    // observation does not wait for an unrelated echo.
    emitSliceState(*rx);
}

ReceiveDispatch AnanBackend::requestSliceAudio(int sliceId, const SliceAudioRequest& request)
{
    if (!receiverForSlice(sliceId) || !request.valid()
        || request.origin != SliceAudioRequest::Origin::Operator) {
        return ReceiveDispatch::Unsupported;
    }
    switch (request.field) {
    case SliceAudioRequest::Field::Gain: setSliceAudioGain(sliceId, request.value); break;
    case SliceAudioRequest::Field::Mute: setSliceAudioMute(sliceId, request.value != 0); break;
    case SliceAudioRequest::Field::Pan: setSliceAudioPan(sliceId, request.value); break;
    }
    return ReceiveDispatch::Dispatched;
}

void AnanBackend::setSliceAudioMute(int sliceId, bool mute)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx || rx->audioMuted == mute) {
        return;
    }
    rx->audioMuted = mute;
    // Published, not just stored. The gate that decides whether the control is
    // offered at all requires an OBSERVATION as well as the capability, so a
    // mute that is applied but never echoed leaves the control refused with
    // "capability.unavailable" -- working audio behind a dead control.
    emitSliceState(*rx);
}

void AnanBackend::setSliceAudioGain(int sliceId, int gainPercent)
{
    Receiver* const rx = receiverForSlice(sliceId);
    const int bounded = std::clamp(gainPercent, 0, 100);
    if (!rx || rx->audioGainPercent == bounded) {
        return;
    }
    rx->audioGainPercent = bounded;
    emitSliceState(*rx);
}

void AnanBackend::setSliceAudioPan(int sliceId, int panPercent)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx)
        return;
    // NOT published: SliceDelta carries no balance field, so there is no
    // observation to echo and nothing downstream reads one. Applied all the
    // same -- the audio stage is the same stage either way.
    rx->audioPanPercent = std::clamp(panPercent, 0, 100);
}

void AnanBackend::setLineoutGain(int percent)
{
    // The percent runs through the SAME dB law as the per-slice fader
    // (sliceAudioAmplitude, at the send), so 50 here is -20 dB and not half
    // amplitude. Worth knowing when comparing families: a Flex's own lineout
    // route sends `mixer lineout gain 50` and whatever that means at the radio is
    // its own scale, so the same slider position is not promised to be the same
    // loudness across radios. One law for both of OUR level controls is the
    // trade taken -- the alternative is a fader whose feel changes depending on
    // which of the three levels the operator happens to be moving.
    const int wanted = std::clamp(percent, 0, 100);
    if (wanted == m_lineoutGainPercent) {
        return;
    }
    m_lineoutGainPercent = wanted;
    emit operatingStateChanged();   // ReceiveOutputLevel capture
}

void AnanBackend::setLineoutMute(bool mute)
{
    m_lineoutMuted = mute;
}

void AnanBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent intent)
{
    Q_UNUSED(intent);  // ignored, matching Hl2Backend::setPanCenter
    // The pan is its receiver's DDC, so its center IS that receiver's tune.
    if (const Receiver* rx = receiverForPan(panId))
        setSliceFrequency(rx->index, hz);
}

int AnanBackend::nearestDdc0RateKsps(int requestedKsps) noexcept
{
    // The DDC runs at exactly one of these rates (no continuous zoom). Nearest by RATIO
    // (log domain), like Hl2Backend::nearestIqSampleRateHz (HERMES.md §15.1): the
    // rates are octave-spaced and zoom is multiplicative, so 140 ksps belongs to 192
    // (geometric mean 135.8), not 96. The equidistant point (e.g. 96*sqrt(2)) is
    // never an integer request, so no tie-break is needed.
    if (requestedKsps <= 0)
        return kDdc0RatesKsps.front();
    int best = kDdc0RatesKsps.front();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const int r : kDdc0RatesKsps) {
        const double distance = std::abs(std::log(static_cast<double>(requestedKsps) / r));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = r;
        }
    }
    return best;
}

void AnanBackend::setPanBandwidth(const QString& panId, double hz)
{
    Receiver* const rx = receiverForPan(panId);
    if (!rx || hz <= 0.0 || !m_connected)
        return;

    const int requestedKsps = static_cast<int>(hz / 1000.0 + 0.5);
    const int snappedKsps = nearestDdc0RateKsps(requestedKsps);

    if (snappedKsps == rx->rateKsps) {
        // Already at the closest rate: republish so an optimistic GUI span snaps back
        // (as PanadapterModel::republishCenterBandwidth(), #4470), and clear any pending
        // request so a stale older zoom cannot fire after this one.
        m_pendingBandwidthKsps = 0;
        emitPanState(*rx);
        return;
    }

    if (!m_rateChanging && m_openingReceiver < 0) {
        beginRateChange(rx->index, snappedKsps);
    } else {
        // A reconfigure or a receiver's open is already running -- remember
        // the latest request rather than starting a second one on top of it.
        // See m_rateChanging's own comment for why this waits for that cycle
        // to actually finish rather than a fixed cooldown.
        m_pendingBandwidthKsps = snappedKsps;
        m_pendingBandwidthReceiver = rx->index;
    }
}

void AnanBackend::retryPendingRateChange()
{
    // A receiver's open in flight retries this itself when it finishes.
    if (m_pendingBandwidthKsps == 0 || m_openingReceiver >= 0)
        return;
    const int ksps = m_pendingBandwidthKsps;
    m_pendingBandwidthKsps = 0;
    if (m_receivers[static_cast<std::size_t>(m_pendingBandwidthReceiver)].open)
        beginRateChange(m_pendingBandwidthReceiver, ksps);
}

void AnanBackend::beginRateChange(int receiverIndex, int newRateKsps)
{
    Receiver& rx = m_receivers[static_cast<std::size_t>(receiverIndex)];
    // A live rate change: build the new WdspChannel on m_dspBuildThread FIRST (up to
    // ~a minute cold) while the old channel and session keep running undisturbed;
    // only then does finishRateChange() mute, swap and send the new rate to the
    // RUNNING session (P2Client::setDdcRateLive()). Build-before-disturb is what
    // keeps zoom from freezing. m_preRateChangeKsps records the running rate so the
    // failure path can roll back: emitPanState() reports from pendingDspConfig.
    m_preRateChangeKsps = rx.rateKsps;
    rx.rateKsps = newRateKsps;
    rx.pendingDspConfig.inputSampleRateHz = newRateKsps * 1000;
    // Refresh from CURRENT live operator state, not connectRadio()'s
    // connect-time snapshot: pendingDspConfig otherwise only ever had its
    // rate touched here, so a rate change silently reverted mode/filter/AGC
    // to whatever they were at connect. m_shiftHz is not part of Config --
    // AnanRxDsp::installChannel() re-applies it separately, after the swap.
    applyLiveRxState(rx, rx.pendingDspConfig);

    m_rateChanging = true;
    ++m_connectGeneration;   // orphans any in-flight prior connect/reconfigure/rebuild
    const quint64 generation = m_connectGeneration;
    const AnanRxDsp::Config cfg = rx.pendingDspConfig;
    AnanRxDsp* const dsp = rx.dsp;

    // Mark "rebuild in flight" on the DSP's own thread FIRST, so any
    // setMode/setFilter/setAgc/setShift call that lands while the build is
    // running defers pushing to the (still-live, still-playing) old channel
    // instead of blocking on WDSP's process-wide setup mutex -- see
    // AnanRxDsp::beginRebuild()'s own comment. THEN hand the slow build to
    // the dedicated build thread.
    QMetaObject::invokeMethod(dsp, [this, dsp, receiverIndex, generation, cfg]() {
        dsp->beginRebuild();

        QMetaObject::invokeMethod(m_dspBuildContext,
                                  [this, dsp, receiverIndex, generation, cfg]() {
            AnanRxDsp::RebuildResult result = AnanRxDsp::buildChannel(cfg);
            const bool ok = result.channel != nullptr;
            const QString errStr = ok ? QString()
                : QStringLiteral("ANAN: DSP rebuild failed: %1")
                      .arg(QString::fromStdString(result.error));

            QMetaObject::invokeMethod(dsp, [dsp, r = std::move(result)]() mutable {
                dsp->installRebuiltChannel(std::move(r));
            }, Qt::QueuedConnection);

            QMetaObject::invokeMethod(this, [this, receiverIndex, generation, ok, errStr]() {
                finishRateChange(receiverIndex, generation, ok, errStr);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void AnanBackend::finishRateChange(int receiverIndex, quint64 generation, bool ok,
                                   const QString& error)
{
    if (generation != m_connectGeneration)
        return;   // superseded by a newer rate change/connect/disconnect

    Receiver& rx = m_receivers[static_cast<std::size_t>(receiverIndex)];
    if (!ok) {
        // Roll the reported rate back to the one still running BEFORE
        // emitting pan state. AnanDroopCalibrator infers "the rate landed"
        // from pan bandwidth reaching its target; told the failed rate had
        // landed, it would measure the OLD rate's spectrum into the NEW
        // rate's correction table and persist it -- one rate's droop curve
        // applied to another rate's bins, which is exactly the cross-rate
        // corruption anan_rxdsp_handedness_test's Group 6 exists to prevent.
        if (m_preRateChangeKsps > 0) {
            rx.rateKsps = m_preRateChangeKsps;
            rx.pendingDspConfig.inputSampleRateHz = m_preRateChangeKsps * 1000;
        }
        emit connectionError(error);
        // Safe to clear m_rateChanging immediately here, unlike
        // startP2ClientSession()'s own failure branch below: the OLD
        // P2Client session was never touched on this path -- beginRateChange()
        // no longer stops it before building (see that function's own
        // comment) -- so there is no live teardown in flight whose
        // linkDown()/linkUp() could leak through unsuppressed. The old
        // channel and old session simply keep running at the OLD rate.
        m_rateChanging = false;
        emitPanState(rx);
        resumeAfterRebuild();
        return;
    }

    // The new channel is already installed (queued just before this call). Mute
    // here, not on a later event: frames reach AnanRxDsp::processIqBlock() by
    // DirectConnection on the I/O thread, so anything triggered after the rate
    // lands would be at least one block late.
    QMetaObject::invokeMethod(rx.dsp, "setAudioMuted", Qt::QueuedConnection, Q_ARG(bool, true));

    // LIVE rate change on the running session: p2app applies DDC-Specific rate
    // changes by a direct FPGA register write with no teardown (see
    // P2Client::setDdcRateLive()). Until it lands the new channel sees old-rate
    // samples; the mute and settle window cover that. Sent THREE times
    // (kRateChangeResendMs): it is fire-and-forget UDP, the keepalive never resends
    // DDC-Specific, and a lost packet would leave the radio at the old rate with no
    // error while everything here records the new one. Duplicates are no-ops
    // (WriteP2DDCRateRegister() fires on change; HandlerCheckDDCSettings() is
    // empty) (#5547).
    const int rateKsps = rx.pendingDspConfig.inputSampleRateHz / 1000;
    auto sendRate = [this, ddc = rx.route.ddc, rateKsps, generation]() {
        if (generation != m_connectGeneration)
            return;
        QMetaObject::invokeMethod(m_client, "setDdcRateLive", Qt::QueuedConnection,
                                  Q_ARG(int, ddc), Q_ARG(int, rateKsps));
    };
    sendRate();
    for (const int delayMs : kRateChangeResendMs)
        QTimer::singleShot(delayMs, this, sendRate);

    QTimer::singleShot(kRateChangeLiveSettleMs, this, [this, receiverIndex, generation]() {
        // Same generation guard the restart path used: a newer rate
        // change/connect/disconnect during the settle window supersedes this.
        if (generation != m_connectGeneration)
            return;
        const Receiver& changed = m_receivers[static_cast<std::size_t>(receiverIndex)];
        QMetaObject::invokeMethod(changed.dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, false));
        m_rateChanging = false;
        emitPanState(changed);
        // An Add Panadapter held behind this, then a zoom the operator kept
        // turning while it ran: apply them now, not a step behind.
        resumeAfterRebuild();
    });
}

void AnanBackend::setPanFrameRate(const QString& panId, int fps)
{
    // Default no-op inherited from IRadioBackend was written for Flex,
    // whose own display engine paces its frames -- silently wrong here,
    // since it left AnanRxDsp's spectrum production uncapped (measured on
    // the bench: the waterfall scrolled far faster than the operator's own
    // FPS setting, because nothing was ever telling it what that setting
    // was). Mirrors Hl2Backend::setPanFrameRate()'s shape exactly.
    if (const Receiver* rx = receiverForPan(panId))
        QMetaObject::invokeMethod(rx->dsp, "setSpectrumRateFps", Qt::QueuedConnection,
                                  Q_ARG(int, fps));
}

void AnanBackend::setPanRfGain(const QString& panId, int gainDb)
{
    const Receiver* const rx = receiverForPan(panId);
    if (!rx)
        return;
    // The G2 has no gain stage the host drives, only the step attenuator in
    // front of each ADC (Protocol v4.4 pp.34,36, HP bytes 1442/1443).
    // deskHPSDR offers it as a 0-31 dB attenuation control; here it rides the RF Gain
    // slider, negated, so "more gain" still means "slider right". Clamped
    // rather than refused, per the seam's contract. It is the pan's ADC's
    // attenuator, so it moves for every receiver on that ADC.
    const int adc = rx->adc;
    const int attenuation = -std::clamp(gainDb, -kMaxStepAttenuationDb, 0);
    // Kept in the params: a session restart resends everything from
    // m_pendingParams, and attenuationDbForAdc() reads it back.
    if (adc == 1) {
        m_pendingParams.adc1AttenuationDb = attenuation;
    } else {
        m_pendingParams.adc0AttenuationDb = attenuation;
    }
    if (m_client) {
        QMetaObject::invokeMethod(m_client, "setStepAttenuationDb", Qt::QueuedConnection,
                                  Q_ARG(int, adc), Q_ARG(int, attenuation));
    }
    // Only open pans: a pan id the model no longer holds would be read as its
    // active pan's.
    for (const Receiver& other : m_receivers) {
        if (other.open && other.adc == adc)
            emit panRfGainChanged(other.panId, -attenuation);
    }
    emit operatingStateChanged();
}

void AnanBackend::setPanAverage(const QString& panId, int average)
{
    // The operator's FFT AVG slider, which otherwise reaches nothing on this
    // radio: the panadapter is averaged inside WDSP's analyzer, as a TIME --
    // see kMsPerAverageStep.
    if (const Receiver* rx = receiverForPan(panId))
        QMetaObject::invokeMethod(rx->dsp, "setSpectrumAverageMs", Qt::QueuedConnection,
                                  Q_ARG(int, std::max(0, average) * kMsPerAverageStep));
}

void AnanBackend::setPanWeightedAverage(const QString& panId, bool on)
{
    // deskHPSDR offers the analyzer's averaging MODE alongside its time. The
    // toggle picks between the two recursive modes: on = log-recursive
    // (deskHPSDR's default, the smoother look), off = linear-recursive (an
    // average of power, which reads noise and weak signals at their true
    // level).
    if (const Receiver* rx = receiverForPan(panId))
        QMetaObject::invokeMethod(rx->dsp, "setSpectrumLogAverage", Qt::QueuedConnection,
                                  Q_ARG(bool, on));
}

void AnanBackend::setPanPixelWidth(const QString& panId, int pixels)
{
    // One analyzer point per screen pixel, as deskHPSDR sizes its analyzer
    // from the panel width. A fixed 1024 points stretched across a wider
    // panel is what made the waterfall look soft.
    Receiver* const rx = receiverForPan(panId);
    if (!rx || pixels < 2)
        return;
    rx->panPoints = std::min(pixels, AnanPanAnalyzer::kMaxPoints);
    rx->pendingDspConfig.panPoints = rx->panPoints;
    QMetaObject::invokeMethod(rx->dsp, "setPanPoints", Qt::QueuedConnection,
                              Q_ARG(int, rx->panPoints));
}

void AnanBackend::setCwPitch(int hz)
{
    // TransmitModel's range, clamped again at this seam: the declared CW filter
    // limits assume the BFO never exceeds 6000 Hz, and 0 would put CW on DC.
    m_cwPitchHz = std::clamp(hz, 100, 6000);
    // Both the shift AND the filter depend on the pitch -- HERMES §5:
    // "entering or leaving CW has to re-push the shift, not just the
    // filter." Re-derive both from current state rather than adjusting in
    // place, so this is correct whether or not the mode is currently CW.
    // One pitch, so every receiver.
    for (const Receiver& rx : m_receivers) {
        pushModeFilterShift(rx);
        emitSliceState(rx);
    }
}

void AnanBackend::setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion)
{
    Q_UNUSED(operation);
    Q_UNUSED(completion);
    // canTransmit is false and P2Client has no PTT capability -- there is
    // nothing this method could do even if the engine's TX guard (which
    // gates every call site above this seam) let a key-on request through.
    // A key-on request reaching here at all means that guard was bypassed,
    // which is itself the bug to chase -- not something to silently absorb.
    if (key)
        qWarning("AnanBackend::setKeying(true) called on a receive-only backend "
                 "(canTransmit=false) -- the engine TX guard should have refused this");
}

QString AnanBackend::persistDroopTables(const QMap<int, anan::DroopCorrectionTable>& tables)
{
    if (tables.isEmpty())
        return QStringLiteral("the request carried no valid correction table");
    // Never write an empty radio_id row (AGENTS.md): RadioSettingsScope falls
    // back exact-radio -> family-wide on read, so a row written with no
    // identity would be silently adopted by every ANAN that has none of its
    // own -- the same guard Hl2Backend::applyFreqCalPpb() uses. This is the
    // one thing saveTables() cannot judge for itself: a family-wide scope is
    // perfectly VALID, just not what a per-radio calibration wants.
    if (m_radioSerial.isEmpty()) {
        return QStringLiteral("no radio identity yet -- the correction is live "
                              "for this session only");
    }
    return AnanDroopCalibrator::saveTables(
        RadioSettingsScope(QStringLiteral("anan"), m_radioSerial), tables);
}

QVariantMap AnanBackend::droopStatus() const
{
    // EVERY DDC0 rate, tagged with where its correction came from -- not just
    // the swept ones. Reporting only measuredTables() told the operator
    // "nothing measured" while the defaults connectRadio() seeds were live on
    // the display, so a panadapter that looked wrong had no surface anywhere
    // connecting it to a correction that was in fact being applied. The tag
    // is what keeps "derived from the gateware" and "measured on this radio"
    // distinguishable rather than collapsing them into one list.
    QVariantList corrections;
    const auto& measured = m_droopCalibrator.measuredTables();
    for (const int rateKsps : defaultDroopRatesKsps()) {
        const auto it = measured.constFind(rateKsps);
        const bool isMeasured = it != measured.constEnd();
        // A default is only live once connectRadio() has actually seeded it.
        // Before that the rate genuinely carries no correction and must not
        // claim one -- measured results, by contrast, outlive the connection
        // because the calibrator holds them for the session.
        const DroopCorrectionTable* table =
            isMeasured ? &it.value()
                       : (m_connected ? defaultDroopTableForRate(rateKsps) : nullptr);
        if (!table)
            continue;
        const auto [lo, hi] = std::minmax_element(table->begin(), table->end());
        corrections.append(QVariantMap{
            {QStringLiteral("rateKsps"), rateKsps},
            {QStringLiteral("minDb"), *lo},
            {QStringLiteral("maxDb"), *hi},
            {QStringLiteral("source"), isMeasured ? QStringLiteral("measured")
                                                  : QStringLiteral("default")}});
    }
    return {{QStringLiteral("running"), m_droopCalibrator.isRunning()},
            {QStringLiteral("rateIndex"), m_droopCalibrator.rateIndex()},
            {QStringLiteral("totalRates"), m_droopCalibrator.totalRates()},
            {QStringLiteral("hasResult"), m_droopCalibrator.hasResult()},
            {QStringLiteral("percent"), m_droopPercent},
            {QStringLiteral("message"), m_droopMessage},
            {QStringLiteral("corrections"), corrections}};
}

void AnanBackend::publishDroopStatus()
{
    emit extensionStatus(QStringLiteral("anan"), QStringLiteral("droop"), droopStatus());
}

QString AnanBackend::applyDroopTables(const QMap<int, DroopCorrectionTable>& tables)
{
    // Preserve the existing apply/persist behavior. The sweep and its data
    // stay below the seam; clients can apply a result, never inject tables.
    // Measured on RX1's DDC, applied to every receiver's: the same gateware
    // block, so the same roll-off (see connectRadio()'s seed).
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        for (const Receiver& rx : m_receivers) {
            QMetaObject::invokeMethod(rx.dsp, "setDroopCorrectionTable", Qt::QueuedConnection,
                Q_ARG(int, it.key()), Q_ARG(std::vector<float>,
                    std::vector<float>(it.value().begin(), it.value().end())));
        }
    }
    return persistDroopTables(tables);
}

void AnanBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    if (ns == QLatin1String("anan") && verb == QLatin1String("frontend.set")) {
        // The live front-end options. Partial: only the named fields move,
        // so one control cannot overwrite another. A change reaches the radio
        // on the next High Priority packet, within 100 ms.
        //
        // This persists nothing. The ANAN Front End page writes AnanSettings
        // and then invokes this; a caller that only invokes it gets a change
        // that reverts at the next connect, which reads AnanSettings.
        const QVariantMap fields = arg.toMap();
        if (fields.contains(QStringLiteral("bandFiltersEnabled"))) {
            m_pendingParams.bandFiltersEnabled =
                fields.value(QStringLiteral("bandFiltersEnabled")).toBool();
            if (m_client) {
                QMetaObject::invokeMethod(m_client, "setBandFiltersEnabled",
                                          Qt::QueuedConnection,
                                          Q_ARG(bool, m_pendingParams.bandFiltersEnabled));
            }
        }
        if (fields.contains(QStringLiteral("speakerAmpMuted"))) {
            m_pendingParams.speakerAmpMuted =
                fields.value(QStringLiteral("speakerAmpMuted")).toBool();
            if (m_client) {
                QMetaObject::invokeMethod(m_client, "setSpeakerAmpMuted",
                                          Qt::QueuedConnection,
                                          Q_ARG(bool, m_pendingParams.speakerAmpMuted));
            }
        }
        if (requestId != 0) {
            emit extensionResult(requestId, QVariantMap{{QStringLiteral("ok"), true}});
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb == QLatin1String("nb.get")) {
        if (requestId != 0) {
            QVariantList receivers;
            for (const Receiver& rx : m_receivers) {
                if (!rx.open)
                    continue;
                const AnanRxDsp::NoiseBlankerState applied = rx.dsp->noiseBlankerState();
                receivers.append(QVariantMap{
                    // The DDC this receiver runs on (kReceiverRoutes).
                    {QStringLiteral("ddc"), rx.route.ddc},
                    {QStringLiteral("panId"), rx.panId},
                    // `on` stays, and stays first: it is what every existing script
                    // reads. `kind` is the same fact with the second blanker in it.
                    {QStringLiteral("on"), applied.on()},
                    {QStringLiteral("kind"), static_cast<int>(applied.kind)},
                    {QStringLiteral("level"), applied.level},
                    {QStringLiteral("fill"), static_cast<int>(applied.fill)},
                    {QStringLiteral("requestedOn"),
                     rx.nbKind != AetherSDR::NoiseBlankerKind::Off},
                    {QStringLiteral("requestedKind"), static_cast<int>(rx.nbKind)},
                    {QStringLiteral("requestedLevel"), rx.nbLevel},
                    {QStringLiteral("requestedFill"), static_cast<int>(rx.nbFill)},
                    {QStringLiteral("hasChain"), applied.hasChain},
                    {QStringLiteral("threshold"),
                     WdspChannel::noiseBlankerThresholdForLevel(applied.level)},
                });
            }
            emit extensionResult(requestId, QVariantMap{
                {QStringLiteral("receivers"), receivers},
            });
        }
        return;
    }
    if (ns == QLatin1String("anan") && verb.startsWith(QLatin1String("droop."))) {
        if (!capabilities().hostDroopCalibration || !m_connected || m_radioSerial.isEmpty()) {
            emit extensionError(requestId, QStringLiteral("connect an ANAN before calibrating"));
            return;
        }
        const QString action = verb.mid(6);
        if (action == QLatin1String("status")) {
            emit extensionResult(requestId, droopStatus());
            return;
        }
        if (action != QLatin1String("start") && action != QLatin1String("stop")
            && action != QLatin1String("apply") && action != QLatin1String("discard")) {
            emit extensionError(requestId, QStringLiteral("unknown droop calibration action"));
            return;
        }
        if (m_droopCalibrator.isRunning() && action != QLatin1String("stop")) {
            emit extensionError(requestId, QStringLiteral("a sweep is already running -- stop it first"));
            return;
        }
        QString failure;
        const QMetaObject::Connection errorConnection = connect(
            &m_droopCalibrator, &AnanDroopCalibrator::error, this,
            [&failure](const QString& reason) { failure = reason; });
        m_droopMessage.clear();
        if (action == QLatin1String("start")) {
            m_droopCalibrator.start();
        } else if (action == QLatin1String("stop")) {
            m_droopCalibrator.stop();
        } else if (action == QLatin1String("apply")) {
            m_droopCalibrator.applyResult();
        } else {
            m_droopCalibrator.clear();
            m_droopPercent = 0;
            m_droopMessage = QStringLiteral("Discarded — no sweep result staged.");
        }
        QObject::disconnect(errorConnection);
        publishDroopStatus();
        if (!failure.isEmpty()) {
            emit extensionError(requestId, failure);
        } else {
            emit extensionResult(requestId, droopStatus());
        }
        return;
    }
    if (requestId != 0) {
        emit extensionError(requestId, QStringLiteral("ANAN: unknown extension verb '%1.%2'")
                                           .arg(ns, verb));
    }
}

QString AnanBackend::levelMeterId(int receiver)
{
    return receiver == 0 ? QStringLiteral("SLC:LEVEL")
                         : QStringLiteral("SLC%1:LEVEL").arg(receiver);
}

void AnanBackend::defineLevelMeter(const Receiver& rx)
{
    // Index is ours to choose -- nothing on a P2 radio assigns meter ids. Same
    // name, unit and range as Hl2Backend::defineMeters()'s receive meter, the
    // "SLC"/"LEVEL" MeterModel binds to a slice's S-meter by its source index.
    MeterDef d;
    d.index = levelMeterIndex(rx.index);
    d.source = QStringLiteral("SLC");
    d.sourceIndex = rx.index;
    d.name = QStringLiteral("LEVEL");
    d.unit = QStringLiteral("dBm");
    d.low = -140.0;
    d.high = 0.0;
    d.description = rx.index == 0
        ? QStringLiteral("Receive signal level")
        : QStringLiteral("Receive signal level, RX%1").arg(rx.index + 1);
    emit meterDefined(d);
}

void AnanBackend::defineMeters()
{
    // Receive-only: no TX meters until this backend can transmit.
    defineLevelMeter(rx1());

    // The supply rail. "+13.8A" is the name MeterModel binds to the status bar
    // and Meter applet voltage readouts, whatever the radio; unit and range as
    // IcomMeters declares the same meter.
    MeterDef rail;
    rail.index = 2;
    rail.source = QStringLiteral("RAD");
    rail.name = QStringLiteral("+13.8A");
    rail.unit = QStringLiteral("Volts");
    rail.low = 0.0;
    rail.high = 16.0;
    rail.description = QStringLiteral("DC supply voltage");
    emit meterDefined(rail);

    // A receiver that came back open at connect.
    for (std::size_t i = 1; i < m_receivers.size(); ++i) {
        if (m_receivers[i].open)
            defineLevelMeter(m_receivers[i]);
    }
}

IRadioBackend::LinkStats AnanBackend::linkStats() const
{
    LinkStats s;
    // Nothing measured yet: the consumer keeps whatever source it already had
    // rather than being handed a snapshot of zeros.
    if (!m_connected || !m_linkCountersSeen)
        return s;
    s.reported = true;
    s.alive = m_linkCounters.rxBytes > m_lastSnapshotRxBytes;
    s.rxBytes = static_cast<qint64>(m_linkCounters.rxBytes);
    s.txBytes = static_cast<qint64>(m_linkCounters.txBytes);
    s.rxPackets = m_linkCounters.rxPackets;
    s.rxPacketsLost = m_linkCounters.drops;
    s.localEndpoint = m_linkCounters.localEndpoint;
    // LEFT AT -1, WHICH THE STRUCT DEFINES AS "NOT MEASURED" AND THE MODEL
    // HONOURS BY IGNORING. This transport is stream-only: the radio is told
    // where to send and then sends, with no request/response exchange to time,
    // so there is no round trip here to measure. Reporting 0 instead would
    // render as a perfect "< 1 ms" link -- a measurement never taken.
    // rttMs, jitterMs, gapMs and gapMaxMs all keep their -1 defaults.
    return s;
}

void AnanBackend::onLinkCounters(const P2Client::LinkCounters& counters)
{
    // A session restart shows as a BACKWARD jump, since P2Client::start() zeroes
    // its counters; the cache is then a dead session's and is discarded here,
    // whichever path restarted it. The first snapshot of a session compares
    // against ZERO: against itself, it would read dead on the first tick.
    if (counters.rxBytes < m_linkCounters.rxBytes)
        m_linkCountersSeen = false;
    m_lastSnapshotRxBytes = m_linkCountersSeen ? m_linkCounters.rxBytes : 0;
    m_linkCounters = counters;
    m_linkCountersSeen = true;
    emit linkStatsUpdated(linkStats());
}

void AnanBackend::onDspMeter(int receiver, float dbfs)
{
    Receiver& rx = m_receivers[static_cast<std::size_t>(receiver)];
    // A closed receiver's meter is withdrawn; a late reading must not land on it.
    if (!rx.open)
        return;
    const double dbm = static_cast<double>(dbfs) + kSMeterDbmOffset
                       + attenuationDbForAdc(rx.adc);
    // Smooth EVERY reading, publish only on the tick -- the same smoother,
    // and the same reasons, as Hl2Backend's receive meter (SMeterSmoother).
    // AnanRxDsp reads the tap at one DSP-rate block's cadence whatever the
    // DDC rate (WdspSMeter::emitEveryBlocks), so this sees ~47 readings a
    // second at 48 and at 1536 ksps alike.
    if (const auto out = rx.sMeter.feed(dbm))
        emit meterUpdate(levelMeterId(rx.index), *out);
}

void AnanBackend::onSupplyRailCounts(int counts)
{
    if (!m_supplyRailClock.isValid())
        m_supplyRailClock.start();
    onSupplyRailCountsAt(counts, m_supplyRailClock.elapsed());
}

void AnanBackend::onSupplyRailCountsAt(int counts, qint64 nowMs)
{
    // A 12-bit ADC reads 0..4095. Anything else means the field is not this
    // input, and no reading is better than a plausible wrong voltage.
    if (counts < 0 || counts > 4095)
        return;
    m_supplyRailCountSum += counts;
    ++m_supplyRailSamples;
    // The first window opens on its first count; each later one opens where the
    // last closed, so publications stay a fixed window apart rather than
    // drifting by a packet interval each time.
    if (m_supplyRailWindowStartMs < 0)
        m_supplyRailWindowStartMs = nowMs;
    if (nowMs - m_supplyRailWindowStartMs < kSupplyRailWindowMs)
        return;
    const double meanCounts =
        static_cast<double>(m_supplyRailCountSum) / m_supplyRailSamples;
    m_supplyRailCountSum = 0;
    m_supplyRailSamples = 0;
    m_supplyRailWindowStartMs = nowMs;
    emit meterUpdate(QStringLiteral("RAD:+13.8A"), meanCounts * kSupplyRailVoltsPerCount);
}

void AnanBackend::resetSupplyRailWindow()
{
    m_supplyRailWindowStartMs = -1;
    m_supplyRailCountSum = 0;
    m_supplyRailSamples = 0;
}

void AnanBackend::emitSliceState()
{
    for (const Receiver& rx : m_receivers)
        emitSliceState(rx);
}

void AnanBackend::emitSliceState(const Receiver& rx)
{
    // A closed receiver has no slice; publishing one would bring it back.
    if (!rx.open)
        return;
    SliceDelta d;
    d.frequency = rx.freqHz / 1.0e6;   // SliceDelta::frequency is MHz
    d.mode = rx.mode;
    d.modeList = publishedModeStrings();
    d.filterLow = rx.filterLowHz;
    d.filterHigh = rx.filterHighHz;
    // Exactly one slice is active: setActiveSlice() clears the other itself.
    d.active = rx.index == m_activeReceiver;
    // A different radio replaces the slice model but retains this backend.
    // Publish the retained NB request so that fresh model agrees with the DSP.
    d.nb = rx.nbKind != AetherSDR::NoiseBlankerKind::Off;
    d.nbKind = rx.nbKind;
    d.nbLevel = rx.nbLevel;
    d.nbFill = rx.nbFill;
    d.nr = rx.nrEnabled;
    d.nrLevel = rx.nrLevel;
    d.ritOn = rx.ritEnabled;
    d.ritFreq = rx.ritOffsetHz;
    // The AGC as the DSP runs it, so the slice's knob starts from this
    // backend's values, not SliceModel's defaults.
    d.agcMode = agcModeName(rx.agcMode);
    d.agcThreshold = rx.agcThreshold;
    d.agcOffLevel = rx.agcOffLevel;
    // The audio stage as APPLIED. Both are required, not cosmetic: the receive
    // control gate refuses an operation whose observation is absent, so without
    // these two the mute and the fader are offered and then rejected.
    //
    // Safe to echo unconditionally -- SliceModel::applyChanges() assigns these
    // into its observation without re-emitting a command, so a published value
    // cannot loop back as a fresh intent (Principle II).
    d.audioGain = rx.audioGainPercent;
    d.audioMute = rx.audioMuted;
    // Without this, RadioModel::sliceChanged's handler never assigns the
    // slice a panId (SliceDelta::panId is std::optional and SliceModel::
    // applyChanges() only touches it when set) -- the slice materialised by
    // "Gap B" stays permanently unassociated with the pan emitPanState()
    // creates, and every click/drag-tune on the panadapter silently no-ops
    // because MainWindow_Wiring.cpp's tune-target resolver matches on
    // panId equality. Hl2Backend::emitSliceState() sets this every time for
    // the same reason.
    d.panId = rx.panId;
    emit sliceChanged(rx.index, d);
}

void AnanBackend::emitPanState()
{
    for (const Receiver& rx : m_receivers)
        emitPanState(rx);
}

void AnanBackend::emitPanState(const Receiver& rx)
{
    if (!rx.open)
        return;
    const double sampleRateHz = static_cast<double>(rx.pendingDspConfig.inputSampleRateHz > 0
        ? rx.pendingDspConfig.inputSampleRateHz : 48000);
    // Reports the TRUE rate, not a cropped fraction: the pan widget's zoom math uses
    // this as its baseline, and a reduced value stops zoom-out from ever crossing to
    // the next rate. The decimation roll-off stays visible (and is corrected by the
    // droop tables).
    if (&rx == &rx1()) {
        // The droop sweep zooms RX1's pan, so RX1's rate is the one it waits on.
        m_droopCalibrator.setLandedRate(static_cast<int>(sampleRateHz / 1000.0));
    }
    emit panCenterBandwidthChanged(rx.panId, rx.freqHz / 1.0e6, sampleRateHz / 1.0e6);
}

void AnanBackend::emitPanLimits(const Receiver& rx)
{
    if (!rx.open)
        return;
    // Bounds are capabilities().sampleRatesHz's endpoints.
    emit panBandwidthLimitsChanged(rx.panId,
                                  kDdc0RatesKsps.front() * 1000.0 / 1.0e6,
                                  kDdc0RatesKsps.back() * 1000.0 / 1.0e6);
    // The RF Gain slider IS the step attenuator of the pan's ADC: -31..0 dB
    // in 1 dB steps.
    emit panRfGainInfoChanged(rx.panId, -kMaxStepAttenuationDb, 0, 1);
    emit panRfGainChanged(rx.panId, -attenuationDbForAdc(rx.adc));
}

bool AnanBackend::createPanadapter()
{
    // The next closed route. While a rate change or another open rebuilds a
    // chain this refuses; requestPanLifecycle() holds the request instead.
    const auto closed = std::find_if(m_receivers.begin(), m_receivers.end(),
                                     [](const Receiver& rx) { return !rx.open; });
    if (closed == m_receivers.end() || !m_connected || rebuildingChain()) {
        return false;
    }
    openReceiver(*closed);
    return true;
}

ReceiveDispatch AnanBackend::requestPanLifecycle(const PanLifecycleRequest& request,
                                                 const PanLifecycleCompletion& completion,
                                                 const RadioControlFence& current)
{
    const bool closedRoute = std::any_of(m_receivers.begin(), m_receivers.end(),
                                         [](const Receiver& rx) { return !rx.open; });
    // Held only with a fence to check later, so a superseded request cannot
    // open a receiver; without one, or with no route left, the dispatch below
    // refuses as before.
    if (request.action == PanLifecycleAction::Create && rebuildingChain() && m_connected
        && closedRoute && !m_heldPanCreate && current && current()) {
        m_heldPanCreate = current;
        qCInfo(lcAnan) << "ANAN: Add Panadapter held until the receiver chain rebuild ends";
        return ReceiveDispatch::Dispatched;
    }
    return IRadioBackend::requestPanLifecycle(request, completion, current);
}

void AnanBackend::openHeldPanadapter()
{
    const RadioControlFence current = std::exchange(m_heldPanCreate, {});
    if (!current || rebuildingChain())
        return;
    // A new session, or RadioModel's deadline passed and it has answered the
    // request itself: opening a receiver now would surprise the operator.
    if (!current()) {
        qCInfo(lcAnan) << "ANAN: the held Add Panadapter is no longer current; nothing opened";
        return;
    }
    createPanadapter();
}

void AnanBackend::resumeAfterRebuild()
{
    if (rebuildingChain())
        return;
    openHeldPanadapter();
    retryPendingRateChange();
}

bool AnanBackend::removePanadapter(const QString& panId)
{
    Receiver* const rx = receiverForPan(panId);
    if (!rx)
        return false;
    // RX1 carries the link DDC, the radio's speaker and the droop sweep.
    if (rx == &rx1()) {
        qCWarning(lcAnan) << "ANAN: RX1's panadapter stays open; close RX2's instead";
        return false;
    }
    closeReceiver(*rx);
    return true;
}

void AnanBackend::setActiveSlice(int sliceId)
{
    Receiver* const rx = receiverForSlice(sliceId);
    if (!rx || rx->index == m_activeReceiver)
        return;
    const int previous = m_activeReceiver;
    m_activeReceiver = rx->index;
    // The radio echoes nothing, so the old slice is cleared here (the seam's
    // contract for a backend without that echo).
    emitSliceState(m_receivers[static_cast<std::size_t>(previous)]);
    emitSliceState(*rx);
}

void AnanBackend::openReceiver(Receiver& rx)
{
    // Never opened, or not remembered: start where RX1 is, so its pan is not of
    // DC. Opened before, it keeps its own dial, mode and rate.
    const Receiver& first = rx1();
    if (rx.freqHz <= 0.0) {
        rx.freqHz = first.freqHz;
        rx.mode = first.mode;
        rx.filterLowHz = first.filterLowHz;
        rx.filterHighHz = first.filterHighHz;
        rx.rateKsps = first.rateKsps;
    }
    rx.open = true;
    rx.reopenFailed = false;
    rx.tuneDirty = false;
    rx.sMeter.reset();
    prepareDspConfig(rx);
    m_speakerMix.setInputActive(rx.index, true);

    // Native creation: RadioModel finds the new pan and slice before this
    // returns. The DDC joins the session only once its chain is built.
    emitPanState(rx);
    emitSliceState(rx);
    emitPanLimits(rx);
    defineLevelMeter(rx);

    m_openingReceiver = rx.index;
    rx.openBuild = ++m_nextOpenBuild;
    startDspBuild(rx, rx.openBuild, &AnanBackend::finishReceiverOpen);
    // The shift is the DSP's own state, not the build's; queued after the
    // build begins, the install keeps it.
    pushModeFilterShift(rx);
    emit operatingStateChanged();   // Receivers capture
}

void AnanBackend::finishReceiverOpen(int index, quint64 build, bool ok, const QString& error)
{
    Receiver& rx = m_receivers[static_cast<std::size_t>(index)];
    // Closed, reopened or disconnected while it built.
    if (!rx.open || rx.openBuild != build)
        return;
    rx.openBuild = 0;
    if (!ok) {
        qCWarning(lcAnan).noquote() << QStringLiteral("ANAN: RX%1 could not be opened -- %2")
                                           .arg(rx.index + 1).arg(error);
        emit sliceLifecycleFailed(QStringLiteral("create"), rx.index, error);
        closeReceiver(rx);   // also ends the open and resumes what waited for it
        return;
    }
    m_openingReceiver = -1;
    // Enabled first, then tuned: P2Client ignores a word for a DDC it does not
    // run, and both run in this order on its thread.
    sendActiveDdcs();
    if (m_client) {
        const int ddc = rx.route.ddc;
        const double hz = rx.freqHz;
        QMetaObject::invokeMethod(m_client,
                                  [this, ddc, hz]() { m_client->setDdcFrequencyHz(ddc, hz); },
                                  Qt::QueuedConnection);
    }
    resumeAfterRebuild();
}

void AnanBackend::closeReceiver(Receiver& rx)
{
    rx.open = false;
    rx.tuneDirty = false;
    rx.openBuild = 0;   // a build still in flight finishes into nothing
    if (m_pendingBandwidthKsps != 0 && m_pendingBandwidthReceiver == rx.index)
        m_pendingBandwidthKsps = 0;
    m_speakerMix.setInputActive(rx.index, false);
    sendActiveDdcs();

    if (m_activeReceiver == rx.index) {
        m_activeReceiver = rx1().index;
        emitSliceState(rx1());
    }
    // Closing the receiver emits both, or the stale slice keeps counting
    // against maxSlices (IRadioBackend::sliceRemoved).
    emit meterRemoved(levelMeterIndex(rx.index));
    emit sliceRemoved(rx.index);
    emit panRemoved(rx.panId);

    if (m_openingReceiver == rx.index) {
        m_openingReceiver = -1;
        resumeAfterRebuild();
    }
    emit operatingStateChanged();   // Receivers capture
}

void AnanBackend::sendActiveDdcs()
{
    // Each send reads the list as it stands then, so a later open or close
    // overtakes an earlier one's resends instead of being undone by them. Sent
    // three times, as a live rate change is (kRateChangeResendMs): UDP, and the
    // keepalive never resends DDC-Specific.
    auto send = [this]() {
        if (!m_connected || !m_client)
            return;
        const std::vector<DdcConfig> ddcs = sessionParams().activeDdcs;
        QMetaObject::invokeMethod(m_client,
                                  [client = m_client, ddcs]() { client->setActiveDdcsLive(ddcs); },
                                  Qt::QueuedConnection);
    };
    send();
    for (const int delayMs : kRateChangeResendMs)
        QTimer::singleShot(delayMs, this, send);
}

void AnanBackend::resetSpeakerResamplers()
{
    m_speakerResampleL.reset();
    m_speakerResampleR.reset();
    m_speakerOutL.clear();
    m_speakerOutR.clear();
    if (!m_speakerAudioEnabled) {
        return;
    }
    // RX1's rate: RX1 is the receiver the radio's speaker plays.
    const int src = rx1().pendingDspConfig.audioSampleRateHz;
    if (src <= 0 || src == kSpeakerSampleRateHz) {
        // Already at the stream's rate: no converter, and none is built rather
        // than a 1:1 one being built and trusted to be transparent. A ratio-1
        // resampler still costs a filter and still adds its group delay.
        return;
    }
    m_speakerResampleL = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
    m_speakerResampleR = std::make_unique<Resampler>(static_cast<double>(src),
                                                     static_cast<double>(kSpeakerSampleRateHz));
}

void AnanBackend::sendSpeakerAudioToRadio(const QByteArray& stereoFloat)
{
    if (!m_speakerAudioEnabled || m_client == nullptr) {
        return;
    }
    const std::size_t frames =
        static_cast<std::size_t>(stereoFloat.size()) / (2 * sizeof(float));
    if (frames == 0) {
        return;
    }
    const auto* in = reinterpret_cast<const float*>(stereoFloat.constData());

    // Split before converting. The two channels have to travel through separate
    // converters to keep their difference (see the member declaration), and a
    // deinterleaved copy is what a mono converter takes.
    m_speakerSrcL.resize(frames);
    m_speakerSrcR.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        m_speakerSrcL[i] = in[i * 2];
        m_speakerSrcR[i] = in[i * 2 + 1];
    }

    const float* outL = m_speakerSrcL.data();
    const float* outR = m_speakerSrcR.data();
    std::size_t outFrames = frames;
    if (m_speakerResampleL && m_speakerResampleR) {
        const int nL = m_speakerResampleL->process(m_speakerSrcL.data(),
                                                   static_cast<int>(frames), m_speakerOutL);
        const int nR = m_speakerResampleR->process(m_speakerSrcR.data(),
                                                   static_cast<int>(frames), m_speakerOutR);
        // Identical configuration and identical input length, so these agree;
        // taking the shorter is not a correction but a refusal to walk off the
        // end of one buffer if they ever stop agreeing. A converter's first
        // calls legitimately return 0 while its filter fills.
        const int n = std::min(nL, nR);
        if (n <= 0) {
            return;
        }
        outFrames = static_cast<std::size_t>(n);
        outL = reinterpret_cast<const float*>(m_speakerOutL.constData());
        outR = reinterpret_cast<const float*>(m_speakerOutR.constData());
    }

    // The RADIO's own level, on top of the per-slice stage; applied here so turning
    // the radio down never quiets the computer's speakers. A mute sends SILENCE, it
    // does not stop sending: stopping would underflow the codec FIFO (polluting the
    // underflow signal and clicking on unmute) and invalidate the pacer's estimate.
    const float lineout = m_lineoutMuted ? 0.0f
                                         : sliceAudioAmplitude(m_lineoutGainPercent);

    // Interleave and quantise. sliceAudioToInt16() carries the
    // clamp-before-scale law and the non-finite guard, with the tests for both.
    m_speakerInterleaved.resize(outFrames * 2);
    for (std::size_t i = 0; i < outFrames; ++i) {
        m_speakerInterleaved[i * 2] = sliceAudioToInt16(outL[i] * lineout);
        m_speakerInterleaved[i * 2 + 1] = sliceAudioToInt16(outR[i] * lineout);
    }

    // One queued hand-off per block. P2Client owns the socket, the packetising
    // and the pacing on its own thread; this side owns the DSP and the audio
    // stage. Copying the block is the price of that boundary and it is a few
    // hundred bytes.
    QMetaObject::invokeMethod(m_client, "enqueueSpeakerAudio", Qt::QueuedConnection,
        Q_ARG(QByteArray, QByteArray(reinterpret_cast<const char*>(m_speakerInterleaved.data()),
                                     static_cast<qsizetype>(outFrames * 2 * sizeof(qint16)))));
}

void AnanBackend::scheduleTuneApply()
{
    if (!m_tuneThrottleTimer->isActive()) {
        // Leading edge: nothing in flight, apply immediately so the first
        // move of a gesture has no added latency.
        applyTuneToRadioAndPan();
        m_tuneThrottleTimer->start(kTuneThrottleMs);
    } else {
        // Already inside a cooldown window from a very recent apply --
        // remember that a trailing catch-up is owed once it elapses, rather
        // than doing the expensive work again right now. The receiver's freqHz
        // already holds the latest value (set by the caller before this
        // runs), so the timeout handler always applies the newest position,
        // not a stale intermediate one.
        m_tunePendingApply = true;
    }
}

void AnanBackend::applyTuneToRadioAndPan()
{
    // The two genuinely expensive things a retune does: an actual UDP
    // High Priority packet to the radio (P2Client::setDdcFrequencyHz --
    // real hardware DDC/NCO retuning, which is not glitch-free at the rate
    // a raw mouse-move stream would otherwise drive it), and the pan's
    // center re-broadcast (PanadapterModel::setCenterBandwidth(), which
    // re-lays-out the spectrum/waterfall geometry on every call). Both are
    // throttled together by scheduleTuneApply() -- see its comment. Only the
    // receivers retuned since the last apply.
    for (Receiver& rx : m_receivers) {
        if (!rx.tuneDirty)
            continue;
        rx.tuneDirty = false;
        if (m_client && m_connected) {
            const int ddc = rx.route.ddc;
            const double hz = rx.freqHz;
            QMetaObject::invokeMethod(m_client,
                                      [this, ddc, hz]() { m_client->setDdcFrequencyHz(ddc, hz); },
                                      Qt::QueuedConnection);
        }
        emitPanState(rx);
    }
    // Tuning capture, once per settled gesture. Outside the connected guard:
    // where the dial ended up is worth keeping even during a teardown.
    emit operatingStateChanged();
}

}  // namespace AetherSDR::anan
