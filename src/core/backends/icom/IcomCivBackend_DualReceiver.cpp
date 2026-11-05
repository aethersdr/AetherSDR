#include "core/backends/icom/IcomCivBackend.h"
#include "core/Resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <QTimer>
#include <QEventLoop>
#include <QPointer>
#include <string>
#include <vector>

namespace AetherSDR::icom {

bool IcomCivBackend::hasDualReceivers() const
{
    return m_model && profileFor(*m_model).dualReceiver.has_value();
}

std::optional<ModulationProfile> IcomCivBackend::currentModulationProfile() const
{
    auto profile = modulationProfileFor(*m_model);
    if (profile && hasDualReceivers()) {
        const int data = m_receivers[m_dualTxReceiver].data;
        if (data >= 1 && data <= 3) {
            profile->dataInputItem = profileFor(*m_model).dualReceiver->dataInputItems[data - 1];
        }
    }
    return profile;
}

std::vector<std::uint8_t> IcomCivBackend::receiverCommand(
    const std::vector<std::uint8_t>& frame) const
{
    if (!hasDualReceivers()) { return frame; }
    const auto parsed = parseFrame(frame);
    if (!parsed) { return frame; }
    const std::uint8_t address = frame[2];
    if (parsed->cmd == cmd::kReadFreq) {
        return buildFrameSub(address, 0x25, 0);
    }
    if (parsed->cmd == cmd::kReadMode) {
        return buildFrameSub(address, 0x26, 0);
    }
    // Shared subset of the model guides' 29 marks; TX controls are global.
    // In particular 14 0A/0B/0C/0E and 16 44/45/46/47 must stay unwrapped.
    const auto listed = [](std::uint8_t value, std::initializer_list<std::uint8_t> allowed) {
        return std::ranges::find(allowed, value) != allowed.end();
    };
    const bool addressed = parsed->hasSub && (
        (parsed->cmd == cmd::kLevel && listed(parsed->sub, {0x01,0x02,0x03,0x05,0x06,0x07,0x08,0x0d,0x12,0x13}))
        || (parsed->cmd == cmd::kMeter && listed(parsed->sub, {0x01,0x02,0x05,0x07}))
        || (parsed->cmd == cmd::kFunction && listed(parsed->sub, {0x02,0x12,0x22,0x40,0x41,0x48,0x57}))
        || (parsed->cmd == cmd::kSetting && listed(parsed->sub, {0x03,0x04})));
    if (addressed) {
        return buildFrameSub(address, 0x29, 0,
            std::span<const std::uint8_t>(frame).subspan(4, frame.size() - 5));
    }
    return frame;
}

void IcomCivBackend::initialiseDualReceivers()
{
    if (m_dualInitialised || !m_session) { return; }
    m_dualInitialised = true;
    for (int receiver = 0; receiver < 2; ++receiver) {
        ReceiverState& state = m_receivers[receiver];
        ScopeGeometry geometry;
        geometry.points = m_model->scopePoints;
        geometry.maxAmplitude = m_model->scopeMaxAmplitude;
        geometry.divisionsUsb = m_model->scopeDivisionsUsb;
        state.scope = ScopeDecoder(geometry);
        state.calibration.spanDb = profileFor(*m_model).dualReceiver->scopeSpanDb;
        state.audio = std::make_unique<Resampler>(m_audioRateHz, kEngineAudioRateHz, 4096);
        state.visible = receiver == 0;
        state.monitorPan = receiver == 0 ? 50 : 100;
    }
    publishDualPan(0);
    sendUserCommand(cmdScopeMode(m_session->civAddress(), 3, 0));
    m_receivers[0].viewInitialised = true;
    if (m_session->rxChannels() != 2) {
        emit configurationWarning(QStringLiteral(
            "Dual-receiver radio connected with mono LAN audio. SUB audio is unavailable; "
            "reconnect with icom.rxChannels=2 for a non-default CI-V address."));
    }
    pollDualReceivers();
}

void IcomCivBackend::pollDualReceivers()
{
    if (!m_session || !m_connected || m_dualRestoring) { return; }
    const std::uint8_t address = m_session->civAddress();
    const auto read = [this](const std::vector<std::uint8_t>& frame) {
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::Control);
    };
    for (std::uint8_t receiver : {0, 1}) {
        read(buildFrameSub(address, 0x25, receiver));
        read(buildFrameSub(address, 0x26, receiver));
        read(cmdReadScopeEdgeNumber(address, receiver));
        read(cmdReadScopeMode(address, receiver));
        queueRead(buildFrameSub(address, cmd::kScope, scope::kReference,
                  std::array<std::uint8_t,1>{receiver}),
                  "scope.reference." + std::to_string(receiver),
                  IcomCivScheduler::Priority::Maintenance, 0, {receiver});
    }
    read(buildFrame(address, cmd::kDuplex));
    read(buildFrameSub(address, cmd::kVfoControl, 0xc2));
    read(cmdReadTransmitFrequency(address));
    read(buildFrameSub(address, cmd::kControl, control::kPtt));
    const auto mod = currentModulationProfile();
    for (int item : {profileFor(*m_model).dualReceiver->lanOutputItem,
                     mod->dataOffInputItem, mod->dataInputItem, mod->networkLevelItem}) {
        read(cmdReadSetting(address, item));
    }
    for (int item : profileFor(*m_model).dualReceiver->dataInputItems) {
        const auto frame = cmdReadSetting(address, item);
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::Maintenance);
    }
    pumpCiv(nowMs());
}

