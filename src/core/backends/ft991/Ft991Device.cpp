#include "core/backends/ft991/Ft991Device.h"

#include "core/backends/ft991/Ft991Cat.h"
#include "core/backends/ft991/Ft991Spectrum.h"

#include <QAudioSink>
#include <QAudioSource>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QSerialPort>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_LOGGING_CATEGORY(lcFt991Dev, "aether.ft991.device")

namespace AetherSDR::ft991 {

namespace {

constexpr double kPi = 3.14159265358979323846;

QAudioDevice findAudioDevice(const QList<QAudioDevice>& devices,
                             const QString& hint)
{
    const QString needle = hint.toLower();
    for (const QAudioDevice& dev : devices) {
        if (dev.description().toLower().contains(needle))
            return dev;
    }
    return {};
}

// The query frame a response of `kind` answers — how one-in-flight matching
// works without tracking sequence numbers the wire does not have.
QByteArray queryForKind(Ft991Cat::Response::Kind kind)
{
    using Kind = Ft991Cat::Response::Kind;
    switch (kind) {
    case Kind::Frequency: return Ft991Cat::queryFrequency();
    case Kind::Mode:      return Ft991Cat::queryMode();
    case Kind::Tx:        return Ft991Cat::queryPtt();
    case Kind::SMeter:    return Ft991Cat::querySMeter();
    case Kind::Power:     return Ft991Cat::queryPower();
    case Kind::Agc:       return Ft991Cat::queryAgc();
    case Kind::Id:        return Ft991Cat::queryId();
    case Kind::Width:     return Ft991Cat::queryWidth();
    case Kind::Narrow:    return Ft991Cat::queryNarrow();
    case Kind::NoiseBlanker:      return Ft991Cat::queryNoiseBlanker();
    case Kind::NoiseBlankerLevel: return Ft991Cat::queryNoiseBlankerLevel();
    case Kind::AutoNotch:         return Ft991Cat::queryAutoNotch();
    case Kind::NoiseReduction:      return Ft991Cat::queryNoiseReduction();
    case Kind::NoiseReductionLevel: return Ft991Cat::queryNoiseReductionLevel();
    case Kind::ManualNotch:       return Ft991Cat::queryManualNotch();
    case Kind::ManualNotchFreq:   return Ft991Cat::queryManualNotchHz();
    case Kind::TxPowerMeter:      return Ft991Cat::queryTxPowerMeter();
    case Kind::TxSwrMeter:        return Ft991Cat::queryTxSwrMeter();
    case Kind::Info:              return Ft991Cat::queryInfo();
    default:              return {};
    }
}

}  // namespace

Ft991Device::Ft991Device(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    qRegisterMetaType<Ft991Device::Params>(
        "AetherSDR::ft991::Ft991Device::Params");
    qRegisterMetaType<Ft991Device::OpenInfo>(
        "AetherSDR::ft991::Ft991Device::OpenInfo");
}

Ft991Device::~Ft991Device()
{
    closeDevice();
}

void Ft991Device::openDevice(const Params& params)
{
    closeDevice();
    m_params = params;

    m_serial = new QSerialPort(params.portName, this);
    m_serial->setBaudRate(params.baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    // 8N2 per the CAT manual — the FT-991 frames with two stop bits.
    m_serial->setStopBits(QSerialPort::TwoStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_serial->open(QIODevice::ReadWrite)) {
        const QString err = m_serial->errorString();
        m_serial->deleteLater();
        m_serial = nullptr;
        emit openFailed(QStringLiteral("cannot open %1: %2")
                            .arg(params.portName, err));
        return;
    }
    // Never key anything through modem lines: this backend keys via CAT
    // only, and a station wired for line PTT must not transmit on connect
    // (Principle VI).
    m_serial->setDataTerminalReady(false);
    m_serial->setRequestToSend(false);
    connect(m_serial, &QSerialPort::readyRead,
            this, &Ft991Device::onSerialReadyRead);
    // USB unplug lands here as ResourceError (Windows) — without this the
    // session is a zombie: no readyRead ever again, no error surfaced, the
    // heartbeat just goes quiet. PermanentError is the POSIX sibling.
    connect(m_serial, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::ResourceError
            || error == QSerialPort::ReadError) {
            failLink(QStringLiteral(
                "serial port %1 lost (USB cable removed?)")
                    .arg(m_params.portName));
        }
    });

    if (!m_pollTimer) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(kPollIntervalMs);
        connect(m_pollTimer, &QTimer::timeout, this, &Ft991Device::onPollTick);
    }

    m_rxBuf.clear();
    m_pendingQueries.clear();
    m_inFlight.clear();
    m_tick = 0;
    m_idAttempts = 0;
    m_consecutiveTimeouts = 0;
    m_haveFreq = false;
    m_haveMode = false;
    m_freqHz = 0.0;
    m_mode.clear();
    m_txState = 0;
    m_powerWatts = -1;
    m_agc.clear();
    m_pttRequested = false;
    m_shIndex = -1;
    m_narrow = -1;
    m_nb = -1;
    m_nbLevel = -1;
    m_autoNotch = -1;
    m_nr = -1;
    m_nrLevel = -1;
    m_manualNotch = -1;
    m_manualNotchHz = -1;
    m_ritOn = -1;
    m_xitOn = -1;
    m_clarifierHz = 0;
    m_haveClarifier = false;

    // Polled operation is the contract (design doc): push mode off, then
    // verify who is answering before anything else is said.
    writeFrame(Ft991Cat::setAutoInformation(false));
    m_state = State::AwaitId;
    enqueueQuery(Ft991Cat::queryId());
    trySendNextQuery();
    m_pollTimer->start();
}

