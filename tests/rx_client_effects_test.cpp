// Socket-free production-effect checks: source isolation, rate-specific delay
// and bandwidth, live parameter changes and discontinuity reset.
#include "core/RxClientEffects.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++g_failures;
    }
}

// Independent reference uses the six original effect classes directly in the
// shipped playback order; it does not call the helper's parameter copier.
struct ReferenceEffects {
    ClientEq eq;
    ClientGate gate;
    ClientComp comp;
    ClientDeEss deEss;
    ClientTube tube;
    ClientPudu pudu;

    explicit ReferenceEffects(int sampleRate)
    {
        prepare(sampleRate);
    }

    void prepare(int sampleRate)
    {
        eq.prepare(sampleRate);
        gate.prepare(sampleRate);
        comp.prepare(sampleRate);
        deEss.prepare(sampleRate);
        tube.prepare(sampleRate);
        pudu.prepare(sampleRate);
    }

    void reset()
    {
        eq.reset();
        gate.reset();
        comp.reset();
        deEss.reset();
        tube.reset();
        pudu.reset();
    }

    void process(std::vector<float>& audio)
    {
        const int frames = static_cast<int>(audio.size() / 2);
        eq.process(audio.data(), frames, 2);
        gate.process(audio.data(), frames, 2);
        comp.process(audio.data(), frames, 2);
        deEss.process(audio.data(), frames, 2);
        tube.process(audio.data(), frames, 2);
        pudu.process(audio.data(), frames, 2);
    }
};

void configure(ReferenceEffects& effects, int variant)
{
    effects.eq.setEnabled(true);
    effects.eq.setMasterGain(variant == 0 ? 0.8f : 1.2f);
    effects.eq.setFilterFamily(variant == 0 ? ClientEq::FilterFamily::Bessel
                                           : ClientEq::FilterFamily::Chebyshev);
    effects.eq.setActiveBandCount(3);
    effects.eq.setBand(0, {180.0f, 0.0f, 0.8f, ClientEq::FilterType::HighPass, true, 24});
    effects.eq.setBand(1, {1700.0f, variant == 0 ? 5.0f : -4.0f, 1.8f,
                          ClientEq::FilterType::Peak, true, 12});
    effects.eq.setBand(2, {6000.0f, -3.0f, 0.9f,
                          ClientEq::FilterType::HighShelf, variant == 0, 12});

    effects.gate.setEnabled(true);
    effects.gate.setMode(variant == 0 ? ClientGate::Mode::Gate : ClientGate::Mode::Expander);
    effects.gate.setThresholdDb(-26.0f);
    effects.gate.setRatio(4.2f);
    effects.gate.setAttackMs(1.8f);
    effects.gate.setReleaseMs(70.0f);
    effects.gate.setHoldMs(7.0f);
    effects.gate.setFloorDb(-21.0f);
    effects.gate.setReturnDb(4.5f);
    effects.gate.setLookaheadMs(variant == 0 ? 2.5f : 0.5f);

    effects.comp.setEnabled(true);
    effects.comp.setThresholdDb(-23.0f);
    effects.comp.setRatio(5.5f);
    effects.comp.setAttackMs(3.0f);
    effects.comp.setReleaseMs(90.0f);
    effects.comp.setKneeDb(2.0f);
    effects.comp.setMakeupDb(variant == 0 ? 2.0f : 4.0f);
    effects.comp.setLimiterEnabled(true);
    effects.comp.setLimiterCeilingDb(-5.0f);
    effects.comp.setDriveDb(6.0f);
    effects.comp.setPhaseRotatorStages(4);

    effects.deEss.setEnabled(true);
    effects.deEss.setFrequencyHz(5500.0f);
    effects.deEss.setQ(1.3f);
    effects.deEss.setThresholdDb(-43.0f);
    effects.deEss.setAmountDb(-10.0f);
    effects.deEss.setAttackMs(2.0f);
    effects.deEss.setReleaseMs(40.0f);
    effects.deEss.setSlopeStages(3);

    effects.tube.setEnabled(true);
    effects.tube.setModel(variant == 0 ? ClientTube::Model::C : ClientTube::Model::B);
    effects.tube.setDriveDb(8.0f);
    effects.tube.setBiasAmount(0.2f);
    effects.tube.setTone(-0.3f);
    effects.tube.setOutputGainDb(-3.0f);
    effects.tube.setDryWet(0.6f);
    effects.tube.setEnvelopeAmount(-0.5f);
    effects.tube.setAttackMs(3.0f);
    effects.tube.setReleaseMs(55.0f);

    effects.pudu.setEnabled(true);
    effects.pudu.setMode(variant == 0 ? ClientPudu::Mode::Behringer : ClientPudu::Mode::Aphex);
    effects.pudu.setPooDriveDb(8.0f);
    effects.pudu.setPooTuneHz(130.0f);
    effects.pudu.setPooMix(0.2f);
    effects.pudu.setDooTuneHz(4200.0f);
    effects.pudu.setDooHarmonicsDb(9.0f);
    effects.pudu.setDooMix(0.35f);
}

