#include "core/tnc/AetherAx25LibmodemShim.h"
#include "core/tnc/Ax25.h"
#include "core/tnc/KissFraming.h"

#include "bitstream.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTemporaryFile>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace AetherSDR;
namespace lm = aether_libmodem_core;

namespace {

int g_failed = 0;
constexpr double kPi = 3.14159265358979323846;

struct ReplayAudio {
    std::vector<float> samples;
    int sampleRate{0};
};

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

QVector<quint8> toQtBits(const std::vector<uint8_t>& bits)
{
    QVector<quint8> out;
    out.reserve(static_cast<qsizetype>(bits.size()));
    for (uint8_t bit : bits)
        out.append(bit ? 1 : 0);
    return out;
}

lm::packet knownPacket()
{
    return lm::packet("N0CALL-9", "APRS",
                      {"WIDE1-1", "WIDE2-1"},
                      "hello world");
}

lm::packet fixedAprsTestPacket()
{
    return lm::packet("KI6BCJ-1", "APDW18",
                      {},
                      "!3644.00N\\11947.00W-KI6BCJ HF APRS test via Direwolf 300 baud");
}

lm::packet fixedVhf1200AprsTestPacket()
{
    return lm::packet("KK7GWY-9", "APDW18",
                      {"WIDE1-1", "WIDE2-1"},
                      "!4742.00N/12217.00W>2m APRS via AetherModem 1200 baud");
}

QByteArray sinePcm(int sampleRate, double frequencyHz, double amplitude)
{
    QByteArray pcm;
    pcm.resize(sampleRate * static_cast<int>(sizeof(float)));
    auto* samples = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < sampleRate; ++i) {
        samples[i] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequencyHz * static_cast<double>(i)
                                 / static_cast<double>(sampleRate)));
    }
    return pcm;
}

std::vector<float> afskPcmFromBits(const std::vector<uint8_t>& bits,
                                   const Ax25DemodConfig& config,
                                   double amplitude = 0.5)
{
    const int samplesPerSymbol = config.sampleRate / config.baud;
    std::vector<float> samples;
    samples.reserve(bits.size() * static_cast<size_t>(samplesPerSymbol));

    double phase = 0.0;
    for (uint8_t bit : bits) {
        const double frequency = bit ? config.markHz : config.spaceHz;
        const double phaseStep = 2.0 * kPi * frequency / static_cast<double>(config.sampleRate);
        for (int i = 0; i < samplesPerSymbol; ++i) {
            samples.push_back(static_cast<float>(amplitude * std::sin(phase)));
            phase += phaseStep;
            if (phase >= 2.0 * kPi)
                phase -= 2.0 * kPi;
        }
    }
    return samples;
}

std::vector<float> monoFromStereoFloat32(const QByteArray& pcm)
{
    const auto* src = reinterpret_cast<const float*>(pcm.constData());
    const int samples = pcm.size() / static_cast<int>(sizeof(float));
    const int frames = samples / 2;
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        mono.push_back((src[i * 2] + src[i * 2 + 1]) * 0.5f);
    return mono;
}

quint16 readLe16(const char* bytes)
{
    return static_cast<quint16>(static_cast<unsigned char>(bytes[0]))
        | (static_cast<quint16>(static_cast<unsigned char>(bytes[1])) << 8);
}

quint32 readLe32(const char* bytes)
{
    return static_cast<quint32>(static_cast<unsigned char>(bytes[0]))
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(bytes[3])) << 24);
}

bool loadMonoFloatWav(const QString& path, ReplayAudio& audio, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (bytes.size() < 44
        || bytes.mid(0, 4) != QByteArrayLiteral("RIFF")
        || bytes.mid(8, 4) != QByteArrayLiteral("WAVE")) {
        error = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }

    quint16 format = 0;
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    int sampleRate = 0;
    const char* data = nullptr;
    qsizetype dataBytes = 0;

    qsizetype pos = 12;
    while (pos + 8 <= bytes.size()) {
        const QByteArray chunkId = bytes.mid(pos, 4);
        const quint32 chunkBytes = readLe32(bytes.constData() + pos + 4);
        pos += 8;
        if (pos + static_cast<qsizetype>(chunkBytes) > bytes.size()) {
            error = QStringLiteral("truncated WAVE chunk");
            return false;
        }

        if (chunkId == QByteArrayLiteral("fmt ")) {
            if (chunkBytes < 16) {
                error = QStringLiteral("short fmt chunk");
                return false;
            }
            const char* fmt = bytes.constData() + pos;
            format = readLe16(fmt);
            channels = readLe16(fmt + 2);
            sampleRate = static_cast<int>(readLe32(fmt + 4));
            bitsPerSample = readLe16(fmt + 14);
        } else if (chunkId == QByteArrayLiteral("data")) {
            data = bytes.constData() + pos;
            dataBytes = static_cast<qsizetype>(chunkBytes);
        }

        pos += static_cast<qsizetype>(chunkBytes);
        if (chunkBytes & 1u)
            ++pos;
    }

    if (format != 3 || channels != 1 || bitsPerSample != 32) {
        error = QStringLiteral("expected mono IEEE-float 32-bit WAV, got format=%1 channels=%2 bits=%3")
            .arg(format)
            .arg(channels)
            .arg(bitsPerSample);
        return false;
    }
    if (!data || dataBytes <= 0 || dataBytes % static_cast<qsizetype>(sizeof(float)) != 0) {
        error = QStringLiteral("missing or invalid data chunk");
        return false;
    }

    audio.sampleRate = sampleRate;
    audio.samples.resize(static_cast<size_t>(dataBytes / static_cast<qsizetype>(sizeof(float))));
    std::memcpy(audio.samples.data(), data, static_cast<size_t>(dataBytes));
    return true;
}