void Ft991Device::closeDevice()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    stopTune();
    if (m_serial) {
        // Safety before anything else: if we keyed the radio, unkey it.
        if (m_pttRequested)
            writeFrame(Ft991Cat::setPtt(false));
        m_serial->close();
        m_serial->deleteLater();
        m_serial = nullptr;
    }
    stopTxSink();
    if (m_audioIn) {
        m_audioIn->stop();
        m_audioIn->deleteLater();
        m_audioIn = nullptr;
    }
    if (m_capSink) {
        m_capSink->close();
        m_capSink->deleteLater();
        m_capSink = nullptr;
    }
    m_spectrum.reset();
    m_state = State::Idle;
    m_pttRequested = false;
    m_pendingQueries.clear();
    m_inFlight.clear();
}

// ---------------------------------------------------------------------------
// CAT plane
// ---------------------------------------------------------------------------

void Ft991Device::writeFrame(const QByteArray& frame)
{
    if (!m_serial || frame.isEmpty())
        return;
    m_serial->write(frame);
    m_serialTxBytes.fetch_add(static_cast<std::uint64_t>(frame.size()));
}

void Ft991Device::enqueueQuery(const QByteArray& frame)
{
    if (frame.isEmpty() || m_inFlight == frame
        || m_pendingQueries.contains(frame))
        return;
    m_pendingQueries.append(frame);
}

void Ft991Device::trySendNextQuery()
{
    if (!m_inFlight.isEmpty() || m_pendingQueries.isEmpty())
        return;
    m_inFlight = m_pendingQueries.takeFirst();
    m_inFlightClock.restart();
    writeFrame(m_inFlight);
}

void Ft991Device::onSerialReadyRead()
{
    const QByteArray chunk = m_serial->readAll();
    m_serialRxBytes.fetch_add(static_cast<std::uint64_t>(chunk.size()));
    m_rxBuf.append(chunk);
    // Frames are ';'-terminated; anything before the first terminator that
    // never completes is bounded by the buffer cap below.
    int at = -1;
    while ((at = m_rxBuf.indexOf(';')) >= 0) {
        const QByteArray frame = m_rxBuf.left(at);
        m_rxBuf.remove(0, at + 1);
        handleFrame(frame.trimmed());
    }
    if (m_rxBuf.size() > 256) {
        qCWarning(lcFt991Dev) << "CAT buffer overrun; discarding"
                              << m_rxBuf.size() << "bytes";
        m_rxBuf.clear();
    }
}

