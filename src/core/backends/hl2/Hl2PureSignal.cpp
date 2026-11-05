// Ported from Thetis' PSForm.cs (MI0BOT's HL2 fork). Attribution and
// license: see Hl2PureSignal.h and THIRD_PARTY_LICENSES entry 36.

#include "core/backends/hl2/Hl2PureSignal.h"

#include "core/backends/hl2/Hl2TxDsp.h"

#include <QLoggingCategory>
#include <QTimer>

#include <cmath>

static Q_LOGGING_CATEGORY(lcHl2Ps, "aether.hl2.puresignal")

namespace AetherSDR::hl2 {

// ---- Hl2PureSignalLogic ------------------------------------------------------

void Hl2PureSignalLogic::setAutoCalibrate(bool on)
{
    // PSForm chkPSAutoCalibrate: on arms auto, off asks for OFF.
    if (on) {
        m_autoOn = true;
    } else {
        m_autoOn = false;
        m_off = true;
    }
}

void Hl2PureSignalLogic::resetToOff()
{
    m_cmd = Cmd::Off;
    m_aa = Aa::Monitor;
    m_autoOn = false;
    m_singleCalOn = false;
    m_off = false;
    m_restoreOn = false;
    m_performingSingleCal = false;
    m_singleCalRetries = 0;
    m_deltaDb = 0;
    m_saveAuto = false;
    m_saveSingle = false;
    m_lastCalibrationCount = 0;
}

int Hl2PureSignalLogic::attenuationStepDb(int fb) noexcept
{
    double ddB = 10.0;   // feedback beyond 256: MI0BOT's HL2 value
    if (fb <= 256) {
        ddB = 20.0 * std::log10(static_cast<double>(fb) * 1000.0 / kFeedbackIdeal100);
        if (std::isnan(ddB)) ddB = 10.0;
        if (ddB < -100.0) ddB = -10.0;   // fb == 0: log of zero
        if (ddB > 100.0) ddB = 10.0;
    }
    return static_cast<int>(std::lround(ddB));   // half away from zero
}

Hl2PureSignalLogic::Output Hl2PureSignalLogic::tick(const Info& info)
{
    Output out;
    runCommand(info, out);
    runAutoAttenuate(info, out);
    m_lastCalibrationCount = info.calibrationCount;
    return out;
}

void Hl2PureSignalLogic::runCommand(const Info& info, Output& out)
{
    const bool applied = info.correcting;
    switch (m_cmd) {
    case Cmd::Off:
        out.controls.push_back({true, false, false, false});
        if (m_restoreOn)
            m_cmd = Cmd::Restore;
        else if (m_autoOn)
            m_cmd = Cmd::TurnOnAuto;
        else if (m_singleCalOn)
            m_cmd = Cmd::TurnOnSingle;
        m_off = false;
        break;
    case Cmd::TurnOnAuto:
        out.controls.push_back({true, false, true, false});
        m_cmd = Cmd::Auto;
        break;
    case Cmd::Auto:
        if (m_off)
            m_cmd = Cmd::TurnOff;
        else if (m_restoreOn)
            m_cmd = Cmd::Restore;
        else if (m_singleCalOn)
            m_cmd = Cmd::TurnOnSingle;
        break;
    case Cmd::TurnOnSingle:
        m_autoOn = false;
        m_performingSingleCal = true;
        out.controls.push_back({true, true, false, false});
        m_cmd = Cmd::Single;
        break;
    case Cmd::Single:
        m_singleCalOn = false;
        if (m_off)
            m_cmd = Cmd::TurnOff;
        else if (m_restoreOn)
            m_cmd = Cmd::Restore;
        else if (m_autoOn)
            m_cmd = Cmd::TurnOnAuto;
        else if (applied)
            m_cmd = Cmd::StayOn;
        break;
    case Cmd::StayOn:
        if (m_off)
            m_cmd = Cmd::TurnOff;
        else if (m_restoreOn)
            m_cmd = Cmd::Restore;
        else if (m_autoOn)
            m_cmd = Cmd::TurnOnAuto;
        else if (m_singleCalOn)
            m_cmd = Cmd::TurnOnSingle;
        else if (m_performingSingleCal) {
            // A single calibration that auto-attenuate had to interrupt: try
            // again while the feedback is outside the window, five times.
            m_performingSingleCal = false;
            if (!feedbackInRange(info.feedbackLevel) && m_singleCalRetries < kSingleCalRetries) {
                ++m_singleCalRetries;
                m_singleCalOn = true;
            } else {
                m_singleCalRetries = 0;
            }
        }
        break;
    case Cmd::TurnOff:
        out.controls.push_back({true, false, false, false});
        m_off = false;
        if (m_restoreOn)
            m_cmd = Cmd::Restore;
        else if (m_autoOn)
            m_cmd = Cmd::TurnOnAuto;
        else if (m_singleCalOn)
            m_cmd = Cmd::TurnOnSingle;
        else if (!applied && info.state == LReset)
            m_cmd = Cmd::Off;
        break;
    case Cmd::Restore:
        m_autoOn = false;
        out.controls.push_back({false, false, false, true});
        m_restoreOn = false;
        if (info.state == LStayOn)
            m_cmd = Cmd::StayOn;
        break;
    }
}

void Hl2PureSignalLogic::runAutoAttenuate(const Info& info, Output& out)
{
    switch (m_aa) {
    case Aa::Monitor: {
        const bool attemptsChanged = info.calibrationCount != m_lastCalibrationCount;
        if (m_autoAttenuate && attemptsChanged
            && needToRecalibrate(info.feedbackLevel, m_att)) {
            m_aa = Aa::SetNewValues;
            m_deltaDb = attenuationStepDb(info.feedbackLevel);
            m_saveAuto = m_cmd == Cmd::Auto;
            m_saveSingle = m_cmd == Cmd::Single;
            out.controls.push_back({true, false, false, false});
        }
        break;
    }
    case Aa::SetNewValues: {
        m_aa = Aa::RestoreOperation;
        const int next = clampAtt(m_att + m_deltaDb);
        if (next != m_att) {
            m_att = next;
            out.attenuationDb = m_att;
        }
        break;
    }
    case Aa::RestoreOperation:
        m_aa = Aa::Monitor;
        out.controls.push_back({false, m_saveSingle, m_saveAuto, false});
        break;
    }
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

WdspChannel* Hl2PureSignal::channel()
{
    WdspChannel* ch = m_tx ? m_tx->pureSignalChannel() : nullptr;
    if (ch && m_tx->modulatorGeneration() != m_seededGeneration) {
        // A rebuilt TXA channel starts with calcc's defaults and no correction.
        seedChannel(ch);
        m_seededGeneration = m_tx->modulatorGeneration();
        m_logic.resetToOff();
        m_lastInfo = {};
        if (m_enabled && m_wantAuto)
            m_logic.setAutoCalibrate(true);
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
    m_lastInfo = {};
    if (on) {
        m_logic.resetToOff();
        if (m_wantAuto)
            m_logic.setAutoCalibrate(true);
        if (!m_timer) {
            m_timer = new QTimer(this);
            m_timer->setInterval(kTickMs);
            connect(m_timer, &QTimer::timeout, this, &Hl2PureSignal::onTick);
        }
        if (WdspChannel* ch = channel())
            seedChannel(ch);
        applyAttenuation(m_logic.attenuationDb());
        m_timer->start();
        qCInfo(lcHl2Ps) << "HL2 PureSignal: on";
    } else {
        // Correction off on the way out. calcc only advances inside pscc(), so
        // the reset needs a block to run on or the correction stays applied.
        if (WdspChannel* ch = channel()) {
            ch->pureSignalControl(true, false, false, false);
            ch->pureSignalMox(false);
            pumpIdle(ch);
        }
        m_logic.resetToOff();
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
    m_logic.setAutoCalibrate(on);
    publish();
}

void Hl2PureSignal::calibrateOnce()
{
    m_logic.requestSingleCalibration();
}

void Hl2PureSignal::resetCorrection()
{
    m_wantAuto = false;
    m_logic.requestOff();
    publish();
}

void Hl2PureSignal::setAutoAttenuate(bool on)
{
    m_logic.setAutoAttenuate(on);
    publish();
}

void Hl2PureSignal::setAttenuationDb(int att)
{
    m_logic.setAttenuationDb(att);
    applyAttenuation(m_logic.attenuationDb());
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
        m_logic.requestRestore();
        qCInfo(lcHl2Ps) << "HL2 PureSignal: restoring correction from" << path;
    }
}

void Hl2PureSignal::applyAttenuation(int att)
{
    if (m_txGainSink)
        m_txGainSink(Hl2PureSignalLogic::lnaGainDbForAtt(att));
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
    // Unkeyed, nothing feeds calcc, and its control machine only moves inside
    // pscc(). A short block of silence lets OFF, restore and turn-on take
    // effect between overs, as they do in Thetis where the PS receivers keep
    // streaming. With mox clear calcc never leaves LRESET/LWAIT/LSTAYON/
    // LTURNON on it, so silence cannot become a calibration.
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
    const auto out = m_logic.tick({info.feedbackLevel, info.calibrationCount, info.correcting,
                                   info.state});
    for (const auto& c : out.controls)
        ch->pureSignalControl(c.reset, c.mancal, c.automode, c.turnon);
    if (out.attenuationDb) {
        qCInfo(lcHl2Ps).nospace() << "HL2 PureSignal: feedback " << info.feedbackLevel
                                  << ", attenuation -> " << *out.attenuationDb << " dB";
        applyAttenuation(*out.attenuationDb);
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
    s.autoAttenuate = m_logic.autoAttenuate();
    s.calibrating = m_enabled && m_logic.calibrating();
    s.attenuationDb = m_logic.attenuationDb();
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
        && s.calibrating == m_last.calibrating && s.correcting == m_last.correcting
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
        {QStringLiteral("correcting"), s.correcting},
        {QStringLiteral("feedbackLevel"), s.feedbackLevel},
        {QStringLiteral("calibrationCount"), s.calibrationCount},
        {QStringLiteral("engineState"), s.engineState},
        {QStringLiteral("attenuationDb"), s.attenuationDb},
        {QStringLiteral("attMin"), Hl2PureSignalLogic::kAttMin},
        {QStringLiteral("attMax"), Hl2PureSignalLogic::kAttMax},
        {QStringLiteral("feedbackWindowLow"), 128},
        {QStringLiteral("feedbackWindowHigh"), 181},
        {QStringLiteral("maxTx"), s.maxTx},
        {QStringLiteral("keyed"), s.keyed},
    };
}

}  // namespace AetherSDR::hl2
