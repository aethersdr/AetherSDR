// ax25_replay — offline AX.25 decode diagnostic.
//
// Loads a mono float32 WAV (e.g. an AetherModem "Capture 3m" recording) and
// replays it through the AetherAx25LibmodemShim decoder, printing every decoded
// frame plus the final reject diagnostics. This lets us debug an on-air decode
// failure from a captured .wav with no radio in the loop.
//
// Usage: ax25_replay <capture.wav> [baud]
//   baud: 300 (HF) or 1200 (VHF). Default 1200.

#include "core/tnc/AetherAx25LibmodemShim.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace AetherSDR;

namespace {

quint16 readLe16(const char* b)
{
    return static_cast<quint16>(static_cast<unsigned char>(b[0]))
        | (static_cast<quint16>(static_cast<unsigned char>(b[1])) << 8);
}

quint32 readLe32(const char* b)
{
    return static_cast<quint32>(static_cast<unsigned char>(b[0]))
        | (static_cast<quint32>(static_cast<unsigned char>(b[1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(b[2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(b[3])) << 24);
}

bool loadWav(const QString& path, std::vector<float>& samples, int& sampleRate, QString& err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        err = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }
    quint16 format = 0, channels = 0, bits = 0;
    const char* data = nullptr;
    qsizetype dataBytes = 0;
    qsizetype pos = 12;
    while (pos + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 sz = readLe32(bytes.constData() + pos + 4);
        pos += 8;
        if (pos + static_cast<qsizetype>(sz) > bytes.size())
            break;
        if (id == "fmt " && sz >= 16) {
            const char* fmt = bytes.constData() + pos;
            format = readLe16(fmt);
            channels = readLe16(fmt + 2);
            sampleRate = static_cast<int>(readLe32(fmt + 4));
            bits = readLe16(fmt + 14);
        } else if (id == "data") {
            data = bytes.constData() + pos;
            dataBytes = static_cast<qsizetype>(sz);
        }
        pos += static_cast<qsizetype>(sz);
        if (sz & 1u)
            ++pos;
    }
    if (format != 3 || channels != 1 || bits != 32) {
        err = QStringLiteral("expected mono float32 WAV (got fmt=%1 ch=%2 bits=%3)")
            .arg(format).arg(channels).arg(bits);
        return false;
    }
    samples.resize(static_cast<size_t>(dataBytes / 4));
    std::memcpy(samples.data(), data, static_cast<size_t>(dataBytes));
    return true;
}

QString hex(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex(' ').toUpper());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <capture.wav> [baud=1200]\n", argv[0]);
        return 2;
    }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const int baud = (argc >= 3) ? QString::fromLocal8Bit(argv[2]).toInt() : 1200;

    std::vector<float> samples;
    int sampleRate = 0;
    QString err;
    if (!loadWav(path, samples, sampleRate, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.toLocal8Bit().constData());
        return 1;
    }
    std::printf("Loaded %zu samples @ %d Hz (%.1f s)\n",
                samples.size(), sampleRate,
                sampleRate > 0 ? double(samples.size()) / sampleRate : 0.0);

    const Ax25ModemProfile profile =
        (baud == 300) ? Ax25ModemProfile::Hf300 : Ax25ModemProfile::Vhf1200;

    // Sweep both tone polarities so a mark/space inversion (common between
    // different radios/TNCs) shows up immediately.
    int grandTotal = 0;
    for (Ax25TonePolarity polarity : {Ax25TonePolarity::Normal, Ax25TonePolarity::Inverted}) {
    AetherAx25LibmodemShim shim;  // NOLINT — block intentionally indented one level

    shim.configure(ax25DemodConfigForProfile(profile, polarity));
    std::printf("\n##### Decoder: %s #####\n", shim.demodDescription().toLocal8Bit().constData());

    int decoded = 0;
    QObject::connect(&shim, &AetherAx25LibmodemShim::frameDecoded,
                     [&](const Ax25DecodedFrame& f) {
        ++decoded;
        const char* kind = f.isUiFrame ? "UI"
            : (f.control == 0x2f || f.control == 0x3f) ? "SABM"
            : (f.control == 0x6f || f.control == 0x73) ? "UA"
            : "CTRL";
        std::printf("  FRAME #%d %s  %s > %s%s  ctrl=0x%02X pid=0x%02X  len=%d\n",
                    decoded, kind,
                    f.source.toLocal8Bit().constData(),
                    f.destination.toLocal8Bit().constData(),
                    f.path.isEmpty() ? "" : (" via " + f.path.join(',')).toLocal8Bit().constData(),
                    f.control, f.pid, int(f.payload.size()));
        if (!f.payloadText.isEmpty())
            std::printf("        text: %s\n", f.payloadText.toLocal8Bit().constData());
        if (!f.ax25FrameNoFcs.isEmpty())
            std::printf("        bytes(noFcs): %s\n", hex(f.ax25FrameNoFcs).toLocal8Bit().constData());
    });

    // Feed in realistic chunks so windowed diagnostics behave like the live tap.
    const int chunk = sampleRate / 10; // ~100 ms
    for (size_t off = 0; off < samples.size(); off += chunk) {
        const int n = int(std::min<size_t>(chunk, samples.size() - off));
        shim.processMonoFloat(samples.data() + off, n, sampleRate);
    }

    const Ax25DecoderDiagnostics d = shim.diagnosticsSnapshot();
    std::printf("\n=== RESULT ===\n");
    std::printf("decoded frames : %d\n", decoded);
    std::printf("hdlc starts    : %llu\n", (unsigned long long)d.hdlcFrameStarts);
    std::printf("hdlc candidates: %llu\n", (unsigned long long)d.hdlcFrameCandidates);
    std::printf("ax25-like      : %llu\n", (unsigned long long)d.plausibleAx25Candidates);
    std::printf("accepted       : %llu\n", (unsigned long long)d.framesAccepted);
    std::printf("rejected       : %llu (short=%llu badFcs=%llu malformed=%llu)\n",
                (unsigned long long)d.decodeRejected,
                (unsigned long long)d.rejectTooShort,
                (unsigned long long)d.rejectBadFcs,
                (unsigned long long)d.rejectMalformed);
    std::printf("last reject    : %s (bytes=%d bits=%d fcs=%s/%s)\n",
                d.lastRejectReason.toLocal8Bit().constData(),
                d.lastRejectFrameBytes, d.lastRejectFrameBits,
                d.lastRejectActualFcs.toLocal8Bit().constData(),
                d.lastRejectExpectedFcs.toLocal8Bit().constData());
    grandTotal += decoded;
    } // end polarity sweep

    std::printf("\n=== TOTAL decoded across both polarities: %d ===\n", grandTotal);
    return grandTotal > 0 ? 0 : 3;
}