void Ft991Device::handleFrame(const QByteArray& frame)
{
    const auto parsed = Ft991Cat::parse(frame);
    if (!parsed)
        return;
    m_consecutiveTimeouts = 0;   // the radio is talking
    using Kind = Ft991Cat::Response::Kind;
    const Ft991Cat::Response& r = *parsed;

    // Release the in-flight query this frame answers. "?" answers whatever
    // was asked (the radio refused it); an unsolicited frame releases
    // nothing.
    if (!m_inFlight.isEmpty()) {
        const bool answers = (r.kind == Kind::Rejected)
            || (queryForKind(r.kind) == m_inFlight);
        if (answers) {
            const int rtt = static_cast<int>(m_inFlightClock.elapsed());
            const int prev = m_rttMs.load();
            m_rttMs.store(prev < 0 ? rtt : (prev * 3 + rtt) / 4);
            m_inFlight.clear();
        }
    }

    switch (r.kind) {
    case Kind::Id: {
        if (m_state != State::AwaitId)
            break;
        const QString id = r.id.trimmed();
        if (id != QLatin1String(Ft991Cat::kRadioId)) {
            failOpen(QStringLiteral(
                "the device on %1 answered CAT ID \"%2\" — expected %3 "
                "(FT-991/FT-991A)")
                    .arg(m_params.portName, id,
                         QLatin1String(Ft991Cat::kRadioId)));
            return;
        }
        qCInfo(lcFt991Dev) << "FT-991 verified on" << m_params.portName;
        m_state = State::AwaitInitial;
        // Dial and mode BEFORE opened(): the backend's first pan/slice
        // emission must carry the radio's real state, not a placeholder.
        enqueueQuery(Ft991Cat::queryFrequency());
        enqueueQuery(Ft991Cat::queryMode());
        enqueueQuery(Ft991Cat::queryPower());
        enqueueQuery(Ft991Cat::queryAgc());
        enqueueQuery(Ft991Cat::queryPtt());
        enqueueQuery(Ft991Cat::queryNarrow());
        enqueueQuery(Ft991Cat::queryWidth());
        enqueueQuery(Ft991Cat::queryNoiseBlanker());
        enqueueQuery(Ft991Cat::queryNoiseBlankerLevel());
        enqueueQuery(Ft991Cat::queryAutoNotch());
        enqueueQuery(Ft991Cat::queryNoiseReduction());
        enqueueQuery(Ft991Cat::queryNoiseReductionLevel());
        enqueueQuery(Ft991Cat::queryManualNotch());
        enqueueQuery(Ft991Cat::queryManualNotchHz());
        break;
    }
    case Kind::Frequency:
        if (r.frequencyHz != m_freqHz) {
            m_freqHz = r.frequencyHz;
            emit catFrequency(m_freqHz);
        }
        m_haveFreq = true;
        break;
    case Kind::Mode:
        if (r.mode != m_mode) {
            m_mode = r.mode;
            emit catMode(m_mode);
            // The SH tables and NA bank are per-mode: a mode change (ours
            // or the radio's own) invalidates the cached width state.
            enqueueQuery(Ft991Cat::queryNarrow());
            enqueueQuery(Ft991Cat::queryWidth());
        }
        m_haveMode = true;
        break;
    case Kind::Tx:
        if (r.txState != m_txState) {
            m_txState = r.txState;
            emit catTxState(m_txState);
        }
        break;
    case Kind::SMeter:
        emit catSMeter(r.raw);
        break;
    case Kind::Info: {
        // IF carries the dial and mode too; feed them through the same
        // change gates as the dedicated polls so the two agree.
        if (r.frequencyHz != m_freqHz) {
            m_freqHz = r.frequencyHz;
            emit catFrequency(m_freqHz);
        }
        m_haveFreq = true;
        const bool rit = r.ritOn;
        const bool xit = r.xitOn;
        if (!m_haveClarifier || m_ritOn != int(rit) || m_xitOn != int(xit)
            || m_clarifierHz != r.clarifierHz) {
            m_haveClarifier = true;
            m_ritOn = rit;
            m_xitOn = xit;
            m_clarifierHz = r.clarifierHz;
            qCDebug(lcFt991Dev) << "clarifier: rit" << rit << "xit" << xit
                                << "offset" << r.clarifierHz << "Hz";
            emit catClarifier(rit, xit, r.clarifierHz);
        }
        break;
    }
    case Kind::TxPowerMeter:
        emit catTxPowerMeter(r.raw);
        break;
    case Kind::TxSwrMeter:
        emit catTxSwrMeter(r.raw);
        break;
    case Kind::Power:
        if (r.raw != m_powerWatts) {
            m_powerWatts = r.raw;
            emit catPower(m_powerWatts);
        }
        break;
    case Kind::Agc:
        if (r.agcMode != m_agc) {
            m_agc = r.agcMode;
            emit catAgc(m_agc);
        }
        break;
    case Kind::Width:
        if (r.raw != m_shIndex) {
            m_shIndex = r.raw;
            emit catWidth(m_shIndex);
        }
        break;
    case Kind::Narrow:
        if (r.raw != m_narrow) {
            m_narrow = r.raw;
            emit catNarrow(m_narrow != 0);
        }
        break;
    case Kind::NoiseBlanker:
        if (r.raw != m_nb) {
            m_nb = r.raw;
            emit catNoiseBlanker(m_nb != 0);
        }
        break;
    case Kind::NoiseBlankerLevel:
        if (r.raw != m_nbLevel) {
            m_nbLevel = r.raw;
            emit catNoiseBlankerLevel(m_nbLevel);
        }
        break;
    case Kind::AutoNotch:
        if (r.raw != m_autoNotch) {
            m_autoNotch = r.raw;
            emit catAutoNotch(m_autoNotch != 0);
        }
        break;
    case Kind::NoiseReduction:
        if (r.raw != m_nr) {
            m_nr = r.raw;
            emit catNoiseReduction(m_nr != 0);
        }
        break;
    case Kind::NoiseReductionLevel:
        if (r.raw != m_nrLevel) {
            m_nrLevel = r.raw;
            emit catNoiseReductionLevel(m_nrLevel);
        }
        break;
    case Kind::ManualNotch:
        if (r.raw != m_manualNotch) {
            m_manualNotch = r.raw;
            emit catManualNotch(m_manualNotch != 0);
        }
        break;
    case Kind::ManualNotchFreq:
        if (r.raw != m_manualNotchHz) {
            m_manualNotchHz = r.raw;
            emit catManualNotchHz(m_manualNotchHz);
        }
        break;
    case Kind::Rejected:
        qCDebug(lcFt991Dev) << "CAT command refused";
        break;
    case Kind::Unknown:
        // An IF we could not read is worth saying out loud ONCE: the whole
        // clarifier readback rides on that frame's field offsets, and the
        // failure is otherwise completely silent (RIT would simply never
        // move). Rate-limited to the first, so a chatty radio cannot flood.
        if (frame.startsWith("IF") && !m_loggedBadInfo) {
            m_loggedBadInfo = true;
            qCWarning(lcFt991Dev)
                << "unparsed IF response (" << frame.size() << "chars):"
                << frame;
        }
        break;
    }

    if (m_state == State::AwaitInitial && m_haveFreq && m_haveMode) {
        OpenInfo info;
        QString error;
        if (!openAudio(&info, &error)) {
            failOpen(error);
            return;
        }
        m_state = State::Running;
        emit opened(info);
    }

    trySendNextQuery();
}

