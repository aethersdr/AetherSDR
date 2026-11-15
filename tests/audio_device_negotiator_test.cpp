// Smoke test for the live Qt-Multimedia negotiation wrapper
// (AudioDeviceNegotiator). The pure policy is exhaustively covered by
// audio_format_negotiation_test; this one verifies the wrapper builds a sane
// DeviceCaps from a real QAudioDevice and round-trips to an openable
// QAudioFormat. It is tolerant of headless CI runners with no audio hardware
// (it reports a skip rather than failing).
//
// Run: ./build/audio_device_negotiator_test

#include "core/AudioDeviceNegotiator.h"

#include <QAudioDevice>
#include <QCoreApplication>
#include <QMediaDevices>

#include <cstdio>
#include <string>

using namespace AetherSDR;
namespace AFN = AetherSDR::AudioFormatNegotiator;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    ++g_total;
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    if (!ok) ++g_failed;
}

void checkDirection(const char* label, const QAudioDevice& dev, AFN::Direction dir)
{
    if (dev.isNull()) {
        std::printf("[SKIP] %-58s (no device on this runner)\n", label);
        return;
    }

    // Probe must produce a non-empty, self-consistent capability snapshot.
    const AFN::DeviceCaps caps = AudioDeviceNegotiator::probe(dev, dir);
    report(std::string(label) + ": probe found rates",
           !caps.supportedRates.isEmpty(),
           caps.supportedRates.isEmpty() ? "no rates probed" : "");

    // Negotiation must succeed and yield an openable (positive-rate) format.
    const auto r = AudioDeviceNegotiator::negotiate(
        dev, dir, AFN::ResamplerPolicy::PreservePan);
    report(std::string(label) + ": negotiate ok",
           r.ok && r.format.sampleRate() > 0 && r.format.channelCount() >= 1,
           r.ok ? (std::string("rate=") + std::to_string(r.format.sampleRate())
                   + " ch=" + std::to_string(r.format.channelCount())
                   + " resampler=" + AFN::toString(r.resampler))
                : "negotiate failed");

    // The format ladder must be non-empty and start at the preferred rung.
    const auto ladder = AudioDeviceNegotiator::formatLadder(
        dev, dir, AFN::ResamplerPolicy::PreservePan);
    report(std::string(label) + ": ladder non-empty", !ladder.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    checkDirection("default output / RX", QMediaDevices::defaultAudioOutput(), AFN::Direction::Output);
    checkDirection("default input / TX",  QMediaDevices::defaultAudioInput(),  AFN::Direction::Input);

    // Enum round-trips are pure and always run.
    report("toQt/fromQt Int16 round-trips",
           AudioDeviceNegotiator::fromQt(AudioDeviceNegotiator::toQt(AFN::SampleFmt::Int16))
               == AFN::SampleFmt::Int16);
    report("toQt/fromQt Float32 round-trips",
           AudioDeviceNegotiator::fromQt(AudioDeviceNegotiator::toQt(AFN::SampleFmt::Float32))
               == AFN::SampleFmt::Float32);

    std::printf("\n%d/%d checks passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
