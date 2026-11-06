// PureSignal for the Hermes-Lite 2. See Hl2PureSignal.h for the design and the
// calcc behaviour it relies on.

#include "core/backends/hl2/Hl2PureSignal.h"

#include "core/backends/hl2/Hl2TxDsp.h"

#include <QLoggingCategory>
#include <QTimer>

#include <algorithm>
#include <cmath>

static Q_LOGGING_CATEGORY(lcHl2Ps, "aether.hl2.puresignal")

namespace AetherSDR::hl2 {

// ---- Hl2PsGain ---------------------------------------------------------------

int Hl2PsGain::stepDb(int fb) noexcept
{
    if (fb <= 0)
        return -kMaxStepDb;
    if (fb > kScale)
        return kMaxStepDb;
    static const double centre = std::sqrt(double(kWindowLow) * double(kWindowHigh));
    const long db = std::lround(20.0 * std::log10(double(fb) / centre));
    return int(std::clamp<long>(db, -kMaxStepDb, kMaxStepDb));
}

// ---- Hl2PsController ---------------------------------------------------------

Hl2PsController::Flags Hl2PsController::flagsFor(Mode m) noexcept
{
    switch (m) {
    case Mode::Auto:      return {true, false, true, false};
    case Mode::Single:    return {true, true, false, false};
    case Mode::Restoring: return {false, false, false, true};
    case Mode::Off:
    case Mode::Hold:      break;
    }
    return {true, false, false, false};
}

void Hl2PsController::select(Mode m)
{
    m_mode = m;
    m_pending = flagsFor(m);
    m_sawActive = false;
    m_collected = false;
}

void Hl2PsController::selectOff() { select(Mode::Off); }

void Hl2PsController::selectAuto() { select(Mode::Auto); }

void Hl2PsController::selectSingle()
{
    select(Mode::Single);
    m_singleStarts = 1;
    m_singleGainMoves = 0;
}

void Hl2PsController::selectRestoring() { select(Mode::Restoring); }

void Hl2PsController::reset()
{
    m_mode = Mode::Off;
    m_pending.reset();
    m_lastAttempts.reset();
    m_sawActive = false;
    m_collected = false;
    m_singleStarts = 0;
    m_singleGainMoves = 0;
}

std::optional<int> Hl2PsController::gainMove(const Report& r)
{
    // Judge each finished attempt once, and only one calcc collected after
    // the last flags: an attempt that was already being calculated when they
    // went out still reports the old gain, and it lands while calcc is back
    // in LRESET or LWAIT. Collecting states come only after that.
    const bool fresh = m_lastAttempts && r.attempts != *m_lastAttempts;
    m_lastAttempts = r.attempts;
    if (r.state >= LMoxDelay && r.state <= MoxCheck)
        m_collected = true;
    if (!fresh || !m_collected || !m_autoAttenuate || !calibrating())
        return std::nullopt;
    if (!r.feedbackFresh || Hl2PsGain::inWindow(r.feedbackLevel))
        return std::nullopt;
    if (m_mode == Mode::Single && m_singleGainMoves >= kSingleGainMoves)
        return std::nullopt;
    const int next = Hl2PsGain::clampAtt(m_att + Hl2PsGain::stepDb(r.feedbackLevel));
    if (next == m_att)
        return std::nullopt;   // at the end of the range: nothing to gain
    m_att = next;
    return next;
}

Hl2PsController::Step Hl2PsController::step(const Report& r)
{
    Step out;
    if (m_pending) {
        out.flags = *m_pending;
        m_pending.reset();
        m_lastAttempts = r.attempts;
        return out;
    }

    if (const auto att = gainMove(r)) {
        // The calibration under way mixes samples from both gains, and a
        // correction made at the old gain is wrong at the new one. Start the
        // same mode over; calcc's MOX delay lets the new gain settle before
        // it collects again.
        out.attenuationDb = att;
        out.flags = flagsFor(m_mode);
        m_sawActive = false;
        m_collected = false;
        if (m_mode == Mode::Single)
            ++m_singleGainMoves;
        return out;
    }

    switch (m_mode) {
    case Mode::Single:
        if (r.state != LReset)
            m_sawActive = true;
        if (r.state == LStayOn && r.correcting) {
            m_mode = Mode::Hold;
        } else if (m_sawActive && r.state == LReset && !r.correcting) {
            // calcc gave up: three bad calculations, or a collection that
            // could not fill. It sits in LRESET with nothing armed.
            if (m_singleStarts < kSingleAttempts) {
                ++m_singleStarts;
                out.flags = flagsFor(Mode::Single);
                m_sawActive = false;
                m_collected = false;
            } else {
                m_mode = Mode::Off;
            }
        }
        break;
    case Mode::Restoring:
        if (r.state == LStayOn && r.correcting)
            m_mode = Mode::Hold;
        break;
    case Mode::Off:
    case Mode::Auto:
    case Mode::Hold:
        break;
    }
    return out;
}

// ---- Hl2PureSignal -----------------------------------------------------------

Hl2PureSignal::Hl2PureSignal(Hl2TxDsp* tx, QObject* parent)
    : QObject(parent), m_tx(tx)
{
    qRegisterMetaType<Hl2PureSignalStatus>("AetherSDR::hl2::Hl2PureSignalStatus");
    m_txBuf.assign(2 * kFeedChunk, 0.0);
    m_rxBuf.assign(2 * kFeedChunk, 0.0);
}

Hl2PureSignal::~Hl2PureSignal() = default;

void Hl2PureSignal::startController()
{
    m_ctl.reset();
    m_lastInfo = {};
    if (m_wantAuto)
        m_ctl.selectAuto();
}

WdspChannel* Hl2PureSignal::channel()
{
    WdspChannel* ch = m_tx ? m_tx->pureSignalChannel() : nullptr;
    if (ch && m_tx->modulatorGeneration() != m_seededGeneration) {
        // A rebuilt TXA channel starts with calcc's defaults and no correction.
        seedChannel(ch);
        m_seededGeneration = m_tx->modulatorGeneration();
        if (m_enabled)
            startController();
    }
    return ch;
}

void Hl2PureSignal::seedChannel(WdspChannel* ch)
{
    ch->pureSignalConfigure(kHwPeak, m_feedbackRateHz, kMoxDelaySec, kLoopDelaySec,
                            kTxDelaySec);
    ch->pureSignalRunCal(true);
    ch->pureSignalMox(m_keyed && m_enabled);
    qCInfo(lcHl2Ps).nospace() << "HL2 PureSignal: TXA channel " << ch->channelIdForTest()
                              << " seeded, feedback " << m_feedbackRateHz << " Hz, peak "
                              << kHwPeak;
}

void Hl2PureSignal::setEnabled(bool on)
{
    if (on && !m_available)
        on = false;
    if (on == m_enabled) {
        publish();
        return;
    }
    m_enabled = on;
    m_fill = 0;
    if (on) {
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setInterval(kTickMs);
            connect(m_timer, &QTimer::timeout, this, &Hl2PureSignal::onTick);
        }
        if (WdspChannel* ch = channel())
            seedChannel(ch);
        startController();
        applyAttenuation(m_ctl.attenuationDb());
        m_timer->start();
        qCInfo(lcHl2Ps) << "HL2 PureSignal: on";
    } else {
        // Correction off on the way out. calcc only moves inside pscc(), so
        // the reset needs a block to run on or the correction stays applied.
        if (WdspChannel* ch = channel()) {
            ch->pureSignalControl(true, false, false, false);
            ch->pureSignalMox(false);
            pumpIdle(ch);
        }
        m_ctl.reset();
        m_lastInfo = {};
        if (m_timer)
            m_timer->stop();
        qCInfo(lcHl2Ps) << "HL2 PureSignal: off";
    }
    publish();
}