void sync(RxClientEffects& effects, const ReferenceEffects& source)
{
    effects.syncParametersFrom(source.eq, source.gate, source.comp,
                               source.deEss, source.tube, source.pudu);
}

void process(RxClientEffects& effects, std::vector<float>& audio)
{
    const int frames = static_cast<int>(audio.size() / 2);
    effects.eq().process(audio.data(), frames, 2);
    effects.gate().process(audio.data(), frames, 2);
    effects.comp().process(audio.data(), frames, 2);
    effects.deEss().process(audio.data(), frames, 2);
    effects.tube().process(audio.data(), frames, 2);
    effects.pudu().process(audio.data(), frames, 2);
}

std::vector<float> signal(int sampleRate, int frames, int offset, int source)
{
    std::vector<float> audio(static_cast<size_t>(frames * 2));
    for (int frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame + offset) / sampleRate;
        const float envelope = (frame + offset) % 1900 < 500 ? 0.03f : 0.4f;
        audio[frame * 2] = envelope * static_cast<float>(
            std::sin(2.0 * std::numbers::pi * (650.0 + source * 193.0) * time)
            + 0.2 * std::sin(2.0 * std::numbers::pi * 5500.0 * time));
        audio[frame * 2 + 1] = envelope * static_cast<float>(
            std::sin(2.0 * std::numbers::pi * (1900.0 + source * 719.0) * time));
    }
    return audio;
}

bool equalAudio(const std::vector<float>& first, const std::vector<float>& second)
{
    return first.size() == second.size()
        && std::equal(first.begin(), first.end(), second.begin(), [](float a, float b) {
            return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 1.0e-6f;
        });
}

void checkConcurrentSources()
{
    ReferenceEffects main(48000);
    ReferenceEffects legacyReference(24000);
    ReferenceEffects profileReference(24000);
    ReferenceEffects otherProfileReference(24000);
    RxClientEffects legacy(24000);
    RxClientEffects profile(24000);
    RxClientEffects otherProfile(24000);
    configure(main, 0);
    configure(legacyReference, 0);
    configure(profileReference, 0);
    configure(otherProfileReference, 0);

    bool legacyMatches = true;
    bool profileMatches = true;
    bool otherProfileMatches = true;
    for (int block = 0; block < 160; ++block) {
        if (block == 50) {
            configure(main, 1);
            configure(legacyReference, 1);
            configure(profileReference, 1);
            configure(otherProfileReference, 1);
        }
        if (block == 90) {
            // One endpoint's discontinuity must not flush a concurrent source.
            profile.reset();
            profileReference.reset();
        }
        if (block == 110) {
            // A main-source format replacement does not change Kiwi's rate or
            // histories, nor copy the main signal's state into an auxiliary bank.
            main.prepare(24000);
        }
        sync(legacy, main);
        sync(profile, main);
        sync(otherProfile, main);
        std::vector<float> mainAudio = signal(block < 110 ? 48000 : 24000, 240, block * 240, 0);
        main.process(mainAudio);
        std::vector<float> legacyAudio = signal(24000, 120, block * 120, 1);
        std::vector<float> referenceAudio = legacyAudio;
        legacyReference.process(referenceAudio);
        process(legacy, legacyAudio);
        legacyMatches = legacyMatches && equalAudio(legacyAudio, referenceAudio);
        std::vector<float> profileAudio = signal(24000, 120, block * 120, 2);
        referenceAudio = profileAudio;
        profileReference.process(referenceAudio);
        process(profile, profileAudio);
        profileMatches = profileMatches && equalAudio(profileAudio, referenceAudio);
        std::vector<float> otherAudio = signal(24000, 120, block * 120, 3);
        referenceAudio = otherAudio;
        otherProfileReference.process(referenceAudio);
        process(otherProfile, otherAudio);
        otherProfileMatches = otherProfileMatches && equalAudio(otherAudio, referenceAudio);
    }
    check(legacyMatches, "Kiwi24 legacy effects match independent 24 kHz chain beside main48");
    check(profileMatches, "Kiwi24 profile effects retain independent state and reset locally");
    check(otherProfileMatches, "second Kiwi24 profile survives other source resets and rate changes");
    check(legacy.gate().mode() == ClientGate::Mode::Expander
          && legacy.gate().ratio() == 4.2f && legacy.gate().floorDb() == -21.0f,
          "mode synchronization preserves explicit gate ratio and floor overrides");
}