void IcomCivBackend::publishDualReceiver(int receiver)
{
    const ReceiverState& state = m_receivers[receiver];
    if (!state.visible) { return; }
    SliceDelta delta;
    if (state.frequencyHz > 0) { delta.frequency = state.frequencyHz / 1e6; }
    if (state.modeKnown) {
        const std::string mode = modeToNeutral(state.mode, state.data != 0);
        delta.mode = QString::fromStdString(mode);
        const auto [low, high] = passbandForModeAndFilter(mode, state.filter);
        delta.filterLow = low;
        delta.filterHigh = high;
    }
    QStringList modes;
    for (const std::string_view mode : modeListFor(*m_model)) {
        modes.append(QString::fromUtf8(mode.data(), static_cast<int>(mode.size())));
    }
    delta.modeList = modes;
    emit sliceChanged(receiver, delta);
}

void IcomCivBackend::syncDualTransmit()
{
    const ReceiverState& tx = m_receivers[m_dualTxReceiver];
    const bool changed = m_mode != tx.mode || m_dataMode != (tx.data != 0);
    m_frequencyHz = tx.frequencyHz;
    m_mode = tx.mode;
    m_dataMode = tx.data != 0;
    m_filter = tx.filter;
    if (changed) {
        m_dataModInput = -1;
    }
    m_dataOffModInput = m_dualModInputs[0];
    m_dataModInput = tx.data > 0 ? m_dualModInputs[tx.data] : -1;
    for (int receiver = 0; receiver < 2; ++receiver) {
        if (!m_receivers[receiver].visible) { continue; }
        SliceDelta delta;
        delta.txSlice = receiver == m_dualTxReceiver;
        emit sliceChanged(receiver, delta);
    }
    checkModInput();
}

void IcomCivBackend::writeDualMode(int receiver, CivMode mode, std::uint8_t data, int filter)
{
    if (!m_connected || !m_session || receiver < 0 || receiver > 1 || data > 3
        || filter < 1 || filter > 3 || !m_receivers[receiver].modeKnown) { return; }
    // All three fields are required: omitted DATA or filter changes the
    // radio's defaults. A write is intent; the 26 readback publishes truth.
    const std::array<std::uint8_t, 3> value{
        static_cast<std::uint8_t>(mode), data, static_cast<std::uint8_t>(filter)};
    sendUserCommand(buildFrameSub(m_session->civAddress(), 0x26,
                                  static_cast<std::uint8_t>(receiver), value));
}

void IcomCivBackend::setActiveSlice(int receiver)
{
    if (!hasDualReceivers() || receiver < 0 || receiver > 1 || !m_receivers[receiver].visible) { return; }
    m_dualFocus = receiver;
    for (int index = 0; index < 2; ++index) {
        if (!m_receivers[index].visible) { continue; }
        SliceDelta delta;
        delta.active = index == receiver;
        emit sliceChanged(index, delta);
    }
    // Local editing focus does not write 07 D2, split, or the TX designation.
}