void Hl2PureSignal::setAvailability(bool available, const QString& reason)
{
    m_available = available;
    m_unavailableReason = available ? QString() : reason;
    if (!available && m_enabled)
        setEnabled(false);
    else
        publish();
}

void Hl2PureSignal::setKeyed(bool keyed)
{
    if (keyed == m_keyed)
        return;
    m_keyed = keyed;
    m_fill = 0;   // a half chunk from the last over must not lead the next
    if (m_enabled) {
        if (WdspChannel* ch = channel())
            ch->pureSignalMox(keyed);
    }
}

void Hl2PureSignal::setFeedbackRateHz(int hz)
{
    if (hz <= 0 || hz == m_feedbackRateHz)
        return;
    m_feedbackRateHz = hz;
    m_fill = 0;
    if (WdspChannel* ch = m_tx ? m_tx->pureSignalChannel() : nullptr)
        ch->pureSignalConfigure(kHwPeak, m_feedbackRateHz, kMoxDelaySec, kLoopDelaySec,
                                kTxDelaySec);
}

void Hl2PureSignal::setAutoCalibrate(bool on)
{
    m_wantAuto = on;
    if (m_enabled) {
        if (on)
            m_ctl.selectAuto();
        else if (m_ctl.mode() == Hl2PsController::Mode::Auto)
            m_ctl.selectOff();   // a held correction stays when Auto was not running
    }
    publish();
}

