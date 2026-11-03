// Feed production VITA ingest directly. No init/start, socket, device peer or TX.
#include "core/backends/flex/PanadapterStream.h"
#include "core/OpusCodec.h"

#include <QCoreApplication>
#include <QtEndian>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdio>
#include <cstring>
#include <vector>

namespace AetherSDR {
class VitaSequenceLossLogTestAccess {
public:
    static void feed(PanadapterStream& stream, const QByteArray& packet, qint64 nowMs)
    {
        stream.m_rxReorderNowForTest = nowMs;
        stream.processDatagram(packet);
    }
    static void flush(PanadapterStream& stream, qint64 nowMs)
    {
        stream.m_rxReorderNowForTest = nowMs;
        stream.flushRxReorder();
    }
    static bool socketFree(const PanadapterStream& stream) { return stream.m_socket == nullptr; }
    static qsizetype queuedBytes(const PanadapterStream& stream) { return stream.m_queuedAudioBytes; }
};
}
using namespace AetherSDR;
namespace {
constexpr quint32 kStream = 0x04000008u;
int failures = 0;
void check(bool condition, const char* label)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }
}
QByteArray packet(int index, quint16 pcc, const QByteArray& payload)
{
    QByteArray out(28, '\0');
    auto* raw = reinterpret_cast<uchar*>(out.data());
    qToBigEndian<quint32>(0x38500000u | (quint32(index & 15) << 16), raw);
    qToBigEndian<quint32>(kStream, raw + 4);
    qToBigEndian<quint32>(pcc, raw + 12);
    if (pcc == 0x8005) {
        qToBigEndian<quint32>(1000 + index / 100, raw + 16);
        qToBigEndian<quint64>((index % 100) * 96, raw + 20);
    }
    out.append(payload);
    return out;
}
QByteArray pcm(int index, bool narrow)
{
    const float value = 0.125f + index / 1024.0f;
    QByteArray out(240 * (narrow ? 8 : 2), Qt::Uninitialized);
    auto* raw = reinterpret_cast<uchar*>(out.data());
    if (narrow) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, 4);
        for (int i = 0; i < 480; ++i) {
            qToBigEndian<quint32>(bits, raw + i * 4);
        }
    } else {
        for (int i = 0; i < 240; ++i) {
            qToBigEndian<qint16>(static_cast<qint16>(value * 32768), raw + i * 2);
        }
    }
    return out;
}
struct Run {
    QVector<float> samples;
    int losses = 0;
    int breaks = 0;
    int reordered = 0;
};
Run runAt(const std::vector<QByteArray>& packets,
          const std::vector<std::pair<int, qint64>>& arrivals, bool plc = true)
{
    PanadapterStream stream;
    stream.setPacketLossConcealment(plc);
    Run result;
    QObject::connect(&stream, &PanadapterStream::pcmFrameReady,
        [&result](const PcmFrame& frame) { result.samples += frame.samples(); });
    for (const auto& [index, nowMs] : arrivals) {
        VitaSequenceLossLogTestAccess::feed(stream, packets[index], nowMs);
    }
    VitaSequenceLossLogTestAccess::flush(stream, 100000);
    result.losses = stream.packetErrorCount();
    result.breaks = stream.packetSequenceBreakCount();
    result.reordered = stream.categoryStats(PanadapterStream::CatAudio).reordered;
    check(VitaSequenceLossLogTestAccess::socketFree(stream), "production ingest never opens a socket");
    check(VitaSequenceLossLogTestAccess::queuedBytes(stream) == 0, "the final deadline releases retained payloads");
    return result;
}
Run run(const std::vector<QByteArray>& packets, const std::vector<int>& order,
        bool plc = true, int lossTiming = 0)
{
    std::vector<std::pair<int, qint64>> arrivals;
    qint64 arrival = 0;
    for (int index : order) {
        arrivals.emplace_back(index, lossTiming ? index * 10 : arrival++ * 10);
    }
    return runAt(packets, arrivals, plc);
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::vector<int> ordered;
    for (int i = 0; i < 120; ++i) {
        ordered.push_back(i);
    }
    std::vector<int> swapped = ordered;
    for (int i : {15, 40, 61, 88}) {
        std::swap(swapped[i], swapped[i + 1]);
    }
    for (quint16 pcc : {quint16{0x03e3}, quint16{0x0123}, quint16{0x8005}}) {
        std::vector<QByteArray> input;
        OpusCodec encoder;
        if (pcc == 0x8005) {
            check(encoder.isValid(), "real Opus encoder/decoder support is enabled");
            if (!encoder.isValid()) {
                continue;
            }
        }
        for (int i = 0; i < 120; ++i) {
            QByteArray payload;
            if (pcc == 0x8005) {
                QByteArray tone(240 * 2 * sizeof(qint16), Qt::Uninitialized);
                auto* dst = reinterpret_cast<qint16*>(tone.data());
                for (int n = 0; n < 240; ++n) {
                    const qint16 sample = static_cast<qint16>(7000 * std::sin((i * 240 + n) * 0.13));
                    dst[2 * n] = dst[2 * n + 1] = sample;
                }
                payload = encoder.encode(tone);
                check(!payload.isEmpty(), "Opus source packet encodes");
            } else {
                payload = pcm(i, pcc == 0x03e3);
            }
            input.push_back(packet(i, pcc, payload));
        }
        const Run clean = run(input, ordered);
        const Run reordered = run(input, swapped);
        check(clean.samples.size() == 120 * 240 * 2, "ordered audio emits exactly the supplied frames");
        check(reordered.samples == clean.samples, "reordered audio is bit-identical to ordered decode, including Opus");
        check(reordered.losses == 0 && reordered.breaks == 0 && reordered.reordered == 4,
              "reorders do not create diagnostic loss or throttle gaps");
        std::vector<int> duplicate = ordered;
        duplicate.insert(duplicate.begin() + 42, 41);
        const Run repeated = run(input, duplicate);
        check(repeated.samples == clean.samples && repeated.losses == 0,
              "duplicate packets do not replay audio or request PLC");
        for (bool genuineLoss : {false, true}) {
            const std::vector<std::pair<int, qint64>> arrivals = genuineLoss
                ? std::vector<std::pair<int, qint64>>{{0, 0}, {2, 20}, {4, 45}, {3, 59}}
                : std::vector<std::pair<int, qint64>>{{0, 0}, {2, 20}, {4, 40}, {1, 49}, {3, 59}};
            const Run overlap = runAt(input, arrivals);
            const Run expected = run(input, genuineLoss ? std::vector<int>{0, 2, 3, 4}
                                                       : std::vector<int>{0, 1, 2, 3, 4}, true, 1);
            check(overlap.losses == (genuineLoss ? 1 : 0)
                  && overlap.breaks == (genuineLoss ? 1 : 0)
                  && overlap.samples == expected.samples,
                  "overlapping gaps preserve actual audio and conceal only genuinely absent packets");
        }
        for (int missing : {1, 3, 6, 10, 15}) {
            std::vector<int> dropped;
            for (int i : ordered) {
                if (i < 40 || i >= 40 + missing) {
                    dropped.push_back(i);
                }
            }
            const Run loss = run(input, dropped, true, 1);
            const Run off = run(input, dropped, false, 1);
            const qsizetype expected = (120 - missing + std::min(missing, 8)) * 240 * 2;
            check(loss.losses == missing && loss.breaks == 1,
                  "real burst loss retains packet count and a single scorer gap");
            check(loss.samples.size() == expected, "real loss still invokes bounded PLC exactly once");
            check(off.samples.size() == (120 - missing) * 240 * 2 && off.losses == missing,
                  "PLC toggle preserves loss accounting and surviving audio");
        }
        if (pcc == 0x8005) {
            for (quint16 otherPcc : {quint16{0x03e3}, quint16{0x0123}}) {
                PanadapterStream stream;
                QVector<float> opusSamples;
                bool otherStream = false;
                QObject::connect(&stream, &PanadapterStream::pcmFrameReady,
                    [&](const PcmFrame& frame) {
                        if (!otherStream) {
                            opusSamples += frame.samples();
                        }
                    });
                for (int i = 0; i < 120; ++i) {
                    if (i == 40) {
                        QByteArray other = packet(0, otherPcc, pcm(0, otherPcc == 0x03e3));
                        qToBigEndian<quint32>(kStream + 1,
                            reinterpret_cast<uchar*>(other.data()) + 4);
                        otherStream = true;
                        VitaSequenceLossLogTestAccess::feed(stream, other, 399);
                        otherStream = false;
                        stream.registerDaxStream(kStream + 1, 1);
                    }
                    VitaSequenceLossLogTestAccess::feed(stream, input[i], i * 10);
                }
                check(opusSamples == clean.samples && stream.packetErrorCount() == 0,
                      "late PCM/DAX adoption on another stream preserves the active Opus decoder");
            }
            PanadapterStream stream;
            QVector<float> samples;
            QObject::connect(&stream, &PanadapterStream::pcmFrameReady,
                [&samples](const PcmFrame& frame) { samples += frame.samples(); });
            for (int i = 0; i < 40; ++i) {
                VitaSequenceLossLogTestAccess::feed(stream, input[i], i * 10);
            }
            stream.restartStreamSequence(kStream);
            samples.clear();
            for (int i = 0; i < 40; ++i) {
                VitaSequenceLossLogTestAccess::feed(stream, input[i], 1000 + i * 10);
            }
            check(samples == clean.samples.mid(0, 40 * 240 * 2)
                  && stream.packetErrorCount() == 0,
                  "restarting the owning Opus stream retires its decoder history");
        }
        std::printf("pcc=0x%04x ordered_frames=%lld reordered_frames=%lld reorder_losses=%d\n",
                    pcc, static_cast<long long>(clean.samples.size() / 2),
                    static_cast<long long>(reordered.samples.size() / 2), reordered.losses);
    }
    {
        PanadapterStream stream;
        int frames = 0;
        QObject::connect(&stream, &PanadapterStream::pcmFrameReady,
            [&frames](const PcmFrame& frame) { frames += frame.frameCount(); });
        VitaSequenceLossLogTestAccess::feed(stream, packet(0, 0x0123, pcm(0, false)), 0);
        VitaSequenceLossLogTestAccess::feed(stream, packet(2, 0x0123, pcm(2, false)), 1);
        check(frames == 240 && VitaSequenceLossLogTestAccess::queuedBytes(stream) > 0,
              "a gap retains its successor before the deadline");
        stream.restartStreamSequence(kStream);
        check(stream.audioStreamDiagnostics().isEmpty(),
              "retired RX streams do not remain as ever-slower delivery rows");
        VitaSequenceLossLogTestAccess::flush(stream, 100);
        check(frames == 240 && stream.packetErrorCount() == 0,
              "retired queued audio is neither delivered nor charged to a reused stream");
        VitaSequenceLossLogTestAccess::feed(stream, packet(9, 0x0123, pcm(9, false)), 101);
        check(frames == 480 && stream.packetErrorCount() == 0,
              "reused stream starts without concealment from its old instance");
    }
    {
        PanadapterStream stream;
        int speakerFrames = 0;
        int daxFrames = 0;
        QObject::connect(&stream, &PanadapterStream::pcmFrameReady,
            [&speakerFrames](const PcmFrame& frame) { speakerFrames += frame.frameCount(); });
        QObject::connect(&stream, &PanadapterStream::daxPcmReady,
            [&daxFrames](int, const PcmFrame& frame) { daxFrames += frame.frameCount(); });
        VitaSequenceLossLogTestAccess::feed(stream, packet(0, 0x03e3, pcm(0, true)), 0);
        VitaSequenceLossLogTestAccess::feed(stream, packet(2, 0x03e3, pcm(2, true)), 1);
        check(stream.audioStreamDiagnostics().size() == 1,
              "unclassified PCM initially contributes a speaker-audio timing row");
        stream.registerDaxStream(kStream, 1);
        check(stream.audioStreamDiagnostics().isEmpty()
              && VitaSequenceLossLogTestAccess::queuedBytes(stream) == 0,
              "DAX adoption immediately retires stale speaker timing and queued packets");
        VitaSequenceLossLogTestAccess::feed(stream, packet(3, 0x03e3, pcm(3, true)), 2);
        VitaSequenceLossLogTestAccess::flush(stream, 100);
        check(speakerFrames == 240 && daxFrames == 240
              && VitaSequenceLossLogTestAccess::queuedBytes(stream) == 0
              && stream.packetErrorCount() == 0,
              "late DAX registration retires queued speaker audio before changing its route");
    }
    std::printf("vita_rx_reorder_audio: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