bool IcomCivBackend::handleDualReceiverFrame(const CivFrame& frame)
{
    const bool waveform = frame.cmd == cmd::kScope && frame.hasSub && frame.sub == 0;
    if (waveform) {
        if (frame.data.empty() || frame.data[0] > 1) { return true; }
        if (frame.data.size() >= 15 && frame.data[1] == 1 && frame.data[2] == 1
            && ((frame.data[14] == 0 && frame.data.size() != 15 + static_cast<std::size_t>(m_model->scopePoints))
                || (frame.data[14] != 0 && frame.data.size() != 15))) { return true; }
        const int receiver = frame.data[0];
        ReceiverState& state = m_receivers[receiver];
        const auto sweep = state.scope.feed(frame);
        if (!sweep || sweep->bandwidthHz() <= 0) { return true; }
        ++state.scopeFrames;
        state.lastScopeAtMs = nowMs();
        if (state.windowTarget) {
            const auto target = *state.windowTarget;
            const bool matched = sweep->startHz == static_cast<std::int64_t>(target.lowerHz)
                && sweep->endHz == static_cast<std::int64_t>(target.upperHz);
            if (!matched && nowMs() < state.windowHoldUntilMs) { return true; }
            if (matched || nowMs() >= state.windowHoldUntilMs) { state.windowTarget.reset(); }
        }
        state.centreHz = sweep->centreHz();
        state.spanHz = sweep->bandwidthHz() / 2;
        const bool modeChanged = state.scopeFrames == 1 || state.scopeMode != sweep->mode;
        state.scopeMode = sweep->mode;
        if (modeChanged && state.visible) {
            emit panScopeModeChanged(QString::number(receiver), sweep->mode == ScopeMode::Centre ? 0
                : sweep->mode == ScopeMode::ScrollF ? 1 : sweep->mode == ScopeMode::Fixed ? 2 : -1);
        }
        settleDualPanLifecycle();
        if (!state.visible) { return true; }
        const std::vector<float> bins = toDbm(*sweep, state.scope.geometry(), state.calibration);
        emit panCenterBandwidthChanged(QString::number(receiver), sweep->centreHz() / 1e6,
                                       sweep->bandwidthHz() / 1e6);
        // CI-V sends ordered amplitudes, low to high. No IQ conjugation or
        // FFT half-rotation belongs here. RadioModel uses this same row for WF.
        emit spectrumFrameReady(receiver, QByteArray(reinterpret_cast<const char*>(bins.data()),
            static_cast<qsizetype>(bins.size() * sizeof(float))));
        return true;
    }
    if (frame.cmd == cmd::kScope && frame.hasSub && frame.sub == scope::kFixedEdge) {
        return handleDualScopePreset(frame);
    }
    const bool frequency = frame.cmd == 0x25;
    const bool mode = frame.cmd == 0x26;
    const bool split = frame.cmd == cmd::kDuplex;
    const bool txFrequency = frame.cmd == cmd::kControl && frame.hasSub && frame.sub == control::kReadTxFreq;
    const bool scopeState = frame.cmd == cmd::kScope;
    const bool dualWatch = frame.cmd == cmd::kVfoControl && frame.hasSub && frame.sub == 0xc2;
    if (frame.cmd == 0x29) {
        if (!frame.hasSub || frame.sub > 1 || frame.data.size() < 3) { return true; }
        // Only MAIN's generic register replies have legacy consumers. SUB
        // controls beyond frequency/mode/filter remain deferred.
        if (frame.sub != 0) {
            const auto observation = m_civScheduler.observe(frame, nowMs());
            ++m_framesObserved;
            m_lastInboundCivAtMs = nowMs();
            if (observation == IcomCivScheduler::Observation::Stale) { return true; }
            if (frame.data[0] == cmd::kLevel && frame.data[1] == level::kRf) {
                if (const auto raw = decodeLevel(std::span<const std::uint8_t>(frame.data).subspan(2))) {
                    emit panRfGainChanged(QStringLiteral("1"), levelRawToPercent(*raw));
                }
            }
            pumpCiv(nowMs());
            return true;
        }
        return false;
    }
    if (!frequency && !mode && !split && !scopeState && !dualWatch && !txFrequency) { return false; }
    if (txFrequency && (frame.data.size() != kFreqBytes || !decodeFreq(frame.data))) { return true; }
    if ((frequency || mode) && (!frame.hasSub || frame.sub > 1)) { return true; }
    if (frequency && (frame.data.size() != kFreqBytes || !decodeFreq(frame.data))) { return true; }
    if (mode && (frame.data.size() != 3 || frame.data[1] > 3
        || frame.data[2] < 1 || frame.data[2] > 3
        || modeToNeutral(static_cast<CivMode>(frame.data[0]), frame.data[1] != 0).empty())) { return true; }
    if (split && (frame.data.size() != 1 || frame.data[0] > 1)) { return true; }
    if (dualWatch && (frame.data.size() != 1 || frame.data[0] > 1)) { return true; }
    if (scopeState && (!frame.hasSub || frame.sub == 0x10 || frame.sub == 0x11 || frame.sub == 0x13)) { return false; }
    if (scopeState && (frame.data.size() < 2 || frame.data[0] > 1)) { return true; }
    const qint64 now = nowMs();
    std::vector<std::uint8_t> body{frame.cmd};
    if (frame.hasSub) { body.push_back(frame.sub); }
    body.insert(body.end(), frame.data.begin(), frame.data.end());
    traceCiv(false, body, false);
    const auto observation = m_civScheduler.observe(frame, now);
    ++m_framesObserved;
    m_lastInboundCivAtMs = now;
    if (observation == IcomCivScheduler::Observation::Stale) { return true; }
    if (frequency) {
        ReceiverState& state = m_receivers[frame.sub];
        const auto reported = *decodeFreq(frame.data);
        const bool expectedTune = state.commandedFrequencyHz && *state.commandedFrequencyHz == reported;
        if (state.windowTarget && reported != state.frequencyHz && !expectedTune) {
            state.windowTarget.reset();
            state.pendingCentreHz.reset();
        }
        state.frequencyHz = reported;
        state.commandedFrequencyHz.reset();
        flushDualScopeWindow(frame.sub);
        if (frame.sub == m_dualTxReceiver) {
            confirmState(QStringLiteral("frequency"), QVariant::fromValue<qulonglong>(state.frequencyHz));
        }
        publishDualReceiver(frame.sub);
    } else if (mode) {
        ReceiverState& state = m_receivers[frame.sub];
        if (frame.sub == m_dualTxReceiver && state.data != frame.data[1]) { m_dataModInput = -1; }
        state.mode = static_cast<CivMode>(frame.data[0]);
        state.data = frame.data[1];
        state.filter = frame.data[2];
        state.modeKnown = true;
        if (frame.sub == m_dualTxReceiver) {
            confirmState(QStringLiteral("mode"), QStringLiteral("%1/%2/%3")
                .arg(frame.data[0]).arg(state.data).arg(state.filter));
        }
        publishDualReceiver(frame.sub);
    } else if (scopeState && frame.sub == scope::kReference && frame.data.size() == 4) {
        const auto magnitude = std::span<const std::uint8_t>(frame.data).subspan(1,2);
        if ((magnitude[0] >> 4) <= 9 && (magnitude[0] & 15) <= 9
            && (magnitude[1] >> 4) <= 9 && (magnitude[1] & 15) == 0 && frame.data[3] <= 1) {
            const double value = (decodeBcdByte(magnitude[0]) + decodeBcdByte(magnitude[1]) / 100.0)
                * (frame.data[3] == 0 ? 1 : -1);
            const auto& profile = *profileFor(*m_model).dualReceiver;
            if (value >= profile.referenceMinDb && value <= profile.referenceMaxDb) {
                m_receivers[frame.data[0]].calibration.referenceDb = value;
                publishScopeDbmRange();
            }
        }
    } else if (scopeState && frame.sub == scope::kEdgeNumber && frame.data.size() == 2
               && frame.data[1] >= 1 && frame.data[1] <= 4) {
        m_receivers[frame.data[0]].edgeSlot = frame.data[1];
        if (m_dualRestoring && m_dualRestoreSlots[frame.data[0]] == frame.data[1]
            && observation == IcomCivScheduler::Observation::Accepted) { m_dualRestoreSlots[frame.data[0]] = 0; }
        for (int receiver = 0; receiver < 2; ++receiver) { flushDualScopeWindow(receiver); }
    } else if (scopeState && frame.sub == scope::kMode && frame.data.size() == 2 && frame.data[1] <= 3) {
        auto& state = m_receivers[frame.data[0]];
        state.scopeMode = static_cast<ScopeMode>(frame.data[1]);
        if (state.visible) {
            emit panScopeModeChanged(QString::number(frame.data[0]), frame.data[1] == 0 ? 0 : frame.data[1] == 3 ? 1 : frame.data[1] == 1 ? 2 : -1);
        }
    } else if (dualWatch) {
        if (m_dualWatch == true && frame.data[0] == 0 && !m_dualPanClosing) { m_dualWatchOwned = false; }
        m_dualWatch = frame.data[0] != 0;
    } else if (split) {
        const int receiver = frame.data[0];
        if (receiver != m_dualTxReceiver) { m_dataModInput = -1; }
        m_dualSplit = receiver != 0;
        m_dualTxReceiver = receiver;
        if (m_dualRequestedTxReceiver == receiver) { m_dualRequestedTxReceiver.reset(); }
    } else if (txFrequency) {
        m_dualTxFrequencyHz = decodeFreq(frame.data);
        TransmitDelta delta;
        delta.transmitFreq = *m_dualTxFrequencyHz / 1e6;
        emit transmitChanged(delta);
    }
    settleDualPanLifecycle();
    syncDualTransmit();
    pumpCiv(now);
    return true;
}

