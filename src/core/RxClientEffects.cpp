#include "RxClientEffects.h"

namespace AetherSDR {
namespace {

// Avoid marking every effect dirty for every packet. Setters publish atomics
// and some invalidate coefficients; a parameter that has not changed must not
// interrupt its source's smoothing or force another coefficient calculation.
template <typename Effect, typename Value>
void syncParameter(Effect& target, const Effect& source,
                   Value (Effect::*getter)() const noexcept,
                   void (Effect::*setter)(Value) noexcept) noexcept
{
    const Value value = (source.*getter)();
    if ((target.*getter)() != value) {
        (target.*setter)(value);
    }
}

bool equalBands(const ClientEq::BandParams& first,
                const ClientEq::BandParams& second) noexcept
{
    return first.freqHz == second.freqHz && first.gainDb == second.gainDb
        && first.q == second.q && first.type == second.type
        && first.enabled == second.enabled
        && first.slopeDbPerOct == second.slopeDbPerOct;
}

} // namespace

RxClientEffects::RxClientEffects(int sampleRate)
{
    m_eq.prepare(sampleRate);
    m_gate.prepare(sampleRate);
    m_comp.prepare(sampleRate);
    m_deEss.prepare(sampleRate);
    m_tube.prepare(sampleRate);
    m_pudu.prepare(sampleRate);
}

void RxClientEffects::syncParametersFrom(
    const ClientEq& eq, const ClientGate& gate, const ClientComp& comp,
    const ClientDeEss& deEss, const ClientTube& tube, const ClientPudu& pudu) noexcept
{
    syncParameter(m_eq, eq, &ClientEq::isEnabled, &ClientEq::setEnabled);
    syncParameter(m_eq, eq, &ClientEq::masterGain, &ClientEq::setMasterGain);
    syncParameter(m_eq, eq, &ClientEq::filterFamily, &ClientEq::setFilterFamily);
    syncParameter(m_eq, eq, &ClientEq::activeBandCount, &ClientEq::setActiveBandCount);
    for (int index = 0; index < ClientEq::kMaxBands; ++index) {
        const ClientEq::BandParams parameters = eq.band(index);
        if (!equalBands(m_eq.band(index), parameters)) {
            m_eq.setBand(index, parameters);
        }
    }

    syncParameter(m_gate, gate, &ClientGate::isEnabled, &ClientGate::setEnabled);
    // Mode applies presets. Restore the explicit ratio/floor afterwards so
    // changes to the mode do not discard the operator's fine-tuned values.
    syncParameter(m_gate, gate, &ClientGate::mode, &ClientGate::setMode);
    syncParameter(m_gate, gate, &ClientGate::thresholdDb, &ClientGate::setThresholdDb);
    syncParameter(m_gate, gate, &ClientGate::ratio, &ClientGate::setRatio);
    syncParameter(m_gate, gate, &ClientGate::attackMs, &ClientGate::setAttackMs);
    syncParameter(m_gate, gate, &ClientGate::releaseMs, &ClientGate::setReleaseMs);
    syncParameter(m_gate, gate, &ClientGate::holdMs, &ClientGate::setHoldMs);
    syncParameter(m_gate, gate, &ClientGate::floorDb, &ClientGate::setFloorDb);
    syncParameter(m_gate, gate, &ClientGate::returnDb, &ClientGate::setReturnDb);
    syncParameter(m_gate, gate, &ClientGate::lookaheadMs, &ClientGate::setLookaheadMs);

    syncParameter(m_comp, comp, &ClientComp::isEnabled, &ClientComp::setEnabled);
    syncParameter(m_comp, comp, &ClientComp::thresholdDb, &ClientComp::setThresholdDb);
    syncParameter(m_comp, comp, &ClientComp::ratio, &ClientComp::setRatio);
    syncParameter(m_comp, comp, &ClientComp::attackMs, &ClientComp::setAttackMs);
    syncParameter(m_comp, comp, &ClientComp::releaseMs, &ClientComp::setReleaseMs);
    syncParameter(m_comp, comp, &ClientComp::kneeDb, &ClientComp::setKneeDb);
    syncParameter(m_comp, comp, &ClientComp::makeupDb, &ClientComp::setMakeupDb);
    syncParameter(m_comp, comp, &ClientComp::limiterEnabled, &ClientComp::setLimiterEnabled);
    syncParameter(m_comp, comp, &ClientComp::limiterCeilingDb, &ClientComp::setLimiterCeilingDb);
    syncParameter(m_comp, comp, &ClientComp::driveDb, &ClientComp::setDriveDb);
    syncParameter(m_comp, comp, &ClientComp::phaseRotatorStages, &ClientComp::setPhaseRotatorStages);

    syncParameter(m_deEss, deEss, &ClientDeEss::isEnabled, &ClientDeEss::setEnabled);
    syncParameter(m_deEss, deEss, &ClientDeEss::frequencyHz, &ClientDeEss::setFrequencyHz);
    syncParameter(m_deEss, deEss, &ClientDeEss::q, &ClientDeEss::setQ);
    syncParameter(m_deEss, deEss, &ClientDeEss::thresholdDb, &ClientDeEss::setThresholdDb);
    syncParameter(m_deEss, deEss, &ClientDeEss::amountDb, &ClientDeEss::setAmountDb);
    syncParameter(m_deEss, deEss, &ClientDeEss::attackMs, &ClientDeEss::setAttackMs);
    syncParameter(m_deEss, deEss, &ClientDeEss::releaseMs, &ClientDeEss::setReleaseMs);
    syncParameter(m_deEss, deEss, &ClientDeEss::slopeStages, &ClientDeEss::setSlopeStages);

    syncParameter(m_tube, tube, &ClientTube::isEnabled, &ClientTube::setEnabled);
    syncParameter(m_tube, tube, &ClientTube::model, &ClientTube::setModel);
    syncParameter(m_tube, tube, &ClientTube::driveDb, &ClientTube::setDriveDb);
    syncParameter(m_tube, tube, &ClientTube::biasAmount, &ClientTube::setBiasAmount);
    syncParameter(m_tube, tube, &ClientTube::tone, &ClientTube::setTone);
    syncParameter(m_tube, tube, &ClientTube::outputGainDb, &ClientTube::setOutputGainDb);
    syncParameter(m_tube, tube, &ClientTube::dryWet, &ClientTube::setDryWet);
    syncParameter(m_tube, tube, &ClientTube::envelopeAmount, &ClientTube::setEnvelopeAmount);
    syncParameter(m_tube, tube, &ClientTube::attackMs, &ClientTube::setAttackMs);
    syncParameter(m_tube, tube, &ClientTube::releaseMs, &ClientTube::setReleaseMs);

    syncParameter(m_pudu, pudu, &ClientPudu::isEnabled, &ClientPudu::setEnabled);
    syncParameter(m_pudu, pudu, &ClientPudu::mode, &ClientPudu::setMode);
    syncParameter(m_pudu, pudu, &ClientPudu::pooDriveDb, &ClientPudu::setPooDriveDb);
    syncParameter(m_pudu, pudu, &ClientPudu::pooTuneHz, &ClientPudu::setPooTuneHz);
    syncParameter(m_pudu, pudu, &ClientPudu::pooMix, &ClientPudu::setPooMix);
    syncParameter(m_pudu, pudu, &ClientPudu::dooTuneHz, &ClientPudu::setDooTuneHz);
    syncParameter(m_pudu, pudu, &ClientPudu::dooHarmonicsDb, &ClientPudu::setDooHarmonicsDb);
    syncParameter(m_pudu, pudu, &ClientPudu::dooMix, &ClientPudu::setDooMix);
}

void RxClientEffects::reset() noexcept
{
    m_eq.reset();
    m_gate.reset();
    m_comp.reset();
    m_deEss.reset();
    m_tube.reset();
    m_pudu.reset();
}

} // namespace AetherSDR