bool writeMonoFloatWavForTest(const QString& path, const std::vector<float>& samples, int sampleRate)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    auto writeAscii = [&file](const char* text) { file.write(text, 4); };
    auto writeU16 = [&file](quint16 value) {
        char bytes[2] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
        };
        file.write(bytes, sizeof(bytes));
    };
    auto writeU32 = [&file](quint32 value) {
        char bytes[4] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
            static_cast<char>((value >> 16) & 0xff),
            static_cast<char>((value >> 24) & 0xff),
        };
        file.write(bytes, sizeof(bytes));
    };

    const quint32 dataBytes = static_cast<quint32>(samples.size() * sizeof(float));
    writeAscii("RIFF");
    writeU32(36u + dataBytes);
    writeAscii("WAVE");
    writeAscii("fmt ");
    writeU32(16);
    writeU16(3);
    writeU16(1);
    writeU32(static_cast<quint32>(sampleRate));
    writeU32(static_cast<quint32>(sampleRate * sizeof(float)));
    writeU16(static_cast<quint16>(sizeof(float)));
    writeU16(32);
    writeAscii("data");
    writeU32(dataBytes);
    file.write(reinterpret_cast<const char*>(samples.data()), static_cast<qint64>(dataBytes));
    return file.error() == QFileDevice::NoError;
}

void printReplayDiagnostics(const char* label,
                            const Ax25DecoderDiagnostics& diagnostics,
                            qsizetype frameCount)
{
    std::printf(
        "%s: frames=%lld rms=%.1f dBFS peak=%.1f dBFS clip=%.2f%% "
        "tone%.0f=%.1f dBFS tone%.0f=%.1f dBFS dTone=%.1f dB "
        "gate=%s gateRms=%.1f dBFS gateFloor=%.1f dBFS gateResets=%llu "
        "lanes=%d symbols=%d conf=%.2f ones=%.1f%% starts=%llu hdlc=%llu ax25=%llu ok=%llu reject=%llu "
        "short=%llu badFcs=%llu malformed=%llu last=%s bytes=%d bits=%d fcs=%s/%s\n",
        label,
        static_cast<long long>(frameCount),
        diagnostics.rmsDbfs,
        diagnostics.peakDbfs,
        diagnostics.clippedPercent,
        diagnostics.markToneHz,
        diagnostics.markToneDbfs,
        diagnostics.spaceToneHz,
        diagnostics.spaceToneDbfs,
        diagnostics.markMinusSpaceDb,
        diagnostics.receiveGateOpen ? "open" : "idle",
        diagnostics.receiveGateRmsDbfs,
        diagnostics.receiveGateFloorDbfs,
        static_cast<unsigned long long>(diagnostics.receiveGateResets),
        diagnostics.decodeLanes,
        diagnostics.demodSymbols,
        diagnostics.averageConfidence,
        diagnostics.onesPercent,
        static_cast<unsigned long long>(diagnostics.hdlcFrameStarts),
        static_cast<unsigned long long>(diagnostics.hdlcFrameCandidates),
        static_cast<unsigned long long>(diagnostics.plausibleAx25Candidates),
        static_cast<unsigned long long>(diagnostics.framesAccepted),
        static_cast<unsigned long long>(diagnostics.decodeRejected),
        static_cast<unsigned long long>(diagnostics.rejectTooShort),
        static_cast<unsigned long long>(diagnostics.rejectBadFcs),
        static_cast<unsigned long long>(diagnostics.rejectMalformed),
        qPrintable(diagnostics.lastRejectReason),
        diagnostics.lastRejectFrameBytes,
        diagnostics.lastRejectFrameBits,
        qPrintable(diagnostics.lastRejectActualFcs.isEmpty() ? QStringLiteral("-") : diagnostics.lastRejectActualFcs),
        qPrintable(diagnostics.lastRejectExpectedFcs.isEmpty() ? QStringLiteral("-") : diagnostics.lastRejectExpectedFcs));
}