void IcomCivBackend::onDualAudio(const std::vector<float>& interleaved)
{
    if (!m_connected || m_dualRestoring || interleaved.empty() || interleaved.size() % 2 != 0
        || interleaved.size() > static_cast<std::size_t>(PcmFrame::kMaxFrames) * 2
        || m_lanOutput != 0 || !m_dualInitialised
        || !std::ranges::all_of(interleaved, [](float v) { return std::isfinite(v); })) { return; }
    const std::size_t frames = interleaved.size() / 2;
    std::array<QByteArray, 2> streams;
    for (int receiver = 0; receiver < 2; ++receiver) {
        if (!m_receivers[receiver].audio) { return; }
        std::vector<float> mono(frames);
        double sumSquares = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            mono[i] = interleaved[i * 2 + receiver];
            sumSquares += static_cast<double>(mono[i]) * mono[i];
        }
        m_receivers[receiver].audioFrames += frames;
        m_receivers[receiver].audioRms = std::sqrt(sumSquares / frames);
        streams[receiver] = m_receivers[receiver].audio->processMonoToStereo(mono.data(), static_cast<int>(frames));
    }
    if (streams[0].isEmpty() || streams[0].size() != streams[1].size()) { return; }
    for (int receiver = 0; receiver < 2; ++receiver) {
        if (m_receivers[receiver].visible) { publishLegacySliceAudio(receiver, streams[receiver]); }
    }
    QByteArray mixed(streams[0].size(), Qt::Uninitialized);
    const float* main = reinterpret_cast<const float*>(streams[0].constData());
    const float* sub = reinterpret_cast<const float*>(streams[1].constData());
    float* out = reinterpret_cast<float*>(mixed.data());
    std::array<std::array<double,2>,2> weights{};
    std::array<double,2> gains{};
    for (int receiver = 0; receiver < 2; ++receiver) {
        const ReceiverState& state = m_receivers[receiver];
        const double balance = state.monitorPan / 100.0;
        weights[receiver] = {std::min(1.0, 2.0 * (1.0 - balance)), std::min(1.0, 2.0 * balance)};
        gains[receiver] = state.monitorMute || !state.visible ? 0.0 : state.monitorGain / 100.0;
        if (!state.visible) { weights[receiver] = {0.0, 0.0}; }
    }
    for (qsizetype i = 0; i < mixed.size() / static_cast<qsizetype>(sizeof(float)); ++i) {
        const int channel = i % 2;
        const double divisor = std::max(1.0, weights[0][channel] + weights[1][channel]);
        out[i] = static_cast<float>((main[i] * gains[0] * weights[0][channel]
                                 + sub[i] * gains[1] * weights[1][channel]) / divisor);
    }
    publishLegacyAudio(mixed);
}