void Ft991Device::failOpen(const QString& reason)
{
    closeDevice();
    emit openFailed(reason);
}

void Ft991Device::failLink(const QString& reason)
{
    // Pre-open failures keep the openFailed vocabulary; only a RUNNING
    // session reports a lost link.
    if (m_state != State::Running) {
        failOpen(reason);
        return;
    }
    qCWarning(lcFt991Dev) << "link lost:" << reason;
    closeDevice();
    emit linkLost(reason);
}

void Ft991Device::onPollTick()
{
    ++m_tick;

    // Expire a query the radio never answered so the plane cannot wedge.
    if (!m_inFlight.isEmpty()
        && m_inFlightClock.elapsed() > kQueryTimeoutMs) {
        m_catTimeouts.fetch_add(1);
        const QByteArray lost = m_inFlight;
        m_inFlight.clear();
        if (m_state == State::AwaitId) {
            if (++m_idAttempts >= kIdRetries) {
                failOpen(QStringLiteral(
                    "no CAT response on %1 at %2 baud — check the port and "
                    "menu 031 CAT RATE")
                        .arg(m_params.portName)
                        .arg(m_params.baudRate));
                return;
            }
            enqueueQuery(Ft991Cat::queryId());
        } else {
            qCDebug(lcFt991Dev) << "CAT query timed out:" << lost;
            if (m_state == State::Running
                && ++m_consecutiveTimeouts >= kMaxConsecutiveTimeouts) {
                failLink(QStringLiteral(
                    "CAT on %1 stopped answering — radio powered off?")
                        .arg(m_params.portName));
                return;
            }
        }
    }

    // The codec dying (USB unplug takes it with the serial port; the audio
    // half can also die alone) has no signal of its own worth trusting —
    // poll the error state once a second.
    if (m_state == State::Running && m_tick % 10 == 0 && m_audioIn
        && m_audioIn->error() == QAudio::IOError) {
        failLink(QStringLiteral("audio capture from \"%1\" failed")
                     .arg(m_params.audioInHint));
        return;
    }

    if (m_state == State::Running) {
        // The poll schedule. The S-meter leads (it is the fastest-moving
        // display); dial and TX state follow at a few Hz so turning the
        // radio's own knob lands within a poll period; drive and AGC crawl.
        if (m_txState == 0)
            enqueueQuery(Ft991Cat::querySMeter());
        else if (m_tick % 2 == 0)
            enqueueQuery(Ft991Cat::queryTxPowerMeter());
        else
            enqueueQuery(Ft991Cat::queryTxSwrMeter());
        if (m_tick % 3 == 1)
            enqueueQuery(Ft991Cat::queryFrequency());
        if (m_tick % 5 == 2)
            enqueueQuery(Ft991Cat::queryPtt());
        if (m_tick % 10 == 4)
            enqueueQuery(Ft991Cat::queryMode());
        if (m_tick % 50 == 7)
            enqueueQuery(Ft991Cat::queryPower());
        if (m_tick % 50 == 27)
            enqueueQuery(Ft991Cat::queryAgc());
        // Radio-side DSP reflection, one query per ~5 s each, phase-spread:
        // enough to follow the radio's own WIDTH/NB/DNF controls without
        // crowding the fast dial/meter polls.
        // IF: the clarifier's only readback (and a free dial cross-check).
        if (m_tick % 10 == 6)
            enqueueQuery(Ft991Cat::queryInfo());
        if (m_tick % 50 == 12)
            enqueueQuery(Ft991Cat::queryNarrow());
        if (m_tick % 50 == 17)
            enqueueQuery(Ft991Cat::queryWidth());
        if (m_tick % 50 == 22)
            enqueueQuery(Ft991Cat::queryNoiseReduction());
        if (m_tick % 50 == 24)
            enqueueQuery(Ft991Cat::queryNoiseReductionLevel());
        if (m_tick % 50 == 32)
            enqueueQuery(Ft991Cat::queryNoiseBlanker());
        if (m_tick % 50 == 34)
            enqueueQuery(Ft991Cat::queryNoiseBlankerLevel());
        if (m_tick % 50 == 37)
            enqueueQuery(Ft991Cat::queryAutoNotch());
        if (m_tick % 50 == 42)
            enqueueQuery(Ft991Cat::queryManualNotch());
        if (m_tick % 50 == 47)
            enqueueQuery(Ft991Cat::queryManualNotchHz());
    }

    trySendNextQuery();
}

// ---------------------------------------------------------------------------
// Control verbs (invoked queued from the backend)
// ---------------------------------------------------------------------------

void Ft991Device::setFrequencyHz(double hz)
{
    writeFrame(Ft991Cat::setFrequency(hz));
    enqueueQuery(Ft991Cat::queryFrequency());
    trySendNextQuery();
}

void Ft991Device::setMode(const QString& neutral)
{
    writeFrame(Ft991Cat::setMode(neutral));
    enqueueQuery(Ft991Cat::queryMode());
    trySendNextQuery();
}

