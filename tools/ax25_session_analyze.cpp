// ax25_session_analyze — replay a captured WAV through BOTH the real decoder
// (AetherAx25LibmodemShim) AND the real connected-mode state machine
// (Ax25Connection), then report how our client reacts frame-by-frame and flag
// sequencing / retry gaps (out-of-sequence drops, REJ storms, stalls).
//
// The captures are RX-only (what the far end transmitted to us), so this models
// "if we received exactly this frame stream, in this order, how does our state
// machine behave?" — isolating logical protocol bugs from RF/half-duplex timing.
//
// Usage: ax25_session_analyze <capture.wav> [baud=1200]

#include "core/tnc/AetherAx25LibmodemShim.h"
#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25Connection.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace AetherSDR;
using AetherSDR::ax25::Address;
using AetherSDR::ax25::Frame;
using AetherSDR::ax25::FrameType;

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
    if (!f.open(QIODevice::ReadOnly)) { err = QStringLiteral("cannot open %1").arg(path); return false; }
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        err = QStringLiteral("not a RIFF/WAVE file"); return false;
    }
    quint16 format = 0, channels = 0, bits = 0;
    const char* data = nullptr; qsizetype dataBytes = 0; qsizetype pos = 12;
    while (pos + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 sz = readLe32(bytes.constData() + pos + 4);
        pos += 8;
        if (pos + static_cast<qsizetype>(sz) > bytes.size()) break;
        if (id == "fmt " && sz >= 16) {
            const char* fmt = bytes.constData() + pos;
            format = readLe16(fmt); channels = readLe16(fmt + 2);
            sampleRate = static_cast<int>(readLe32(fmt + 4)); bits = readLe16(fmt + 14);
        } else if (id == "data") { data = bytes.constData() + pos; dataBytes = static_cast<qsizetype>(sz); }
        pos += static_cast<qsizetype>(sz);
        if (sz & 1u) ++pos;
    }
    if (format != 3 || channels != 1 || bits != 32) {
        err = QStringLiteral("expected mono float32 WAV (fmt=%1 ch=%2 bits=%3)").arg(format).arg(channels).arg(bits);
        return false;
    }
    samples.resize(static_cast<size_t>(dataBytes / 4));
    std::memcpy(samples.data(), data, static_cast<size_t>(dataBytes));
    return true;
}

QString frameDesc(const Frame& f)
{
    QString s = ax25::frameTypeName(f.type);
    if (f.type == FrameType::I)
        s += QStringLiteral(" NS=%1 NR=%2").arg(f.ns).arg(f.nr);
    else if (f.type == FrameType::RR || f.type == FrameType::RNR || f.type == FrameType::REJ)
        s += QStringLiteral(" NR=%1").arg(f.nr);
    if (f.pollFinal) s += QStringLiteral(" P/F");
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::fprintf(stderr, "usage: %s <capture.wav> [baud=1200]\n", argv[0]); return 2; }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const int baud = (argc >= 3) ? QString::fromLocal8Bit(argv[2]).toInt() : 1200;

    std::vector<float> samples; int sampleRate = 0; QString err;
    if (!loadWav(path, samples, sampleRate, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.toLocal8Bit().constData()); return 1;
    }

    // 1) Decode the WAV to an ordered list of (time, frame).
    const Ax25ModemProfile profile = (baud == 300) ? Ax25ModemProfile::Hf300 : Ax25ModemProfile::Vhf1200;
    struct Rx { double t; Frame frame; };
    QVector<Rx> rx;
    {
        AetherAx25LibmodemShim shim;
        shim.configure(ax25DemodConfigForProfile(profile, Ax25TonePolarity::Normal));
        const int chunk = sampleRate / 10;
        for (size_t off = 0; off < samples.size(); off += chunk) {
            const int n = int(std::min<size_t>(chunk, samples.size() - off));
            const auto frames = shim.processMonoFloat(samples.data() + off, n, sampleRate);
            for (const auto& df : frames) {
                if (df.ax25FrameNoFcs.isEmpty()) continue;
                auto parsed = Frame::decode(df.ax25FrameNoFcs);
                if (parsed) rx.push_back({double(off) / sampleRate, *parsed});
            }
        }
    }
    std::printf("== %s ==\n", path.toLocal8Bit().constData());
    std::printf("decoded %d frame(s) over %.0f s\n", int(rx.size()),
                samples.size() / double(sampleRate ? sampleRate : 1));
    if (rx.isEmpty()) { std::printf("(no frames)\n\n"); return 0; }

    // 2) Identify our local address (the dest the far end is talking to) + peer.
    const Address local = rx.first().frame.dest;
    const Address peer = rx.first().frame.src;
    std::printf("local=%s  peer=%s\n", local.toString().toLocal8Bit().constData(),
                peer.toString().toLocal8Bit().constData());

    // 3) Drive the real state machine. connectTo() arms the Connecting state; the
    //    first I/RR adopts the link (lost-UA recovery), then frames flow normally.
    Ax25Connection conn;
    conn.setLocalAddress(local);

    QVector<QByteArray> emitted;
    QByteArray deliveredThisFrame;
    int totalDelivered = 0, inSeq = 0, outOfSeq = 0, rejSent = 0, rrSent = 0;
    int maxOutOfSeqRun = 0, curOutOfSeqRun = 0;

    QObject::connect(&conn, &Ax25Connection::sendFrame,
                     [&](const QByteArray& f) { emitted.append(f); });
    QObject::connect(&conn, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { deliveredThisFrame += d; });

    conn.connectTo(peer);
    emitted.clear();

    int prevVr = -1;
    for (const Rx& r : rx) {
        if (r.frame.dest != local) continue; // only frames addressed to us
        emitted.clear();
        deliveredThisFrame.clear();
        const int vrBefore = conn.recvSeq();
        conn.onFrameReceived(r.frame);

        // Classify our reaction.
        QStringList reactions;
        for (const QByteArray& e : emitted) {
            auto d = Frame::decode(e);
            if (d) reactions << frameDesc(*d);
            if (d && d->type == FrameType::REJ) ++rejSent;
            if (d && d->type == FrameType::RR) ++rrSent;
        }
        const bool advanced = conn.recvSeq() != vrBefore;
        if (r.frame.type == FrameType::I) {
            if (advanced) { ++inSeq; curOutOfSeqRun = 0; }
            else { ++outOfSeq; ++curOutOfSeqRun; maxOutOfSeqRun = std::max(maxOutOfSeqRun, curOutOfSeqRun); }
        }
        totalDelivered += deliveredThisFrame.size();

        const char* flag = (r.frame.type == FrameType::I && !advanced) ? "  <-- DROPPED (out-of-seq)" : "";
        std::printf("[%6.1fs] RX %-18s -> TX [%s]  V(R)=%d->%d  data=%dB%s\n",
                    r.t, frameDesc(r.frame).toLocal8Bit().constData(),
                    reactions.join(QStringLiteral(", ")).toLocal8Bit().constData(),
                    vrBefore, conn.recvSeq(), int(deliveredThisFrame.size()), flag);
        prevVr = conn.recvSeq();
    }
    (void)prevVr;

    std::printf("\n  SUMMARY: I-frames in-seq(delivered)=%d  out-of-seq(dropped)=%d  "
                "REJ sent=%d  RR sent=%d  bytes delivered=%d  longest out-of-seq run=%d\n\n",
                inSeq, outOfSeq, rejSent, rrSent, totalDelivered, maxOutOfSeqRun);
    return 0;
}
