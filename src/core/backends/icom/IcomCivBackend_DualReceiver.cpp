#include "core/backends/icom/IcomCivBackend.h"
#include "core/Resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
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
    // Exact 29 marks in both model guides; TX levels/functions are global.
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
        state.calibration.spanDb = 100.0;
        state.audio = std::make_unique<Resampler>(m_audioRateHz, kEngineAudioRateHz, 4096);
        const QString id = QString::number(receiver);
        emit panCenterBandwidthChanged(id, 0.0, 0.0);
        emit panRangeChanged(id, state.calibration.floorDbm,
                             state.calibration.floorDbm + state.calibration.spanDb);
        emit panBandwidthLimitsChanged(id, 0.005, 1.0);
        SliceDelta delta;
        delta.inUse = true;
        delta.panId = id;
        delta.active = receiver == m_dualFocus;
        delta.txSlice = receiver == 0;
        delta.audioGain = state.monitorGain;
        delta.audioPan = state.monitorPan;
        delta.audioMute = state.monitorMute;
        emit sliceChanged(receiver, delta);
    }
    if (m_session->rxChannels() != 2) {
        emit configurationWarning(QStringLiteral(
            "Dual-receiver radio connected with mono LAN audio. SUB audio is unavailable; "
            "reconnect with icom.rxChannels=2 for a non-default CI-V address."));
    }
    pollDualReceivers();
}

void IcomCivBackend::pollDualReceivers()
{
    if (!m_session || !m_connected) { return; }
    const std::uint8_t address = m_session->civAddress();
    const auto read = [this](const std::vector<std::uint8_t>& frame) {
        queueRead(frame, semanticKey(frame), IcomCivScheduler::Priority::Control);
    };
    for (std::uint8_t receiver : {0, 1}) {
        read(buildFrameSub(address, 0x25, receiver));
        read(buildFrameSub(address, 0x26, receiver));
        queueRead(buildFrameSub(address, cmd::kScope, scope::kReference,
                  std::array<std::uint8_t,1>{receiver}),
                  "scope.reference." + std::to_string(receiver),
                  IcomCivScheduler::Priority::Maintenance, 0, {receiver});
    }
    read(buildFrame(address, cmd::kDuplex));
    read(cmdReadTransmitFrequency(address));
    read(buildFrameSub(address, cmd::kControl, control::kPtt));
    const auto mod = currentModulationProfile();
    for (int item : {profileFor(*m_model).dualReceiver->lanOutputItem,
                     mod->dataOffInputItem, mod->dataInputItem, mod->networkLevelItem}) {
        read(cmdReadSetting(address, item));
    }
    pumpCiv(nowMs());
}