void Ft991Device::setPtt(bool tx)
{
    m_pttRequested = tx;
    writeFrame(Ft991Cat::setPtt(tx));
    enqueueQuery(Ft991Cat::queryPtt());
    if (tx)
        startTxSink();
    else
        stopTxSink();
    trySendNextQuery();
}

void Ft991Device::setPowerWatts(int watts)
{
    writeFrame(Ft991Cat::setPowerWatts(watts));
    enqueueQuery(Ft991Cat::queryPower());
    trySendNextQuery();
}

void Ft991Device::setAgc(const QString& neutral)
{
    writeFrame(Ft991Cat::setAgc(neutral));
    enqueueQuery(Ft991Cat::queryAgc());
    trySendNextQuery();
}

void Ft991Device::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? 1000 / fps : 0;
}

void Ft991Device::setRadioWidth(const QString& neutralMode, int widthHz)
{
    const int index = Ft991Cat::nearestWidthIndex(neutralMode, widthHz);
    if (index < 0)
        return;   // AM/FM: fixed width, nothing to command
    // NA FIRST (the CAT manual's order): the SH index lands in the narrow or
    // wide bank depending on it.
    writeFrame(Ft991Cat::setNarrow(
        Ft991Cat::narrowForWidth(neutralMode, widthHz)));
    writeFrame(Ft991Cat::setWidthIndex(index));
    enqueueQuery(Ft991Cat::queryNarrow());
    enqueueQuery(Ft991Cat::queryWidth());
    trySendNextQuery();
}

void Ft991Device::setRadioNoiseBlanker(bool on)
{
    writeFrame(Ft991Cat::setNoiseBlanker(on));
    enqueueQuery(Ft991Cat::queryNoiseBlanker());
    trySendNextQuery();
}

void Ft991Device::setRadioNoiseBlankerLevel(int level0to10)
{
    writeFrame(Ft991Cat::setNoiseBlankerLevel(level0to10));
    enqueueQuery(Ft991Cat::queryNoiseBlankerLevel());
    trySendNextQuery();
}

void Ft991Device::setRadioAutoNotch(bool on)
{
    writeFrame(Ft991Cat::setAutoNotch(on));
    enqueueQuery(Ft991Cat::queryAutoNotch());
    trySendNextQuery();
}

void Ft991Device::setRadioNoiseReduction(bool on)
{
    writeFrame(Ft991Cat::setNoiseReduction(on));
    enqueueQuery(Ft991Cat::queryNoiseReduction());
    trySendNextQuery();
}

void Ft991Device::setRadioNoiseReductionLevel(int level1to15)
{
    writeFrame(Ft991Cat::setNoiseReductionLevel(level1to15));
    enqueueQuery(Ft991Cat::queryNoiseReductionLevel());
    trySendNextQuery();
}

void Ft991Device::setRadioClarifier(bool ritOn, bool xitOn, int offsetHz)
{
    // Offset first: enabling a clarifier that still holds the previous
    // offset would pull the receiver off frequency for a poll period.
    writeFrame(Ft991Cat::setClarifierOffset(offsetHz));
    writeFrame(Ft991Cat::setRitEnabled(ritOn));
    writeFrame(Ft991Cat::setXitEnabled(xitOn));
    enqueueQuery(Ft991Cat::queryInfo());
    trySendNextQuery();
}

void Ft991Device::setRadioManualNotch(bool on, int hz)
{
    // Position before enable, so the notch never bites at a stale offset.
    if (hz > 0)
        writeFrame(Ft991Cat::setManualNotchHz(hz));
    writeFrame(Ft991Cat::setManualNotch(on));
    enqueueQuery(Ft991Cat::queryManualNotchHz());
    enqueueQuery(Ft991Cat::queryManualNotch());
    trySendNextQuery();
}

void Ft991Device::setAudioPassband(int lowHz, int highHz)
{
    // SliceModel's signed edges -> the audio band the codec carries. A
    // sideband pair (USB 100..2900, LSB -2900..-100) is the same audio
    // band either way; a symmetric passband (AM -4000..4000) means BOTH
    // sidebands landed in 0..4000 audio, so the low edge is 0, not 4000.
    const bool straddlesZero = (lowHz < 0) != (highHz < 0);
    const int absLo = std::min(std::abs(lowHz), std::abs(highHz));
    const int absHi = std::max(std::abs(lowHz), std::abs(highHz));
    const double loEdge = straddlesZero ? 0.0 : static_cast<double>(absLo);
    const double hiEdge = static_cast<double>(absHi);

    // 4th-order Butterworth = two cascaded sections at these Qs.
    constexpr double kQ1 = 0.54119610;
    constexpr double kQ2 = 1.30656296;
    constexpr double kNyquistGuard = kEngineRxRateHz * 0.45;

    // Wide-open edges are SKIPPED, not configured at an extreme corner —
    // no group delay for a filter that would pass everything anyway.
    m_rxHighpassOn = loEdge >= 30.0;
    if (m_rxHighpassOn) {
        m_rxHighpass[0].setCoefficients(Biquad::Type::HighPass,
                                        kEngineRxRateHz, loEdge, kQ1);
        m_rxHighpass[1].setCoefficients(Biquad::Type::HighPass,
                                        kEngineRxRateHz, loEdge, kQ2);
    }
    m_rxLowpassOn = hiEdge > 0.0 && hiEdge < kNyquistGuard;
    if (m_rxLowpassOn) {
        m_rxLowpass[0].setCoefficients(Biquad::Type::LowPass,
                                       kEngineRxRateHz, hiEdge, kQ1);
        m_rxLowpass[1].setCoefficients(Biquad::Type::LowPass,
                                       kEngineRxRateHz, hiEdge, kQ2);
    }
    // Discontinuous retune: old state at the new corner is just a click.
    for (Biquad& b : m_rxHighpass)
        b.reset();
    for (Biquad& b : m_rxLowpass)
        b.reset();
    qCDebug(lcFt991Dev) << "audio passband" << loEdge << ".." << hiEdge
                        << "Hz (hp" << m_rxHighpassOn << "lp" << m_rxLowpassOn
                        << ")";
}