int replayCapture(const QString& path)
{
    ReplayAudio audio;
    QString error;
    if (!loadMonoFloatWav(path, audio, error)) {
        std::fprintf(stderr, "Replay load failed: %s\n", qPrintable(error));
        return 2;
    }

    std::printf("Replaying %s: %zu mono float samples at %d Hz\n",
                qPrintable(path),
                audio.samples.size(),
                audio.sampleRate);

    // Sweep every profile and polarity so a single capture (e.g. a real 2m
    // APRS recording) is scored against HF 300 and VHF 1200 in one pass.
    for (Ax25ModemProfile profile : {Ax25ModemProfile::Hf300, Ax25ModemProfile::Vhf1200}) {
        for (Ax25TonePolarity polarity : {Ax25TonePolarity::Normal, Ax25TonePolarity::Inverted}) {
            AetherAx25LibmodemShim shim;
            shim.configure(ax25DemodConfigForProfile(profile, polarity));
            QVector<Ax25DecodedFrame> frames;
            QVector<double> frameTimesSeconds;
            constexpr int chunkSamples = 1024;
            for (size_t offset = 0; offset < audio.samples.size(); offset += chunkSamples) {
                const int count = static_cast<int>(
                    std::min<size_t>(chunkSamples, audio.samples.size() - offset));
                const auto chunkFrames = shim.processMonoFloat(audio.samples.data() + offset,
                                                               count,
                                                               audio.sampleRate);
                frames += chunkFrames;
                for (qsizetype i = 0; i < chunkFrames.size(); ++i) {
                    frameTimesSeconds.append(
                        static_cast<double>(offset + static_cast<size_t>(count))
                        / static_cast<double>(audio.sampleRate));
                }
            }
            const auto diagnostics = shim.diagnosticsSnapshot();
            const QString label = QStringLiteral("%1 %2")
                .arg(ax25ModemProfileName(profile),
                     polarity == Ax25TonePolarity::Normal
                         ? QStringLiteral("Normal")
                         : QStringLiteral("Reverse"));
            printReplayDiagnostics(qPrintable(label),
                                   diagnostics,
                                   frames.size());
            for (qsizetype i = 0; i < frames.size(); ++i) {
                const auto& frame = frames.at(i);
                std::printf("  %.1fs phase=%d %s > %s%s  %s\n",
                            frameTimesSeconds.value(i, 0.0),
                            frame.decodePhaseOffsetSamples,
                            qPrintable(frame.source),
                            qPrintable(frame.destination),
                            qPrintable(frame.path.isEmpty()
                                ? QString()
                                : QStringLiteral(",") + frame.path.join(QStringLiteral(","))),
                            qPrintable(frame.payloadText.isEmpty() ? frame.payloadHex : frame.payloadText));
            }
        }
    }

    return 0;
}

void testConstructsWithHf300Config()
{
    AetherAx25LibmodemShim shim;
    const auto cfg = shim.config();
    report("default profile is HF 300", cfg.profile == Ax25ModemProfile::Hf300);
    report("default sample rate", cfg.sampleRate == 24000);
    report("default baud", cfg.baud == 300);
    report("default tones", cfg.markHz == 1600.0 && cfg.spaceHz == 1800.0);
    report("HF profile uses phase diversity lanes", shim.diagnosticsSnapshot().decodeLanes == 21);
}

void testVhf1200ProfileConfig()
{
    AetherAx25LibmodemShim shim;
    shim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    const auto cfg = shim.config();
    report("VHF profile is retained", cfg.profile == Ax25ModemProfile::Vhf1200);
    report("VHF sample rate", cfg.sampleRate == 24000);
    report("VHF baud", cfg.baud == 1200);
    report("VHF tones", cfg.markHz == 1200.0 && cfg.spaceHz == 2200.0);
    // Default VhfMode::APlus = 9 slicers (Direwolf A+ profile).
    report("VHF profile default APlus: 9 lanes",
           shim.diagnosticsSnapshot().decodeLanes == 9);
    report("VHF description names profile", shim.demodDescription().contains(QStringLiteral("1200 baud VHF")));
}