void IcomCivBackend::publishDualPan(int receiver)
{
    const auto& state = m_receivers[receiver];
    if (!state.visible) { return; }
    const QString id = QString::number(receiver);
    emit panCenterBandwidthChanged(id, state.centreHz / 1e6, state.spanHz * 2.0 / 1e6);
    emit panScopeModesChanged(id, {QStringLiteral("Center"), QStringLiteral("Scroll-F"), QStringLiteral("Fixed")});
    emit panScopeModeChanged(id, state.scopeMode == ScopeMode::Centre ? 0 : state.scopeMode == ScopeMode::ScrollF ? 1 : 2);
    emit panRangeChanged(id, state.calibration.floorDbm, state.calibration.floorDbm + state.calibration.spanDb);
    emit panBandwidthLimitsChanged(id, 0.005, 1.0);
    SliceDelta delta;
    delta.inUse = true;
    delta.panId = id;
    delta.active = receiver == m_dualFocus;
    delta.txSlice = receiver == m_dualTxReceiver;
    delta.audioGain = state.monitorGain;
    delta.audioPan = state.monitorPan;
    delta.audioMute = state.monitorMute;
    emit sliceChanged(receiver, delta);
    publishDualReceiver(receiver);
}

bool IcomCivBackend::createPanadapter()
{
    if (!hasDualReceivers() || !m_session || !m_connected || m_keyed || !m_dualInitialised
        || m_receivers[1].visible || m_dualPanOpening || m_dualPanClosing) { return false; }
    m_dualPanOpening = true;
    m_dualPanWatchRequested = false;
    const auto paneEpoch = ++m_dualPaneEpoch;
    if (!m_dualPanHasOpened) {
        sendUserCommand(cmdScopeMode(m_session->civAddress(), 3, 1));
        m_receivers[1].viewInitialised = true;
    }
    sendUserCommand(buildFrameSub(m_session->civAddress(), cmd::kScope, 0x13,
                                 std::array<std::uint8_t,1>{1}));
    const auto watchRead = buildFrameSub(m_session->civAddress(), cmd::kVfoControl, 0xc2);
    queueRead(watchRead, semanticKey(watchRead), IcomCivScheduler::Priority::Operator);
    settleDualPanLifecycle();
    const auto generation = m_sessionGeneration;
    QTimer::singleShot(10000, this, [this, generation, paneEpoch] {
        if (generation != m_sessionGeneration || paneEpoch != m_dualPaneEpoch || !m_dualPanOpening) { return; }
        m_dualPanOpening = false;
        if (m_dualWatchOwned) {
            sendUserCommand(buildFrameSub(m_session->civAddress(), cmd::kVfoControl, 0xc2,
                                         std::array<std::uint8_t,1>{0}));
            m_dualWatchOwned = false;
        }
        emit configurationWarning(QStringLiteral("SUB did not confirm Dualwatch and scope mode; the second pan was not opened."));
    });
    return true;
}