void checkRateDomains()
{
    for (const int rate : {24000, 48000}) {
        ReferenceEffects reference(rate);
        RxClientEffects actual(rate);
        configure(reference, 0);
        bool matches = true;
        for (int block = 0; block < 80; ++block) {
            if (block == 30) {
                configure(reference, 1);
            }
            sync(actual, reference);
            std::vector<float> audio = signal(rate, 120, block * 120, 1);
            std::vector<float> expected = audio;
            process(actual, audio);
            reference.process(expected);
            matches = matches && equalAudio(audio, expected);
        }
        check(matches, rate == 24000 ? "24 kHz enabled effects preserve original processing and parameter changes"
                                   : "48 kHz enabled effects match independently prepared original processors");

        RxClientEffects effects(rate);
        effects.gate().setEnabled(true);
        effects.gate().setRatio(1.0f);
        effects.gate().setFloorDb(0.0f);
        effects.gate().setLookaheadMs(2.5f);
        std::vector<float> impulse(static_cast<size_t>(rate / 100 * 2), 0.0f);
        impulse[0] = 0.5f;
        impulse[1] = -0.5f;
        process(effects, impulse);
        const int delayFrames = rate / 400;
        const bool onlyAtDelay = std::all_of(impulse.begin(), impulse.begin() + delayFrames * 2,
                                           [](float value) { return value == 0.0f; });
        check(onlyAtDelay && impulse[delayFrames * 2] == 0.5f
              && impulse[delayFrames * 2 + 1] == -0.5f,
              rate == 24000 ? "24 kHz gate lookahead is 2.5 ms" : "48 kHz gate lookahead is 2.5 ms");
        // Leave a pulse pending inside the lookahead, then mark the source
        // discontinuous. A reset that forgets delayed samples leaks this pulse.
        std::vector<float> queuedPulse{0.5f, -0.5f};
        process(effects, queuedPulse);
        effects.reset();
        std::vector<float> silence(impulse.size(), 0.0f);
        process(effects, silence);
        check(std::all_of(silence.begin(), silence.end(), [](float value) { return value == 0.0f; }),
              rate == 24000 ? "24 kHz reset removes lookahead samples" : "48 kHz reset removes lookahead samples");
    }

    RxClientEffects wideband(48000);
    wideband.eq().setEnabled(true);
    wideband.eq().setActiveBandCount(1);
    wideband.eq().setBand(0, {15000.0f, 6.0f, 1.0f, ClientEq::FilterType::Peak, true, 12});
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    for (int block = 0; block < 300; ++block) {
        std::vector<float> tone(480);
        for (int frame = 0; frame < 240; ++frame) {
            const float value = 0.1f * static_cast<float>(std::sin(
                2.0 * std::numbers::pi * 15000.0 * (block * 240 + frame) / 48000.0));
            tone[frame * 2] = tone[frame * 2 + 1] = value;
            if (block > 200) {
                inputEnergy += value * value * 2;
            }
        }
        process(wideband, tone);
        if (block > 200) {
            for (const float value : tone) {
                outputEnergy += value * value;
            }
        }
    }
    const double gainDb = 10.0 * std::log10(outputEnergy / inputEnergy);
    check(std::isfinite(gainDb) && std::abs(gainDb - 6.0) < 0.1,
          "48 kHz EQ applies requested gain at 15 kHz without a 24 kHz bandwidth ceiling");
}