void Hl2PureSignal::calibrateOnce()
{
    m_wantAuto = false;
    if (m_enabled)
        m_ctl.selectSingle();
    publish();
}

void Hl2PureSignal::resetCorrection()
{
    m_wantAuto = false;
    if (m_enabled)
        m_ctl.selectOff();
    publish();
}

void Hl2PureSignal::setAutoAttenuate(bool on)
{
    m_ctl.setAutoAttenuate(on);
    publish();
}

void Hl2PureSignal::setAttenuationDb(int att)
{
    m_ctl.setAttenuationDb(att);
    applyAttenuation(m_ctl.attenuationDb());
    publish();
}

void Hl2PureSignal::saveCorrection(const QString& path)
{
    const QByteArray p = path.toLocal8Bit();
    if (p.size() >= 256) {   // calcc ignores it silently; the backend refuses first
        qCWarning(lcHl2Ps) << "HL2 PureSignal: path too long to save to" << path;
        return;
    }
    if (WdspChannel* ch = channel()) {
        ch->pureSignalSave(p.constData());
        qCInfo(lcHl2Ps) << "HL2 PureSignal: correction saved to" << path;
    }
}

void Hl2PureSignal::restoreCorrection(const QString& path)
{
    if (!m_enabled)
        return;
    const QByteArray p = path.toLocal8Bit();
    if (p.size() >= 256) {   // calcc would load nothing and we would turn it on
        qCWarning(lcHl2Ps) << "HL2 PureSignal: path too long to restore from" << path;
        return;
    }
    if (WdspChannel* ch = channel()) {
        ch->pureSignalRestore(p.constData());
        m_wantAuto = false;
        m_ctl.selectRestoring();
        qCInfo(lcHl2Ps) << "HL2 PureSignal: restoring correction from" << path;
        publish();
    }
}

void Hl2PureSignal::applyAttenuation(int att)
{
    if (m_txGainSink)
        m_txGainSink(Hl2PsGain::lnaGainDbForAtt(att));
}

void Hl2PureSignal::feed(const std::vector<std::complex<float>>& txRef,
                         const std::vector<std::complex<float>>& feedback)
{
    if (!m_enabled || !m_keyed)
        return;
    const std::size_t n = std::min(txRef.size(), feedback.size());
    for (std::size_t i = 0; i < n; ++i) {
        m_txBuf[2 * m_fill] = txRef[i].real();
        m_txBuf[2 * m_fill + 1] = txRef[i].imag();
        m_rxBuf[2 * m_fill] = feedback[i].real();
        m_rxBuf[2 * m_fill + 1] = feedback[i].imag();
        if (++m_fill == kFeedChunk)
            flushFeed();
    }
}

void Hl2PureSignal::flushFeed()
{
    if (WdspChannel* ch = channel())
        ch->pureSignalFeed(static_cast<int>(m_fill), m_txBuf.data(), m_rxBuf.data());
    m_fill = 0;
}

void Hl2PureSignal::pumpIdle(WdspChannel* ch)
{
    // Unkeyed, nothing feeds calcc and its state machine stands still. A short
    // block of silence lets a reset, a restore or a turn-on take effect
    // between overs. With mox clear calcc never gets past LWAIT on it, so
    // silence cannot become a calibration.
    static constexpr int kIdle = 64;
    double zt[2 * kIdle] = {};
    double zr[2 * kIdle] = {};
    ch->pureSignalFeed(kIdle, zt, zr);
}