void IcomCivBackend::settleDualPanLifecycle()
{
    if (!m_session || !m_connected) { return; }
    if (m_dualPanOpening && m_dualWatch) {
        if (!*m_dualWatch) {
            if (!m_dualPanWatchRequested) {
                m_dualPanWatchRequested = true;
                m_dualWatchOwned = true;
                sendUserCommand(buildFrameSub(m_session->civAddress(), cmd::kVfoControl, 0xc2,
                                             std::array<std::uint8_t,1>{1}));
            }
        } else if (m_dualPanHasOpened || m_receivers[1].scopeMode == ScopeMode::ScrollF) {
            m_dualPanOpening = false;
            m_dualPanHasOpened = true;
            m_receivers[1].visible = true;
            m_receivers[0].monitorPan = m_dualMainMonitorPan;
            SliceDelta main;
            main.audioPan = m_receivers[0].monitorPan;
            emit sliceChanged(0, main);
            publishDualPan(1);
        }
    }
    if (m_dualPanClosing && m_dualWatch == false) {
        m_dualPanClosing = false;
        m_dualWatchOwned = false;
        m_receivers[1].visible = false;
        m_dualMainMonitorPan = m_receivers[0].monitorPan;
        m_receivers[0].monitorPan = 50;
        setActiveSlice(0);
        emit sliceRemoved(1);
        emit panRemoved(QStringLiteral("1"));
        SliceDelta main;
        main.audioPan = 50;
        emit sliceChanged(0, main);
    }
}

bool IcomCivBackend::removePanadapter(const QString& id)
{
    if (!hasDualReceivers() || id != QLatin1String("1") || !m_session || !m_connected || m_keyed
        || !m_receivers[1].visible || m_dualPanOpening || m_dualPanClosing) { return false; }
    const auto paneEpoch = ++m_dualPaneEpoch;
    auto& sub = m_receivers[1];
    sub.windowTarget.reset();
    sub.pendingCentreHz.reset();
    sub.windowFlushScheduled = false;
    ++sub.windowEpoch;
    if (m_dualWatchOwned && m_dualWatch == true) {
        m_dualPanClosing = true;
        sendUserCommand(buildFrameSub(m_session->civAddress(), cmd::kVfoControl, 0xc2,
                                     std::array<std::uint8_t,1>{0}));
        const auto generation = m_sessionGeneration;
        QTimer::singleShot(10000, this, [this, generation, paneEpoch] {
            if (generation != m_sessionGeneration || paneEpoch != m_dualPaneEpoch || !m_dualPanClosing) { return; }
            m_dualPanClosing = false;
            emit configurationWarning(QStringLiteral("Dualwatch did not confirm off; SUB remains open."));
        });
    } else {
        m_dualWatchOwned = false;
        m_receivers[1].visible = false;
        m_dualMainMonitorPan = m_receivers[0].monitorPan;
        m_receivers[0].monitorPan = 50;
        setActiveSlice(0);
        emit sliceRemoved(1);
        emit panRemoved(QStringLiteral("1"));
        SliceDelta main;
        main.audioPan = 50;
        emit sliceChanged(0, main);
    }
    return true;
}

bool IcomCivBackend::removeSlice(int receiver)
{
    return receiver == 1 && removePanadapter(QStringLiteral("1"));
}

void IcomCivBackend::requestDualScopeWindow(int receiver, double centreHz, double widthHz)
{
    if (!m_session || !m_connected || m_dualRestoring || m_keyed || receiver < 0 || receiver > 1
        || !std::isfinite(centreHz) || !std::isfinite(widthHz) || widthHz <= 0) { return; }
    auto& state = m_receivers[receiver];
    if (!state.visible || (receiver == 1 && m_dualPanClosing)
        || !state.frequencyHz || state.scopeMode == ScopeMode::Centre) { return; }
    const auto vfoHz = state.commandedFrequencyHz.value_or(state.frequencyHz);
    const auto range = scopeEdgeRangeFor(scopeEdgeRanges(), vfoHz);
    if (!range || range->number > 12) { return; }
    const double width = std::clamp(widthHz, 5000.0, 1e6);
    if (state.scopeMode == ScopeMode::ScrollF) {
        centreHz = scopeCentreHoldingVfo(centreHz, width, vfoHz,
            std::max(width * kScopeDragVfoMarginFraction, kScopeDragVfoMarginMinHz));
    }
    state.windowTarget = clampScopeEdgeWindow(*range, centreHz, width);
    state.windowRange = range->number;
    state.windowHoldUntilMs = nowMs() + 5000;
    if (!state.windowFlushScheduled) {
        state.windowFlushScheduled = true;
        const auto windowEpoch = ++state.windowEpoch;
        const int wait = static_cast<int>(std::max<qint64>(0,
            kScopeEdgeWriteIntervalMs - (nowMs() - state.lastWindowWriteMs)));
        const auto generation = m_sessionGeneration;
        QTimer::singleShot(wait, this, [this, receiver, generation, windowEpoch] {
            if (generation != m_sessionGeneration || windowEpoch != m_receivers[receiver].windowEpoch) { return; }
            m_receivers[receiver].windowFlushScheduled = false;
            flushDualScopeWindow(receiver);
        });
    }
}