void testKnownGoodBitstreamDecodes()
{
    AetherAx25LibmodemShim shim;
    lm::ax25_bitstream_converter converter;
    const auto bits = converter.encode(knownPacket(), 6, 2);
    const auto frames = shim.processRecoveredBitsForTest(toQtBits(bits));
    const auto diagnostics = shim.diagnosticsSnapshot();

    report("known-good AX.25 bitstream emits one frame", frames.size() == 1);
    report("known-good AX.25 bitstream records one HDLC start", diagnostics.hdlcFrameStarts == 1);
    report("known-good AX.25 bitstream records one HDLC candidate", diagnostics.hdlcFrameCandidates == 1);
    report("known-good AX.25 bitstream records one AX.25-like candidate", diagnostics.plausibleAx25Candidates == 1);
    if (frames.isEmpty())
        return;
    const auto& frame = frames.first();
    report("decoded source", frame.source == QStringLiteral("N0CALL-9"));
    report("decoded destination", frame.destination == QStringLiteral("APRS"));
    report("decoded path", frame.path == QStringList({QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")}));
    report("decoded UI frame", frame.isUiFrame && frame.control == 0x03 && frame.pid == 0xf0);
    report("decoded payload", frame.payloadText == QStringLiteral("hello world"));
    report("decoded FCS accepted", frame.fcsOk);
}

void testSyntheticHf300AfskLoopbackDecodes()
{
    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Hf300);

    AetherAx25LibmodemShim shim;
    shim.configure(cfg);

    const auto frameBytes = lm::ax25::encode_frame(fixedAprsTestPacket());
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 0, 80, 8);
    const auto audio = afskPcmFromBits(bits, cfg);
    const auto frames = shim.processMonoFloat(audio.data(),
                                              static_cast<int>(audio.size()),
                                              cfg.sampleRate);
    const auto diagnostics = shim.diagnosticsSnapshot();

    report("synthetic 300 baud AFSK loopback emits one frame", frames.size() == 1);
    report("synthetic 300 baud AFSK loopback has no clipping", diagnostics.clippedPercent == 0.0);
    report("synthetic 300 baud AFSK loopback produced symbols", diagnostics.demodSymbols > 0);
    if (frames.isEmpty())
        return;

    const auto& frame = frames.first();
    report("synthetic loopback source", frame.source == QStringLiteral("KI6BCJ-1"));
    report("synthetic loopback destination", frame.destination == QStringLiteral("APDW18"));
    report("synthetic loopback has no path", frame.path.isEmpty());
    report("synthetic loopback UI frame", frame.isUiFrame && frame.control == 0x03 && frame.pid == 0xf0);
    report("synthetic loopback payload",
           frame.payloadText == QStringLiteral("!3644.00N\\11947.00W-KI6BCJ HF APRS test via Direwolf 300 baud"));
}

void testSyntheticVhf1200AfskLoopbackDecodes()
{
    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200);

    AetherAx25LibmodemShim shim;
    shim.configure(cfg);

    const auto frameBytes = lm::ax25::encode_frame(fixedVhf1200AprsTestPacket());
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 0, 80, 8);
    const auto audio = afskPcmFromBits(bits, cfg);
    const auto frames = shim.processMonoFloat(audio.data(),
                                              static_cast<int>(audio.size()),
                                              cfg.sampleRate);
    const auto diagnostics = shim.diagnosticsSnapshot();

    report("synthetic 1200 baud AFSK loopback emits one frame", frames.size() == 1);
    report("synthetic 1200 baud AFSK loopback runs the decode bank", diagnostics.decodeLanes > 1);
    report("synthetic 1200 baud AFSK loopback produced symbols", diagnostics.demodSymbols > 0);
    report("synthetic 1200 baud AFSK loopback has no clipping", diagnostics.clippedPercent == 0.0);
    if (frames.isEmpty())
        return;

    const auto& frame = frames.first();
    report("synthetic 1200 loopback source", frame.source == QStringLiteral("KK7GWY-9"));
    report("synthetic 1200 loopback destination", frame.destination == QStringLiteral("APDW18"));
    report("synthetic 1200 loopback path",
           frame.path == QStringList({QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")}));
    report("synthetic 1200 loopback UI frame", frame.isUiFrame && frame.control == 0x03 && frame.pid == 0xf0);
    report("synthetic 1200 loopback payload",
           frame.payloadText == QStringLiteral("!4742.00N/12217.00W>2m APRS via AetherModem 1200 baud"));
}

// A connect request (SABM) is the SHORTEST possible AX.25 frame: dest(7) +
// src(7) + control(1) = 15 bytes, no PID/info, = 17 bytes with FCS. That is the
// worst case for the decoder's minimum-length gate, and it is exactly what a TNC
// sends to open a PMS session.
//
// Regression guard: the bitstream decoder previously rejected frames < 18 bytes,
// which silently dropped every no-PID U-frame (SABM/DISC/UA/DM) at exactly 17 —
// so a PMS/BBS could never be reached in connected mode even with perfect audio.
// The gate is now < 17. This test fails if that off-by-one ever returns.
void testSabmConnectFrameLoopbackDecodes()
{
    using AetherSDR::ax25::Address;
    using AetherSDR::ax25::Frame;
    using AetherSDR::ax25::FrameType;

    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200);
    AetherAx25LibmodemShim shim;
    shim.configure(cfg);

    const Address dest{QStringLiteral("KI6BCJ"), 10, false, false};
    const Address src{QStringLiteral("KI6BCJ"), 7, false, false};
    const Frame sabm = Frame::makeU(dest, src, FrameType::SABM,
                                    /*pollFinal=*/true, /*command=*/true);
    const QByteArray sabmNoFcs = sabm.encode();
    report("SABM encodes to the minimum 15-byte frame", sabmNoFcs.size() == 15);

    const auto tx = shim.buildTransmitAudioFromFrame(sabmNoFcs);
    report("SABM TX builds audio", tx.ok && !tx.stereoFloat32Pcm.isEmpty());
    if (!tx.ok)
        return;

    // One-shot feed, same as the other synthetic loopback tests — the SABM now
    // decodes directly once the < 17 length gate is correct.
    const auto monoTx = monoFromStereoFloat32(tx.stereoFloat32Pcm);
    const auto frames = shim.processMonoFloat(monoTx.data(),
                                              static_cast<int>(monoTx.size()),
                                              cfg.sampleRate);
    report("SABM loopback emits one frame", frames.size() == 1);
    if (frames.isEmpty())
        return;
    const auto& frame = frames.first();
    report("SABM loopback FCS accepted", frame.fcsOk);
    report("SABM loopback is not a UI frame", !frame.isUiFrame);
    // Control 0x2F (SABM) with the poll bit (0x10) set = 0x3F.
    report("SABM loopback control byte is SABM+P", frame.control == 0x3F);
    report("SABM loopback destination", frame.destination == QStringLiteral("KI6BCJ-10"));
    report("SABM loopback source", frame.source == QStringLiteral("KI6BCJ-7"));

    // And confirm our own decoder parses the recovered on-air bytes back to a
    // SABM, i.e. the full path a caller's connect takes into the PMS data link.
    const auto parsed = Frame::decode(frame.ax25FrameNoFcs);
    report("SABM loopback re-parses as a Frame", parsed.has_value());
    if (parsed)
        report("SABM loopback parses as FrameType::SABM", parsed->type == FrameType::SABM);
}

void testChunkedVhf1200ReplayDecodes()
{
    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200);
    const auto frameBytes = lm::ax25::encode_frame(fixedVhf1200AprsTestPacket());
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 0, 80, 8);
    const auto packetAudio = afskPcmFromBits(bits, cfg);

    // Near-silent lead-in so the receive gate has a floor to open against, then
    // the burst, fed in 1024-sample chunks exactly like the live RX audio tap.
    std::vector<float> audio(static_cast<size_t>(cfg.sampleRate), 0.002f);
    audio.insert(audio.end(), packetAudio.begin(), packetAudio.end());

    AetherAx25LibmodemShim shim;
    shim.configure(cfg);

    QVector<Ax25DecodedFrame> frames;
    constexpr int chunkSamples = 1024;
    for (size_t offset = 0; offset < audio.size(); offset += chunkSamples) {
        const int count = static_cast<int>(
            std::min<size_t>(chunkSamples, audio.size() - offset));
        frames += shim.processMonoFloat(audio.data() + offset, count, cfg.sampleRate);
    }

    const auto diagnostics = shim.diagnosticsSnapshot();
    report("chunked 1200 replay receive gate opened", diagnostics.receiveGateResets > 0);
    report("chunked 1200 replay emits one frame", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("chunked 1200 replay source", frames.first().source == QStringLiteral("KK7GWY-9"));
    report("chunked 1200 replay path",
           frames.first().path == QStringList({QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")}));
    report("chunked 1200 replay payload",
           frames.first().payloadText == QStringLiteral("!4742.00N/12217.00W>2m APRS via AetherModem 1200 baud"));
}

void testTransmitRawPayloadBuildsLoopbackAudio()
{
    AetherAx25LibmodemShim txShim;
    txShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));

    const Ax25TransmitResult tx = txShim.buildTransmitAudio(
        QStringLiteral("hello from AetherModem"),
        QStringLiteral("N0CALL-9"));
    report("TX raw payload packetizes", tx.ok);
    if (!tx.ok)
        return;
    report("TX raw source", tx.frame.source == QStringLiteral("N0CALL-9"));
    report("TX raw destination defaults APRS", tx.frame.destination == QStringLiteral("APRS"));
    report("TX raw payload preserved", tx.frame.payloadText == QStringLiteral("hello from AetherModem"));
    report("TX raw stereo PCM generated", !tx.stereoFloat32Pcm.isEmpty() && tx.audioFrames > 0);
    report("TX raw audio padded to VITA packet frames", (tx.audioFrames % tx.vitaPacketFrames) == 0);
    report("TX raw waveform has sane level", tx.peakDbfs < -6.0 && tx.peakDbfs > -12.0);

    const std::vector<float> mono = monoFromStereoFloat32(tx.stereoFloat32Pcm);
    AetherAx25LibmodemShim rxShim;
    rxShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));
    const auto frames = rxShim.processMonoFloat(mono.data(),
                                                static_cast<int>(mono.size()),
                                                tx.sampleRate);
    report("TX raw loopback decodes", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("TX raw loopback source", frames.first().source == QStringLiteral("N0CALL-9"));
    report("TX raw loopback payload", frames.first().payloadText == QStringLiteral("hello from AetherModem"));
}