// ---------------------------------------------------------------------------
// RX audio
// ---------------------------------------------------------------------------

bool Ft991Device::openAudio(OpenInfo* info, QString* error)
{
    const QAudioDevice inDev =
        findAudioDevice(QMediaDevices::audioInputs(), m_params.audioInHint);
    if (inDev.isNull()) {
        *error = QStringLiteral(
            "no audio capture device matching \"%1\" — is the radio's USB "
            "cable connected?")
                .arg(m_params.audioInHint);
        return false;
    }

    // Capture at the device's NATIVE rate (the WfmDemodulator rule: forcing
    // a rate makes the OS mixer resample silently). Int16 preferred, the
    // device's own format as fallback.
    const QAudioFormat preferred = inDev.preferredFormat();
    QAudioFormat fmt;
    fmt.setSampleRate(preferred.sampleRate() > 0 ? preferred.sampleRate()
                                                 : kAudioOutRateFallbackHz);
    fmt.setChannelCount(preferred.channelCount() > 0
                            ? preferred.channelCount() : 2);
    fmt.setSampleFormat(QAudioFormat::Int16);
    if (!inDev.isFormatSupported(fmt))
        fmt = preferred;
    m_capFormat = fmt;

    m_spectrum = std::make_unique<Ft991Spectrum>(fmt.sampleRate(),
                                                 m_params.spectrumSpanHz);
    m_rxPhase = 0.0;
    m_rxLastSample = 0.0f;
    m_lastSpectrumMs = 0;
    m_spectrumClock.invalidate();

    m_capSink = new Ft991CaptureSink(this);
    m_capSink->open(QIODevice::WriteOnly);
    connect(m_capSink, &Ft991CaptureSink::pcmReady,
            this, &Ft991Device::onCapturePcm);

    m_audioIn = new QAudioSource(inDev, fmt, this);
    m_audioIn->start(m_capSink);
    if (m_audioIn->error() != QAudio::NoError) {
        *error = QStringLiteral("cannot start capture on \"%1\" (error %2)")
                     .arg(inDev.description())
                     .arg(static_cast<int>(m_audioIn->error()));
        return false;
    }

    info->coveredSpanHz = m_spectrum->coveredSpanHz();
    info->audioInDesc = inDev.description();
    info->audioInRateHz = fmt.sampleRate();

    // Resolve (but do not start) the playback device; the sink comes up on
    // key. Absence is not an error — the session is then receive-only, and
    // the health panel says so.
    m_audioOutDevice =
        findAudioDevice(QMediaDevices::audioOutputs(), m_params.audioOutHint);
    if (!m_audioOutDevice.isNull()) {
        const QAudioFormat outPreferred = m_audioOutDevice.preferredFormat();
        QAudioFormat out;
        out.setSampleRate(outPreferred.sampleRate() > 0
                              ? outPreferred.sampleRate()
                              : kAudioOutRateFallbackHz);
        out.setChannelCount(2);
        out.setSampleFormat(QAudioFormat::Int16);
        if (!m_audioOutDevice.isFormatSupported(out))
            out = outPreferred;
        m_outFormat = out;
        info->audioOutDesc = m_audioOutDevice.description();
        info->audioOutRateHz = out.sampleRate();
    } else {
        qCWarning(lcFt991Dev)
            << "no playback device matching" << m_params.audioOutHint
            << "— TX audio unavailable this session";
    }

    qCInfo(lcFt991Dev) << "capture:" << inDev.description() << "@"
                       << fmt.sampleRate() << "Hz ch" << fmt.channelCount()
                       << "fmt" << fmt.sampleFormat();
    return true;
}