void IcomCivBackend::flushDualScopeWindow(int receiver)
{
    auto& state = m_receivers[receiver];
    const auto& other = m_receivers[1 - receiver];
    if (!state.windowTarget || !m_session || !m_connected || m_keyed || !state.visible
        || m_dualRestoring || state.windowFlushScheduled || state.commandedFrequencyHz) { return; }
    const auto address = m_session->civAddress();
    if (!state.edgeSlot || !other.edgeSlot) {
        for (std::uint8_t index : {0, 1}) {
            const auto read = cmdReadScopeEdgeNumber(address, index);
            queueRead(read, semanticKey(read), IcomCivScheduler::Priority::Operator);
        }
        pumpCiv(nowMs());
        return;
    }
    if (!state.borrowedSlot) {
        for (int slot : {4, 3, 2, 1}) {
            if (slot != other.edgeSlot && slot != other.borrowedSlot) {
                state.borrowedSlot = slot;
                break;
            }
        }
    }
    // Presets are global even though slot selection is receiver-addressed.
    if (!state.borrowedSlot || state.borrowedSlot == other.edgeSlot) { return; }
    const ScopePresetKey key{state.windowRange, state.borrowedSlot};
    const auto target = *state.windowTarget;
    const auto write = cmdScopeFixedEdge(address, key.first, key.second, target.lowerHz, target.upperHz);
    const auto read = buildFrameSub(address, cmd::kScope, scope::kFixedEdge,
                                   std::array<std::uint8_t,2>{encodeBcdByte(key.first), encodeBcdByte(key.second)});
    if (!m_dualSavedPresets.contains(key)) {
        m_dualPresetReads.insert(key);
        queueRead(read, semanticKey(read), IcomCivScheduler::Priority::Operator);
        pumpCiv(nowMs());
        return;
    }
    if (const auto last = m_dualWrittenPresets.find(key); last != m_dualWrittenPresets.end()
        && last->second.lowerHz == target.lowerHz && last->second.upperHz == target.upperHz) { return; }
    if (!state.originalSlot) { state.originalSlot = state.edgeSlot; }
    m_dualWrittenPresets[key] = target;
    state.lastWindowWriteMs = nowMs();
    state.windowHoldUntilMs = nowMs() + 5000;
    sendUserCommand(write);
    if (state.edgeSlot != state.borrowedSlot) {
        sendUserCommand(cmdScopeEdgeNumber(address, state.borrowedSlot, static_cast<std::uint8_t>(receiver)));
    }
}

bool IcomCivBackend::handleDualScopePreset(const CivFrame& frame)
{
    if (frame.data.size() != 12) { return true; }
    if ((frame.data[0] >> 4) > 9 || (frame.data[0] & 15) > 9) { return true; }
    const int range = decodeBcdByte(frame.data[0]);
    const int slot = frame.data[1];
    const auto low = decodeFreq(std::span<const std::uint8_t>(frame.data).subspan(2, 5));
    const auto high = decodeFreq(std::span<const std::uint8_t>(frame.data).subspan(7, 5));
    if (range < 1 || range > 12 || slot < 1 || slot > 4 || !low || !high || *high < *low) { return true; }
    std::vector<std::uint8_t> body{cmd::kScope, scope::kFixedEdge};
    body.insert(body.end(), frame.data.begin(), frame.data.end());
    traceCiv(false, body, false);
    const auto observation = m_civScheduler.observe(frame, nowMs());
    if (observation == IcomCivScheduler::Observation::Stale) { return true; }
    const ScopePresetKey key{range, slot};
    m_dualObservedPresets[key] = ScopeEdgeWindow{*low, *high};
    if (m_dualRestoring) {
        if (const auto expected = m_dualRestoreExpected.find(key); expected != m_dualRestoreExpected.end()
            && observation == IcomCivScheduler::Observation::Accepted
            && expected->second.lowerHz == *low && expected->second.upperHz == *high) {
            m_dualRestoreExpected.erase(expected);
        }
        pumpCiv(nowMs());
        return true;
    }
    const auto written = m_dualWrittenPresets.find(key);
    const auto original = m_dualSavedPresets.find(key);
    if (!m_dualPresetReads.contains(key) && written != m_dualWrittenPresets.end()
        && original != m_dualSavedPresets.end()
        && (*low != written->second.lowerHz || *high != written->second.upperHz)
        && (*low != original->second.lowerHz || *high != original->second.upperHz)) {
        // A newer operator preset edit is no longer ours to restore.
        m_dualWrittenPresets.erase(key);
        m_dualSavedPresets.erase(key);
        for (auto& state : m_receivers) {
            if (state.windowRange == range && state.borrowedSlot == slot) { state.windowTarget.reset(); }
        }
        pumpCiv(nowMs());
        return true;
    }
    if (observation == IcomCivScheduler::Observation::Accepted
        && m_dualPresetReads.erase(key) && !m_dualSavedPresets.contains(key)) {
        m_dualSavedPresets[key] = ScopeEdgeWindow{*low, *high};
    }
    for (int receiver = 0; receiver < 2; ++receiver) {
        if (m_receivers[receiver].windowRange == range && m_receivers[receiver].borrowedSlot == slot) {
            flushDualScopeWindow(receiver);
        }
    }
    pumpCiv(nowMs());
    return true;
}