void testTransmitMonitorSyntaxBuildsLoopbackAudio()
{
    AetherAx25LibmodemShim txShim;
    txShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));

    const Ax25TransmitResult tx = txShim.buildTransmitAudio(
        QStringLiteral("KI6BCJ-1>APDW18,WIDE1-1:!3644.00N\\11947.00W-Test TX"),
        QStringLiteral("N0CALL"));
    report("TX monitor syntax packetizes", tx.ok);
    if (!tx.ok)
        return;
    report("TX monitor source", tx.frame.source == QStringLiteral("KI6BCJ-1"));
    report("TX monitor destination", tx.frame.destination == QStringLiteral("APDW18"));
    report("TX monitor path", tx.frame.path == QStringList({QStringLiteral("WIDE1-1")}));

    const std::vector<float> mono = monoFromStereoFloat32(tx.stereoFloat32Pcm);
    AetherAx25LibmodemShim rxShim;
    rxShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));
    const auto frames = rxShim.processMonoFloat(mono.data(),
                                                static_cast<int>(mono.size()),
                                                tx.sampleRate);
    report("TX monitor loopback decodes", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("TX monitor loopback destination", frames.first().destination == QStringLiteral("APDW18"));
    report("TX monitor loopback path", frames.first().path == QStringList({QStringLiteral("WIDE1-1")}));
    report("TX monitor loopback payload", frames.first().payloadText == QStringLiteral("!3644.00N\\11947.00W-Test TX"));
}