void Ft991Device::appendMonoFloat(const QByteArray& pcm,
                                  std::vector<float>& mono) const
{
    const int channels = std::max(1, m_capFormat.channelCount());
    switch (m_capFormat.sampleFormat()) {
    case QAudioFormat::Int16: {
        const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
        const std::size_t frames =
            static_cast<std::size_t>(pcm.size()) / (sizeof(qint16) * channels);
        for (std::size_t i = 0; i < frames; ++i)
            mono.push_back(static_cast<float>(s[i * channels]) / 32768.0f);
        break;
    }
    case QAudioFormat::Int32: {
        const auto* s = reinterpret_cast<const qint32*>(pcm.constData());
        const std::size_t frames =
            static_cast<std::size_t>(pcm.size()) / (sizeof(qint32) * channels);
        for (std::size_t i = 0; i < frames; ++i)
            mono.push_back(static_cast<float>(s[i * channels]) / 2147483648.0f);
        break;
    }
    case QAudioFormat::Float: {
        const auto* s = reinterpret_cast<const float*>(pcm.constData());
        const std::size_t frames =
            static_cast<std::size_t>(pcm.size()) / (sizeof(float) * channels);
        for (std::size_t i = 0; i < frames; ++i)
            mono.push_back(s[i * channels]);
        break;
    }
    case QAudioFormat::UInt8: {
        const auto* s = reinterpret_cast<const quint8*>(pcm.constData());
        const std::size_t frames =
            static_cast<std::size_t>(pcm.size()) / channels;
        for (std::size_t i = 0; i < frames; ++i)
            mono.push_back((static_cast<float>(s[i * channels]) - 128.0f)
                           / 128.0f);
        break;
    }
    default:
        break;
    }
}

bool Ft991Device::spectrumFrameDue()
{
    if (m_spectrumIntervalMs <= 0)
        return true;
    if (!m_spectrumClock.isValid()) {
        m_spectrumClock.start();
        return true;
    }
    return m_spectrumClock.elapsed() - m_lastSpectrumMs
        >= m_spectrumIntervalMs;
}

void Ft991Device::onCapturePcm(const QByteArray& pcm)
{
    if (!m_spectrum)
        return;

    m_monoScratch.clear();
    appendMonoFloat(pcm, m_monoScratch);
    if (m_monoScratch.empty())
        return;

    m_audioBlocks.fetch_add(1);
    m_audioSamples.fetch_add(m_monoScratch.size());

    // ---- panadapter (at the capture rate) ----
    if (spectrumFrameDue()) {
        const int frames = m_spectrum->process(m_monoScratch, m_binsScratch);
        if (frames > 0) {
            if (m_spectrumClock.isValid())
                m_lastSpectrumMs = m_spectrumClock.elapsed();
            emit spectrumFrame(m_binsScratch);
        }
    } else {
        m_spectrum->accumulate(m_monoScratch);
    }

    // ---- speaker/decoder feed: linear resample to 24 kHz mono ----
    const double inRate = m_capFormat.sampleRate();
    const double step = inRate / static_cast<double>(kEngineRxRateHz);
    m_mono24k.clear();
    m_mono24k.reserve(static_cast<std::size_t>(
        m_monoScratch.size() / step + 4));
    // m_rxPhase in [-1, 0) indexes the PREVIOUS block's last sample
    // (m_rxLastSample), so the interpolation is continuous across blocks.
    double pos = m_rxPhase;
    const std::size_t n = m_monoScratch.size();
    while (pos < static_cast<double>(n) - 1.0) {
        float sample = 0.0f;
        if (pos < 0.0) {
            const float frac = static_cast<float>(pos + 1.0);
            sample = m_rxLastSample * (1.0f - frac) + m_monoScratch[0] * frac;
        } else {
            const std::size_t i = static_cast<std::size_t>(pos);
            const float frac = static_cast<float>(pos - static_cast<double>(i));
            sample = m_monoScratch[i] * (1.0f - frac)
                   + m_monoScratch[i + 1] * frac;
        }
        m_mono24k.push_back(sample);
        pos += step;
    }
    m_rxPhase = pos - static_cast<double>(n);
    m_rxLastSample = m_monoScratch[n - 1];

    // The slice passband (host-side; the spectrum above stays pre-filter).
    if (m_rxHighpassOn) {
        for (Biquad& b : m_rxHighpass)
            b.process(m_mono24k.data(), m_mono24k.data(),
                      static_cast<int>(m_mono24k.size()));
    }
    if (m_rxLowpassOn) {
        for (Biquad& b : m_rxLowpass)
            b.process(m_mono24k.data(), m_mono24k.data(),
                      static_cast<int>(m_mono24k.size()));
    }

    m_stereoOut.clear();
    m_stereoOut.reserve(m_mono24k.size() * 2);
    for (const float s : m_mono24k) {
        m_stereoOut.push_back(s);
        m_stereoOut.push_back(s);
    }
    if (!m_stereoOut.empty())
        emit audioBlockReady(m_stereoOut);
}

// ---------------------------------------------------------------------------
// TX audio
// ---------------------------------------------------------------------------