bool IcomCivBackend::restoreDualScopePresetsConfirmed()
{
    if (!m_session || !m_connected || m_dualRestoring) { return false; }
    const QPointer<IcomCivBackend> alive(this);
    const auto generation = m_sessionGeneration;
    m_dualRestoring = true;
    retirePcmStreams();
    m_dualPanOpening = m_dualPanClosing = false;
    ++m_dualPaneEpoch;
    for (auto& state : m_receivers) {
        state.windowTarget.reset();
        state.pendingCentreHz.reset();
        state.windowFlushScheduled = false;
        ++state.windowEpoch;
    }
    terminateScheduler(IcomCivScheduler::TerminalOutcome::Cancelled, SchedulerWaiterOutcome::Cancelled);
    const auto address = m_session->civAddress();
    queueWrite(cmdSetPtt(address, false), "ptt", IcomCivScheduler::Priority::Emergency, true, false);
    if (capabilities().hasRadioSideCwKeyer) {
        queueWrite(cmdAbortCwMessage(address), "cw.message", IcomCivScheduler::Priority::Ptt, true, false);
    }
    const auto restore = [this](const std::vector<std::uint8_t>& frame) {
        const auto key = semanticKey(frame);
        queueWrite(frame, key, IcomCivScheduler::Priority::Operator);
        if (const auto read = confirmationFor(frame)) {
            queueRead(*read, key, IcomCivScheduler::Priority::Operator, nowMs() + 60);
        }
    };
    m_dualRestoreExpected.clear();
    m_dualRestoreSlots = {};
    for (const auto& [key, value] : m_dualSavedPresets) {
        if (!m_dualWrittenPresets.contains(key)) { continue; }
        m_dualRestoreExpected[key] = value;
        restore(cmdScopeFixedEdge(address, key.first, key.second, value.lowerHz, value.upperHz));
    }
    for (int receiver = 0; receiver < 2; ++receiver) {
        const auto& state = m_receivers[receiver];
        if (state.originalSlot && (state.edgeSlot == state.borrowedSlot || state.edgeSlot == state.originalSlot)) {
            m_dualRestoreSlots[receiver] = state.originalSlot;
            restore(cmdScopeEdgeNumber(address, state.originalSlot, static_cast<std::uint8_t>(receiver)));
        }
    }
    const bool restoreWatch = m_dualWatchOwned && m_dualWatch == true;
    if (restoreWatch) {
        restore(buildFrameSub(address, cmd::kVfoControl, 0xc2, std::array<std::uint8_t,1>{0}));
    }
    // Deauthentication must follow the serialized setting readbacks. Keep the
    // owner event loop serving replies, with a bounded wait and no GUI input.
    QEventLoop loop;
    QTimer timer;
    const qint64 deadline = nowMs() + 12000;
    bool confirmed = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [alive, generation, deadline, restoreWatch, &loop, &confirmed] {
        if (!alive || alive->m_sessionGeneration != generation) { loop.quit(); return; }
        confirmed = alive->m_dualRestoreExpected.empty()
            && std::ranges::all_of(alive->m_dualRestoreSlots, [](int slot) { return slot == 0; })
            && (!restoreWatch || alive->m_dualWatch == false);
        if (confirmed || alive->nowMs() >= deadline) { loop.quit(); return; }
        alive->pumpCiv(alive->nowMs());
    });
    timer.start(10);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!alive || m_sessionGeneration != generation) { return false; }
    m_dualRestoring = false;
    return confirmed;
}

}  // namespace AetherSDR::icom
