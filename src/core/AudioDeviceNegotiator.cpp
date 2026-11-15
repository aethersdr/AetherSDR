#include "core/AudioDeviceNegotiator.h"

namespace AetherSDR {
namespace AudioDeviceNegotiator {

namespace AFN = AudioFormatNegotiator;

QAudioFormat::SampleFormat toQt(AFN::SampleFmt f)
{
    switch (f) {
    case AFN::SampleFmt::Int16:   return QAudioFormat::Int16;
    case AFN::SampleFmt::Float32: return QAudioFormat::Float;
    }
    return QAudioFormat::Float;
}

AFN::SampleFmt fromQt(QAudioFormat::SampleFormat f)
{
    // Only Int16 and Float32 are modelled; everything else negotiates as Float.
    return (f == QAudioFormat::Int16) ? AFN::SampleFmt::Int16 : AFN::SampleFmt::Float32;
}

QAudioFormat makeFormat(int rate, AFN::SampleFmt fmt, int channels)
{
    QAudioFormat f;
    f.setSampleRate(rate);
    f.setChannelCount(channels);
    f.setSampleFormat(toQt(fmt));
    return f;
}

AFN::DeviceCaps probe(const QAudioDevice& dev, AFN::Direction /* dir */, AFN::TargetOs os,
                      bool bluetoothHfp, int preferredRateOverride)
{
    AFN::DeviceCaps caps;

    if (dev.isNull()) {
        // Nothing to probe — treat as unknown so reliable backends fail cleanly
        // and probe-at-open backends still take their preferred rung.
        caps.isFormatSupportedReliable = (os != AFN::TargetOs::Windows);
        if (preferredRateOverride > 0) caps.preferredRate = preferredRateOverride;
        caps.isBluetoothHfp = bluetoothHfp;
        return caps;
    }

    const QAudioFormat pref = dev.preferredFormat();
    const int channels = pref.channelCount() >= 1 ? pref.channelCount() : 2;
    caps.channels = channels;
    caps.preferredRate = preferredRateOverride > 0 ? preferredRateOverride : pref.sampleRate();
    caps.preferredFormat = fromQt(pref.sampleFormat());
    caps.isBluetoothHfp = bluetoothHfp;

    // WASAPI's isFormatSupported() returns false-negatives for many valid
    // devices (Voicemeeter, FlexRadio DAX, shared-mix mismatches), so on Windows
    // the policy must probe-at-open instead of trusting the query (#2120/#2929).
    caps.isFormatSupportedReliable = (os != AFN::TargetOs::Windows);

    // Probe the candidate rates × the two modelled formats against the device.
    static const int kCandidateRates[] = {8000, 16000, 24000, 44100, 48000};
    const AFN::SampleFmt fmts[] = {AFN::SampleFmt::Float32, AFN::SampleFmt::Int16};

    QList<int> rates;
    QList<AFN::SampleFmt> supportedFmts;
    for (int rate : kCandidateRates) {
        bool rateOk = false;
        for (AFN::SampleFmt sf : fmts) {
            QAudioFormat test = makeFormat(rate, sf, channels);
            if (dev.isFormatSupported(test)) {
                rateOk = true;
                if (!supportedFmts.contains(sf)) supportedFmts.append(sf);
            }
        }
        if (rateOk) rates.append(rate);
    }

    // Always include the device's own preferred rate/format as supported — it is
    // by definition openable even when isFormatSupported() is conservative.
    if (caps.preferredRate > 0 && !rates.contains(caps.preferredRate)) {
        rates.append(caps.preferredRate);
    }
    if (!supportedFmts.contains(caps.preferredFormat)) {
        supportedFmts.append(caps.preferredFormat);
    }

    caps.supportedRates = rates;
    if (!supportedFmts.isEmpty()) {
        caps.supportedFormats = supportedFmts;
    }
    return caps;
}

Result negotiate(const QAudioDevice& dev, AFN::Direction dir, AFN::ResamplerPolicy policy,
                 AFN::TargetOs os, int internalRate, bool bluetoothHfp, AFN::FormatPreference pref)
{
    const AFN::DeviceCaps caps = probe(dev, dir, os, bluetoothHfp);
    const AFN::NegotiatedFormat n = AFN::negotiate(os, dir, caps, policy, internalRate, pref);

    Result r;
    r.ok = n.ok;
    r.resampler = n.resampler;
    r.fellBack = n.fellBack;
    r.reason = n.reason;
    if (n.ok) {
        r.format = makeFormat(n.rate, n.fmt, n.channels);
    }
    return r;
}

QList<QAudioFormat> formatLadder(const QAudioDevice& dev, AFN::Direction dir,
                                 AFN::ResamplerPolicy policy, AFN::TargetOs os,
                                 int internalRate, bool bluetoothHfp, int preferredRateOverride,
                                 AFN::FormatPreference pref)
{
    const AFN::DeviceCaps caps = probe(dev, dir, os, bluetoothHfp, preferredRateOverride);
    const QList<AFN::FormatCandidate> ladder =
        AFN::buildLadder(os, dir, caps, policy, internalRate, pref);

    QList<QAudioFormat> out;
    out.reserve(ladder.size());
    for (const AFN::FormatCandidate& c : ladder) {
        out.append(makeFormat(c.rate, c.fmt, c.channels));
    }
    return out;
}

} // namespace AudioDeviceNegotiator
} // namespace AetherSDR