void Ft991Device::startTxSink()
{
    if (m_txSink || m_audioOutDevice.isNull())
        return;
    m_txSink = new QAudioSink(m_audioOutDevice, m_outFormat, this);
    // A quarter second of buffer: enough to ride scheduling jitter, small
    // enough that unkeying does not trail audio.
    m_txSink->setBufferSize(m_outFormat.bytesForDuration(250'000));
    m_txIo = m_txSink->start();
    m_txPhase = 0.0;
    m_txLastL = 0.0f;
    if (!m_txIo)
        qCWarning(lcFt991Dev) << "TX sink failed to start:"
                              << static_cast<int>(m_txSink->error());
}

void Ft991Device::stopTxSink()
{
    if (!m_txSink)
        return;
    m_txSink->stop();   // drops the queued tail — unkey means silence NOW
    m_txSink->deleteLater();
    m_txSink = nullptr;
    m_txIo = nullptr;
}

void Ft991Device::submitTxAudio(const QByteArray& int16Stereo,
                                int engineRateHz, float gain)
{
    // Inert unless WE keyed the radio: TX audio into an unkeyed radio is
    // harmless, but the gate keeps the path provably quiet (Principle VI).
    if (!m_pttRequested || !m_txIo || m_tuneToneActive || engineRateHz <= 0)
        return;

    const auto* in = reinterpret_cast<const qint16*>(int16Stereo.constData());
    const std::size_t frames =
        static_cast<std::size_t>(int16Stereo.size()) / (sizeof(qint16) * 2);
    if (frames == 0)
        return;

    // Mono mix + gain, engine rate.
    m_monoScratch.clear();
    m_monoScratch.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = static_cast<float>(in[i * 2]) / 32768.0f;
        const float r = static_cast<float>(in[i * 2 + 1]) / 32768.0f;
        m_monoScratch.push_back(std::clamp(0.5f * (l + r) * gain,
                                           -1.0f, 1.0f));
    }

    // Linear resample engine rate -> codec rate (same phase discipline as
    // the RX path), then interleave into the sink's format.
    const double step = static_cast<double>(engineRateHz)
        / std::max(1, m_outFormat.sampleRate());
    const int outCh = std::max(1, m_outFormat.channelCount());
    const bool outFloat = m_outFormat.sampleFormat() == QAudioFormat::Float;
    const int bytesPerSample = outFloat ? 4 : 2;

    m_txScratch.clear();
    double pos = m_txPhase;
    const std::size_t n = m_monoScratch.size();
    while (pos < static_cast<double>(n) - 1.0) {
        float sample = 0.0f;
        if (pos < 0.0) {
            const float frac = static_cast<float>(pos + 1.0);
            sample = m_txLastL * (1.0f - frac) + m_monoScratch[0] * frac;
        } else {
            const std::size_t i = static_cast<std::size_t>(pos);
            const float frac = static_cast<float>(pos - static_cast<double>(i));
            sample = m_monoScratch[i] * (1.0f - frac)
                   + m_monoScratch[i + 1] * frac;
        }
        for (int c = 0; c < outCh; ++c) {
            if (outFloat) {
                m_txScratch.append(reinterpret_cast<const char*>(&sample),
                                   bytesPerSample);
            } else {
                const qint16 s16 =
                    static_cast<qint16>(std::lround(sample * 32767.0f));
                m_txScratch.append(reinterpret_cast<const char*>(&s16),
                                   bytesPerSample);
            }
        }
        pos += step;
    }
    m_txPhase = pos - static_cast<double>(n);
    m_txLastL = m_monoScratch[n - 1];

    if (!m_txScratch.isEmpty())
        m_txIo->write(m_txScratch);   // partial acceptance = dropped tail; OK
}

// ---------------------------------------------------------------------------
// TUNE tone
// ---------------------------------------------------------------------------

void Ft991Device::startTune()
{
    if (m_tuneToneActive)
        return;
    m_tuneToneActive = true;
    m_tonePhase = 0.0;
    setPtt(true);
    if (!m_txIo) {
        // No playback device: a keyed SSB transmitter with no audio makes no
        // RF, so say why the TUNE button appears dead.
        qCWarning(lcFt991Dev)
            << "TUNE keyed but no TX audio device — no carrier will be made";
        return;
    }
    if (!m_toneTimer) {
        m_toneTimer = new QTimer(this);
        m_toneTimer->setInterval(20);
        connect(m_toneTimer, &QTimer::timeout, this, [this] {
            if (!m_txIo)
                return;
            const int rate = std::max(1, m_outFormat.sampleRate());
            const int outCh = std::max(1, m_outFormat.channelCount());
            const bool outFloat =
                m_outFormat.sampleFormat() == QAudioFormat::Float;
            const int samplesPer = rate / 50;   // one 20 ms block
            const double inc = 2.0 * kPi * kTuneToneHz / rate;
            m_txScratch.clear();
            for (int i = 0; i < samplesPer; ++i) {
                const float sample = static_cast<float>(
                    std::sin(m_tonePhase) * kTuneToneAmplitude);
                m_tonePhase += inc;
                for (int c = 0; c < outCh; ++c) {
                    if (outFloat) {
                        m_txScratch.append(
                            reinterpret_cast<const char*>(&sample), 4);
                    } else {
                        const qint16 s16 = static_cast<qint16>(
                            std::lround(sample * 32767.0f));
                        m_txScratch.append(
                            reinterpret_cast<const char*>(&s16), 2);
                    }
                }
            }
            if (m_tonePhase > 2.0 * kPi)
                m_tonePhase = std::fmod(m_tonePhase, 2.0 * kPi);
            m_txIo->write(m_txScratch);
        });
    }
    m_toneTimer->start();
}

void Ft991Device::stopTune()
{
    if (!m_tuneToneActive)
        return;
    m_tuneToneActive = false;
    if (m_toneTimer)
        m_toneTimer->stop();
    setPtt(false);
}

}  // namespace AetherSDR::ft991