void IcomCivBackend::publishDualReceiver(int receiver)
{
    const ReceiverState& state = m_receivers[receiver];
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
    for (int receiver = 0; receiver < 2; ++receiver) {
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
    if (!hasDualReceivers() || receiver < 0 || receiver > 1) { return; }
    m_dualFocus = receiver;
    for (int index = 0; index < 2; ++index) {
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
        state.centreHz = sweep->centreHz();
        state.spanHz = sweep->bandwidthHz() / 2;
        state.scopeMode = sweep->mode;
        const std::vector<float> bins = toDbm(*sweep, state.scope.geometry(), state.calibration);
        emit panCenterBandwidthChanged(QString::number(receiver), sweep->centreHz() / 1e6,
                                       sweep->bandwidthHz() / 1e6);
        // CI-V sends ordered amplitudes, low to high. No IQ conjugation or
        // FFT half-rotation belongs here. RadioModel uses this same row for WF.
        emit spectrumFrameReady(receiver, QByteArray(reinterpret_cast<const char*>(bins.data()),
            static_cast<qsizetype>(bins.size() * sizeof(float))));
        return true;
    }
    if (frame.cmd == cmd::kControl && frame.hasSub && frame.sub == control::kReadTxFreq) {
        if (frame.data.size() != kFreqBytes) { return true; }
        const auto frequency = decodeFreq(frame.data);
        if (!frequency) { return true; }
        m_civScheduler.observe(frame, nowMs());
        TransmitDelta delta;
        delta.transmitFreq = *frequency / 1e6;
        emit transmitChanged(delta);
        return true;
    }
    const bool frequency = frame.cmd == 0x25;
    const bool mode = frame.cmd == 0x26;
    const bool split = frame.cmd == cmd::kDuplex;
    const bool scopeState = frame.cmd == cmd::kScope;
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
    if (!frequency && !mode && !split && !scopeState) { return false; }
    if ((frequency || mode) && (!frame.hasSub || frame.sub > 1)) { return true; }
    if (frequency && (frame.data.size() != kFreqBytes || !decodeFreq(frame.data))) { return true; }
    if (mode && (frame.data.size() != 3 || frame.data[1] > 3
        || frame.data[2] < 1 || frame.data[2] > 3
        || modeToNeutral(static_cast<CivMode>(frame.data[0]), frame.data[1] != 0).empty())) { return true; }
    if (split && (frame.data.size() != 1 || frame.data[0] > 1)) { return true; }
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
        state.frequencyHz = *decodeFreq(frame.data);
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
            if (value >= -30 && value <= 10) {
                m_receivers[frame.data[0]].calibration.referenceDb = value;
                publishScopeDbmRange();
            }
        }
    } else if (split) {
        const int receiver = frame.data[0];
        if (receiver != m_dualTxReceiver) { m_dataModInput = -1; }
        m_dualSplit = receiver != 0;
        m_dualTxReceiver = receiver;
    }
    syncDualTransmit();
    pumpCiv(now);
    return true;
}

void IcomCivBackend::onDualAudio(const std::vector<float>& interleaved)
{
    if (!m_connected || interleaved.empty() || interleaved.size() % 2 != 0
        || interleaved.size() > static_cast<std::size_t>(PcmFrame::kMaxFrames) * 2
        || m_lanOutput != 0 || !m_dualInitialised
        || !std::ranges::all_of(interleaved, [](float v) { return std::isfinite(v); })) { return; }
    const std::size_t frames = interleaved.size() / 2;
    std::array<QByteArray, 2> streams;
    for (int receiver = 0; receiver < 2; ++receiver) {
        if (!m_receivers[receiver].audio) { return; }
        std::vector<float> mono(frames);
        for (std::size_t i = 0; i < frames; ++i) { mono[i] = interleaved[i * 2 + receiver]; }
        streams[receiver] = m_receivers[receiver].audio->processMonoToStereo(mono.data(), static_cast<int>(frames));
        if (streams[receiver].isEmpty()) { return; }
    }
    if (streams[0].size() != streams[1].size()) { return; }
    for (int receiver = 0; receiver < 2; ++receiver) {
        publishLegacySliceAudio(receiver, streams[receiver]);
    }
    QByteArray mixed(streams[0].size(), Qt::Uninitialized);
    const float* main = reinterpret_cast<const float*>(streams[0].constData());
    const float* sub = reinterpret_cast<const float*>(streams[1].constData());
    float* out = reinterpret_cast<float*>(mixed.data());
    for (qsizetype i = 0; i < mixed.size() / static_cast<qsizetype>(sizeof(float)); ++i) {
        const auto monitorScale = [i](const ReceiverState& state) {
            const double balance = state.monitorPan / 100.0;
            const double channel = i % 2 == 0 ? std::min(1.0, 2.0 * (1.0 - balance))
                                             : std::min(1.0, 2.0 * balance);
            return state.monitorMute ? 0.0f : static_cast<float>(state.monitorGain / 100.0 * channel);
        };
        out[i] = 0.5f * (main[i] * monitorScale(m_receivers[0])
                        + sub[i] * monitorScale(m_receivers[1]));
    }
    publishLegacyAudio(mixed);
}

}  // namespace AetherSDR::icom