void testTransmitVhf1200LoopbackDecodes()
{
    AetherAx25LibmodemShim txShim;
    txShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));

    const Ax25TransmitResult tx = txShim.buildTransmitAudio(
        QStringLiteral("KK7GWY-9>APDW18,WIDE1-1:!4742.00N/12217.00W>2m APRS TX"),
        QStringLiteral("KK7GWY"));
    report("VHF 1200 TX packetizes", tx.ok);
    if (!tx.ok)
        return;
    report("VHF 1200 TX reports 1200 baud", tx.baud == 1200);
    report("VHF 1200 TX uses Bell 202 tones", tx.markHz == 1200.0 && tx.spaceHz == 2200.0);
    report("VHF 1200 TX stereo PCM generated", !tx.stereoFloat32Pcm.isEmpty() && tx.audioFrames > 0);
    report("VHF 1200 TX padded to VITA packet frames", (tx.audioFrames % tx.vitaPacketFrames) == 0);
    report("VHF 1200 TX waveform has sane level", tx.peakDbfs < -6.0 && tx.peakDbfs > -12.0);

    // TXDELAY optimization: VHF 1200 emits a shorter preamble than HF 300.
    AetherAx25LibmodemShim hfShim;
    hfShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));
    const Ax25TransmitResult hfTx = hfShim.buildTransmitAudio(
        QStringLiteral("hello"), QStringLiteral("KK7GWY"));
    report("VHF 1200 TX uses a shorter preamble than HF 300",
           hfTx.ok && tx.preambleFlags > 0 && tx.preambleFlags < hfTx.preambleFlags);

    const std::vector<float> mono = monoFromStereoFloat32(tx.stereoFloat32Pcm);
    AetherAx25LibmodemShim rxShim;
    rxShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    const auto frames = rxShim.processMonoFloat(mono.data(),
                                                static_cast<int>(mono.size()),
                                                tx.sampleRate);
    report("VHF 1200 TX loopback decodes", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("VHF 1200 TX loopback source", frames.first().source == QStringLiteral("KK7GWY-9"));
    report("VHF 1200 TX loopback destination", frames.first().destination == QStringLiteral("APDW18"));
    report("VHF 1200 TX loopback path",
           frames.first().path == QStringList({QStringLiteral("WIDE1-1")}));
    report("VHF 1200 TX loopback payload",
           frames.first().payloadText == QStringLiteral("!4742.00N/12217.00W>2m APRS TX"));
}

void testKissFramingRoundTrip()
{
    namespace k = AetherSDR::kiss;

    // Payload deliberately contains FEND (0xC0) and FESC (0xDB) so escaping runs.
    QByteArray ax25;
    ax25.append(static_cast<char>(0xC0));
    ax25.append(static_cast<char>(0x01));
    ax25.append(static_cast<char>(0xDB));
    ax25.append("HELLO");

    const QByteArray framed = k::encodeDataFrame(ax25);
    report("KISS frame is delimited by FEND",
           !framed.isEmpty()
           && static_cast<quint8>(framed.front()) == k::kFend
           && static_cast<quint8>(framed.back()) == k::kFend);
    report("KISS framing escaped the special bytes", framed.size() > ax25.size() + 3);

    k::Decoder decoder;
    const QVector<QByteArray> frames = decoder.feed(framed);
    report("KISS decoder yields exactly one frame", frames.size() == 1);
    if (!frames.isEmpty()) {
        quint8 port = 9;
        quint8 command = 9;
        QByteArray payload;
        k::splitTypeByte(frames.first(), port, command, payload);
        report("KISS decoded a data command on port 0", port == 0 && command == k::kCmdData);
        report("KISS payload round-trips through escape/unescape", payload == ax25);
    }

    // Feeding one byte at a time must reassemble identically (TCP can split).
    k::Decoder dripDecoder;
    QVector<QByteArray> dripFrames;
    for (int i = 0; i < framed.size(); ++i)
        dripFrames += dripDecoder.feed(framed.mid(i, 1));
    bool dripOk = dripFrames.size() == 1;
    if (dripOk) {
        quint8 p = 0;
        quint8 c = 0;
        QByteArray pl;
        k::splitTypeByte(dripFrames.first(), p, c, pl);
        dripOk = (pl == ax25);
    }
    report("KISS decoder reassembles across byte-split feeds", dripOk);
}

void testKissTxFromFrameLoopbackDecodes()
{
    AetherAx25LibmodemShim txShim;
    txShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));

    // A KISS data frame carries a raw AX.25 frame with no FCS: encode a packet
    // and drop the trailing 2-byte FCS to simulate what a host app would send.
    const std::vector<uint8_t> frameWithFcs = lm::ax25::encode_frame(fixedVhf1200AprsTestPacket());
    const QByteArray ax25NoFcs(reinterpret_cast<const char*>(frameWithFcs.data()),
                               static_cast<qsizetype>(frameWithFcs.size() - 2));

    const Ax25TransmitResult tx = txShim.buildTransmitAudioFromFrame(ax25NoFcs);
    report("KISS TX-from-frame packetizes", tx.ok);
    if (!tx.ok)
        return;
    report("KISS TX-from-frame is 1200 baud", tx.baud == 1200);
    report("KISS TX-from-frame appended FCS", tx.frameBytes == ax25NoFcs.size() + 2);

    const std::vector<float> mono = monoFromStereoFloat32(tx.stereoFloat32Pcm);
    AetherAx25LibmodemShim rxShim;
    rxShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Vhf1200));
    const auto frames = rxShim.processMonoFloat(mono.data(),
                                                static_cast<int>(mono.size()),
                                                tx.sampleRate);
    report("KISS TX-from-frame loopback decodes one frame", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("KISS TX loopback source", frames.first().source == QStringLiteral("KK7GWY-9"));
    report("KISS TX loopback payload",
           frames.first().payloadText
               == QStringLiteral("!4742.00N/12217.00W>2m APRS via AetherModem 1200 baud"));
    // RX raw-frame capture must round-trip back to exactly what we keyed.
    report("KISS RX raw frame bytes match the keyed frame",
           frames.first().ax25FrameNoFcs == ax25NoFcs);
}