void checkConcurrentRateReaders()
{
    ReferenceEffects main(24000);
    std::atomic<bool> finished{false};
    std::atomic<bool> valid{true};
    std::thread reader([&]() {
        while (!finished.load(std::memory_order_acquire)) {
            for (const double rate : {main.eq.sampleRate(), main.gate.sampleRate(),
                                     main.comp.sampleRate(), main.deEss.sampleRate(),
                                     main.tube.sampleRate(), main.pudu.sampleRate()}) {
                if (rate != 24000.0 && rate != 48000.0) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
        }
    });
    for (int change = 0; change < 1000; ++change) {
        main.prepare(change % 2 == 0 ? 48000 : 24000);
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    check(valid.load(std::memory_order_relaxed),
          "GUI rate snapshots remain valid while audio owner prepares replacement rates");
}

void checkMeterCopies()
{
    ReferenceEffects presented(24000);
    ReferenceEffects main(48000);
    ReferenceEffects untouchedMain(48000);
    configure(presented, 1);
    configure(main, 0);
    configure(untouchedMain, 0);
    presented.comp.setThresholdDb(-35.0f);
    for (int block = 0; block < 100; ++block) {
        std::vector<float> audio = signal(24000, 240, block * 240, 1);
        presented.process(audio);
    }
    main.gate.copyMeteringFrom(presented.gate);
    main.comp.copyMeteringFrom(presented.comp);
    main.deEss.copyMeteringFrom(presented.deEss);
    main.tube.copyMeteringFrom(presented.tube);
    main.pudu.copyMeteringFrom(presented.pudu);
    check(main.gate.inputPeakDb() == presented.gate.inputPeakDb()
          && main.gate.outputPeakDb() == presented.gate.outputPeakDb()
          && main.gate.gainReductionDb() == presented.gate.gainReductionDb()
          && main.gate.gateOpen() == presented.gate.gateOpen()
          && main.comp.inputPeakDb() == presented.comp.inputPeakDb()
          && main.comp.outputPeakDb() == presented.comp.outputPeakDb()
          && main.comp.gainReductionDb() == presented.comp.gainReductionDb()
          && main.comp.limiterGrDb() == presented.comp.limiterGrDb()
          && main.comp.limiterActive() == presented.comp.limiterActive()
          && main.deEss.inputPeakDb() == presented.deEss.inputPeakDb()
          && main.deEss.sidechainPeakDb() == presented.deEss.sidechainPeakDb()
          && main.deEss.gainReductionDb() == presented.deEss.gainReductionDb()
          && main.tube.inputPeakDb() == presented.tube.inputPeakDb()
          && main.tube.outputPeakDb() == presented.tube.outputPeakDb()
          && main.tube.driveAppliedDb() == presented.tube.driveAppliedDb()
          && main.pudu.inputPeakDb() == presented.pudu.inputPeakDb()
          && main.pudu.outputPeakDb() == presented.pudu.outputPeakDb()
          && main.pudu.wetRmsDb() == presented.pudu.wetRmsDb(),
          "auxiliary meter copies update every UI-facing dynamics snapshot");
    std::vector<float> actual = signal(48000, 2400, 0, 2);
    std::vector<float> expected = actual;
    main.process(actual);
    untouchedMain.process(expected);
    check(main.comp.thresholdDb() == -23.0f && equalAudio(actual, expected),
          "copying auxiliary meters preserves main parameters and processing histories");
}

void checkPhaseHistoryReset()
{
    RxClientEffects effects(24000);
    effects.comp().setEnabled(true);
    effects.comp().setRatio(1.0f);
    effects.comp().setLimiterEnabled(false);
    effects.comp().setPhaseRotatorStages(4);
    std::vector<float> pulse{0.5f, -0.5f};
    process(effects, pulse);
    effects.reset();
    std::vector<float> silence(480, 0.0f);
    process(effects, silence);
    check(std::all_of(silence.begin(), silence.end(), [](float sample) { return sample == 0.0f; }),
          "source reset clears compressor phase-rotator history as well as envelopes");
}

} // namespace

int main()
{
    checkConcurrentSources();
    checkRateDomains();
    checkConcurrentRateReaders();
    checkMeterCopies();
    checkPhaseHistoryReset();
    return g_failures == 0 ? 0 : 1;
}