void Hl2PureSignal::onTick()
{
    if (!m_enabled)
        return;
    WdspChannel* ch = channel();
    if (!ch) {
        publish();
        return;
    }
    if (!m_keyed)
        pumpIdle(ch);
    WdspChannel::PureSignalInfo info;
    ch->pureSignalInfo(&info);
    m_lastInfo = info;
    const auto out = m_ctl.step({info.state, info.correcting, info.feedbackLevel,
                                 info.feedbackFresh, info.attempts});
    if (out.attenuationDb) {
        qCInfo(lcHl2Ps).nospace() << "HL2 PureSignal: feedback " << info.feedbackLevel
                                  << ", attenuation -> " << *out.attenuationDb << " dB";
        applyAttenuation(*out.attenuationDb);
    }
    if (out.flags) {
        const auto& f = *out.flags;
        ch->pureSignalControl(f.reset, f.mancal, f.automode, f.turnon);
    }
    publish();
}

void Hl2PureSignal::publish()
{
    const WdspChannel::PureSignalInfo& info = m_lastInfo;
    Hl2PureSignalStatus s;
    s.available = m_available;
    s.unavailableReason = m_unavailableReason;
    s.enabled = m_enabled;
    s.autoCalibrate = m_wantAuto;
    s.autoAttenuate = m_ctl.autoAttenuate();
    s.calibrating = m_enabled && m_ctl.calibrating();
    s.singleCalibrating = m_enabled && m_ctl.mode() == Hl2PsController::Mode::Single;
    s.attenuationDb = m_ctl.attenuationDb();
    s.keyed = m_keyed;
    if (m_enabled) {
        s.correcting = info.correcting;
        s.feedbackLevel = info.feedbackLevel;
        s.calibrationCount = info.calibrationCount;
        s.engineState = info.state;
        s.maxTx = info.maxTx;
    }
    const bool same = m_havePublished && s.available == m_last.available
        && s.unavailableReason == m_last.unavailableReason && s.enabled == m_last.enabled
        && s.autoCalibrate == m_last.autoCalibrate && s.autoAttenuate == m_last.autoAttenuate
        && s.calibrating == m_last.calibrating
        && s.singleCalibrating == m_last.singleCalibrating && s.correcting == m_last.correcting
        && s.feedbackLevel == m_last.feedbackLevel
        && s.calibrationCount == m_last.calibrationCount && s.engineState == m_last.engineState
        && s.attenuationDb == m_last.attenuationDb && s.keyed == m_last.keyed
        && std::abs(s.maxTx - m_last.maxTx) < 1e-4;
    m_last = s;
    m_havePublished = true;
    if (!same)
        emit statusChanged(s);
}

QVariantMap pureSignalStatusFields(const Hl2PureSignalStatus& s)
{
    return QVariantMap{
        {QStringLiteral("available"), s.available},
        {QStringLiteral("unavailableReason"), s.unavailableReason},
        {QStringLiteral("enabled"), s.enabled},
        {QStringLiteral("autoCalibrate"), s.autoCalibrate},
        {QStringLiteral("autoAttenuate"), s.autoAttenuate},
        {QStringLiteral("calibrating"), s.calibrating},
        {QStringLiteral("singleCalibrating"), s.singleCalibrating},
        {QStringLiteral("correcting"), s.correcting},
        {QStringLiteral("feedbackLevel"), s.feedbackLevel},
        {QStringLiteral("calibrationCount"), s.calibrationCount},
        {QStringLiteral("engineState"), s.engineState},
        {QStringLiteral("attenuationDb"), s.attenuationDb},
        {QStringLiteral("attMin"), Hl2PsGain::kAttMin},
        {QStringLiteral("attMax"), Hl2PsGain::kAttMax},
        {QStringLiteral("feedbackWindowLow"), Hl2PsGain::kWindowLow},
        {QStringLiteral("feedbackWindowHigh"), Hl2PsGain::kWindowHigh},
        {QStringLiteral("maxTx"), s.maxTx},
        {QStringLiteral("keyed"), s.keyed},
    };
}

}  // namespace AetherSDR::hl2