void testMalformedTransmitMonitorSyntaxIsRejected()
{
    AetherAx25LibmodemShim txShim;
    txShim.configure(ax25DemodConfigForProfile(Ax25ModemProfile::Hf300));

    const Ax25TransmitResult tx = txShim.buildTransmitAudio(
        QStringLiteral("KI6BCJ-123>APDW18:bad source"),
        QStringLiteral("N0CALL"));
    report("malformed TX monitor syntax is rejected", !tx.ok);
    report("malformed TX monitor syntax reports error",
           tx.error.contains(QStringLiteral("invalid monitor syntax"))
           || tx.error.contains(QStringLiteral("invalid AX.25 address")));
    report("malformed TX monitor syntax emits no audio", tx.stereoFloat32Pcm.isEmpty());
}

void testChunkedSyntheticReplayUsesReceiveGate()
{
    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Hf300);
    const auto frameBytes = lm::ax25::encode_frame(fixedAprsTestPacket());
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 0, 80, 8);
    const auto packetAudio = afskPcmFromBits(bits, cfg);

    std::vector<float> audio(static_cast<size_t>(cfg.sampleRate * 2), 0.002f);
    audio.insert(audio.end(), packetAudio.begin(), packetAudio.end());

    AetherAx25LibmodemShim shim;
    shim.configure(cfg);

    QVector<Ax25DecodedFrame> frames;
    constexpr int chunkSamples = 1024;
    for (size_t offset = 0; offset < audio.size(); offset += chunkSamples) {
        const int count = static_cast<int>(
            std::min<size_t>(chunkSamples, audio.size() - offset));
        frames += shim.processMonoFloat(audio.data() + offset, count, cfg.sampleRate);
    }

    const auto diagnostics = shim.diagnosticsSnapshot();
    report("chunked replay receive gate opened", diagnostics.receiveGateResets > 0);
    report("chunked replay emits one frame", frames.size() == 1);
    if (frames.isEmpty())
        return;
    report("chunked replay payload",
           frames.first().payloadText == QStringLiteral("!3644.00N\\11947.00W-KI6BCJ HF APRS test via Direwolf 300 baud"));
}

void testReplayWavLoaderFeedsShim()
{
    const auto cfg = ax25DemodConfigForProfile(Ax25ModemProfile::Hf300);
    const auto frameBytes = lm::ax25::encode_frame(fixedAprsTestPacket());
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 0, 80, 8);
    const auto audio = afskPcmFromBits(bits, cfg);

    QTemporaryFile file;
    file.setFileTemplate(QStringLiteral("aether-ax25-capture-XXXXXX.wav"));
    const bool opened = file.open();
    report("temporary replay WAV opened", opened);
    if (!opened)
        return;
    const QString path = file.fileName();
    file.close();

    report("temporary replay WAV written", writeMonoFloatWavForTest(path, audio, cfg.sampleRate));

    ReplayAudio loaded;
    QString error;
    report("temporary replay WAV loaded", loadMonoFloatWav(path, loaded, error));
    report("temporary replay WAV sample rate", loaded.sampleRate == cfg.sampleRate);
    report("temporary replay WAV sample count", loaded.samples.size() == audio.size());

    AetherAx25LibmodemShim shim;
    shim.configure(cfg);
    const auto frames = shim.processMonoFloat(loaded.samples.data(),
                                              static_cast<int>(loaded.samples.size()),
                                              loaded.sampleRate);
    report("temporary replay WAV decodes", frames.size() == 1);
}

void testBadFcsDoesNotEmit()
{
    AetherAx25LibmodemShim shim;
    std::vector<uint8_t> frameBytes = lm::ax25::encode_frame(knownPacket());
    if (!frameBytes.empty())
        frameBytes.back() ^= 0x40u;
    const auto bits = lm::ax25::encode_bitstream(frameBytes, 6, 2);
    const auto frames = shim.processRecoveredBitsForTest(toQtBits(bits));
    const auto diagnostics = shim.diagnosticsSnapshot();
    report("bad-FCS AX.25 bitstream emits no valid frames", frames.isEmpty());
    report("bad-FCS AX.25 bitstream records one HDLC start", diagnostics.hdlcFrameStarts == 1);
    report("bad-FCS AX.25 bitstream records one HDLC candidate", diagnostics.hdlcFrameCandidates == 1);
    report("bad-FCS AX.25 bitstream records one AX.25-like candidate", diagnostics.plausibleAx25Candidates == 1);
    report("bad-FCS AX.25 bitstream is classified", diagnostics.rejectBadFcs == 1);
    report("bad-FCS diagnostics preserve expected FCS",
           diagnostics.lastRejectReason.contains(QStringLiteral("bad-fcs"), Qt::CaseInsensitive)
           && !diagnostics.lastRejectActualFcs.isEmpty()
           && !diagnostics.lastRejectExpectedFcs.isEmpty());
}

void testTooShortRejectDiagnostics()
{
    AetherAx25LibmodemShim shim;
    const std::vector<uint8_t> shortFrame = { 0x13, 0x37 };
    const auto bits = lm::ax25::encode_bitstream(shortFrame, 3, 2);
    const auto frames = shim.processRecoveredBitsForTest(toQtBits(bits));
    const auto diagnostics = shim.diagnosticsSnapshot();

    report("too-short HDLC candidate emits no valid frames", frames.isEmpty());
    report("too-short HDLC candidate records one HDLC start", diagnostics.hdlcFrameStarts == 1);
    report("too-short HDLC candidate records one HDLC candidate", diagnostics.hdlcFrameCandidates == 1);
    report("too-short HDLC candidate is not AX.25-like", diagnostics.plausibleAx25Candidates == 0);
    report("too-short HDLC candidate is classified", diagnostics.rejectTooShort == 1);
    report("too-short diagnostics include byte preview",
           diagnostics.lastRejectFrameBytes == 2
           && diagnostics.lastRejectPreviewHex == QStringLiteral("13 37"));
}

void testTonePolarityConfig()
{
    AetherAx25LibmodemShim shim;
    const QString normal = shim.demodDescription();

    auto cfg = shim.config();
    cfg.polarity = Ax25TonePolarity::Inverted;
    shim.configure(cfg);
    const QString inverted = shim.demodDescription();

    report("normal and inverted demod setup are distinct", normal != inverted);
    report("inverted config retained", shim.config().polarity == Ax25TonePolarity::Inverted);
}

void testSampleRateMismatchIsIgnored()
{
    AetherAx25LibmodemShim shim;
    const float samples[8] = {};
    const auto frames = shim.processMonoFloat(samples, 8, 48000);
    report("non-24k sample-rate input is ignored for now", frames.isEmpty());
}

void testToneDiagnosticsSeparateMarkAndSpace()
{
    constexpr int sampleRate = 24000;

    AetherAx25LibmodemShim markShim;
    Ax25DecoderDiagnostics markDiagnostics;
    bool sawMarkDiagnostics = false;
    QObject::connect(&markShim, &AetherAx25LibmodemShim::diagnosticsUpdated,
                     [&markDiagnostics, &sawMarkDiagnostics](const Ax25DecoderDiagnostics& diagnostics) {
        markDiagnostics = diagnostics;
        sawMarkDiagnostics = true;
    });
    markShim.feedAudio(sinePcm(sampleRate, 1600.0, 0.5), sampleRate);
    report("mark tone diagnostics emitted", sawMarkDiagnostics);
    report("1600 Hz mark tone dominates diagnostics", markDiagnostics.markMinusSpaceDb > 15.0);
    report("1600 Hz mark tone level is plausible",
           markDiagnostics.markToneDbfs > -8.0 && markDiagnostics.markToneDbfs < -4.0);

    AetherAx25LibmodemShim spaceShim;
    Ax25DecoderDiagnostics spaceDiagnostics;
    bool sawSpaceDiagnostics = false;
    QObject::connect(&spaceShim, &AetherAx25LibmodemShim::diagnosticsUpdated,
                     [&spaceDiagnostics, &sawSpaceDiagnostics](const Ax25DecoderDiagnostics& diagnostics) {
        spaceDiagnostics = diagnostics;
        sawSpaceDiagnostics = true;
    });
    spaceShim.feedAudio(sinePcm(sampleRate, 1800.0, 0.5), sampleRate);
    report("space tone diagnostics emitted", sawSpaceDiagnostics);
    report("1800 Hz space tone dominates diagnostics", spaceDiagnostics.markMinusSpaceDb < -15.0);
    report("1800 Hz space tone level is plausible",
           spaceDiagnostics.spaceToneDbfs > -8.0 && spaceDiagnostics.spaceToneDbfs < -4.0);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QStringList args = app.arguments();
    const int replayArg = args.indexOf(QStringLiteral("--replay-capture"));
    if (replayArg >= 0) {
        if (replayArg + 1 >= args.size()) {
            std::fprintf(stderr, "Usage: %s --replay-capture <mono-float32-wav>\n", argv[0]);
            return 2;
        }
        return replayCapture(args.at(replayArg + 1));
    }

    testConstructsWithHf300Config();
    testVhf1200ProfileConfig();
    testKnownGoodBitstreamDecodes();
    testSyntheticHf300AfskLoopbackDecodes();
    testSyntheticVhf1200AfskLoopbackDecodes();
    testSabmConnectFrameLoopbackDecodes();
    testChunkedVhf1200ReplayDecodes();
    testTransmitRawPayloadBuildsLoopbackAudio();
    testTransmitMonitorSyntaxBuildsLoopbackAudio();
    testTransmitVhf1200LoopbackDecodes();
    testKissFramingRoundTrip();
    testKissTxFromFrameLoopbackDecodes();
    testMalformedTransmitMonitorSyntaxIsRejected();
    testChunkedSyntheticReplayUsesReceiveGate();
    testReplayWavLoaderFeedsShim();
    testBadFcsDoesNotEmit();
    testTooShortRejectDiagnostics();
    testTonePolarityConfig();
    testSampleRateMismatchIsIgnored();
    testToneDiagnosticsSeparateMarkAndSpace();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
