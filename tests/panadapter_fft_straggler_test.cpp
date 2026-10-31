// A late FFT packet from the previous frame must not restart assembly and
// drop the frame in progress (#6883). FlexLib discards it:
// reference/FlexLib/FlexLib/Panadapter.cs:805 (v4.3.16).

#include "core/backends/flex/PanadapterStream.h"
#include "core/VitaBinCoverage.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QtEndian>

#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
struct PanadapterFftStragglerTestAccess {
    static constexpr quint16 kTotalBins = 8;
    static constexpr quint16 kHalf = kTotalBins / 2;

    // One fragment of numBins bins starting at startBin, all set to value.
    static void fragment(PanadapterStream& stream, quint32 id,
                         quint32 frameIndex, quint16 startBin, quint16 value,
                         quint16 numBins = kHalf, quint16 totalBins = kTotalBins) {
        QByteArray packet(40 + 2 * numBins, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<quint16>(startBin, raw + 28);
        qToBigEndian<quint16>(numBins, raw + 30);
        qToBigEndian<quint16>(2, raw + 32);
        qToBigEndian<quint16>(totalBins, raw + 34);
        qToBigEndian<quint32>(frameIndex, raw + 36);
        for (int i = 0; i < numBins; ++i) {
            qToBigEndian<quint16>(value, raw + 40 + 2 * i);
        }
        stream.decodeFFT(raw, packet.size(), false, id);
    }

    static void frame(PanadapterStream& stream, quint32 id,
                      quint32 frameIndex, quint16 value) {
        fragment(stream, id, frameIndex, 0, value);
        fragment(stream, id, frameIndex, kHalf, value);
    }
};
}

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("panadapter-fft-straggler"));
    QCoreApplication app(argc, argv);

    using Access = PanadapterFftStragglerTestAccess;

    CHECK(!isStragglerFftFrame(10, 10));
    CHECK(isStragglerFftFrame(10, 9));
    CHECK(isStragglerFftFrame(10, 10 - kFftStragglerLookbackFrames));
    CHECK(!isStragglerFftFrame(10, 9 - kFftStragglerLookbackFrames));
    CHECK(!isStragglerFftFrame(10, 11));
    CHECK(isStragglerFftFrame(1, 0xFFFFFFFFu));
    CHECK(!isStragglerFftFrame(0xFFFFFFFFu, 0));

    PanadapterStream stream;
    QHash<quint32, QList<QVector<float>>> emitted;
    QObject receiver;
    QObject::connect(&stream, &PanadapterStream::spectrumReady, &receiver,
        [&](quint32 id, const QVector<float>& bins, qint64,
            const SpectrumDecodeScale&) { emitted[id].append(bins); });

    // Reference decodes of the two fill values on a clean sequence.
    constexpr quint32 kRef = 0x40000000;
    Access::frame(stream, kRef, 1, 100);
    Access::frame(stream, kRef, 2, 200);
    CHECK(emitted[kRef].size() == 2);
    if (emitted[kRef].size() != 2) {
        return 1;
    }
    const float v100 = emitted[kRef][0].front();
    const float v200 = emitted[kRef][1].front();
    CHECK(v100 != v200);

    // Frame 10 first half, then a straggler from frame 9, then frame 10's
    // second half: frame 10 is emitted intact and frame 9 never is.
    {
        constexpr quint32 kId = kRef + 1;
        Access::frame(stream, kId, 8, 200);
        Access::fragment(stream, kId, 10, 0, 100);
        Access::fragment(stream, kId, 9, Access::kHalf, 300);
        Access::fragment(stream, kId, 10, Access::kHalf, 200);
        CHECK(emitted[kId].size() == 2);
        if (emitted[kId].size() == 2) {
            const QVector<float>& bins = emitted[kId][1];
            CHECK(bins.size() == Access::kTotalBins);
            for (int i = 0; i < Access::kHalf; ++i) {
                CHECK(bins[i] == v100);
                CHECK(bins[Access::kHalf + i] == v200);
            }
        }
    }

    // The same straggler across the 32-bit wrap.
    {
        constexpr quint32 kId = kRef + 2;
        Access::fragment(stream, kId, 1, 0, 100);
        Access::fragment(stream, kId, 0xFFFFFFFFu, Access::kHalf, 300);
        Access::fragment(stream, kId, 1, Access::kHalf, 200);
        CHECK(emitted[kId].size() == 1);
    }

    // A counter that restarts far behind, or wraps forward, is a new frame,
    // so the pan never blanks waiting for the old count to come back.
    {
        constexpr quint32 kId = kRef + 3;
        Access::frame(stream, kId, 1000, 100);
        Access::frame(stream, kId, 0, 200);
        CHECK(emitted[kId].size() == 2);
    }
    {
        constexpr quint32 kId = kRef + 5;
        Access::frame(stream, kId, 0xFFFFFFFFu, 100);
        Access::frame(stream, kId, 0, 200);
        CHECK(emitted[kId].size() == 2);
    }

    // A geometry change keeps restarting assembly even with an older index.
    {
        constexpr quint32 kId = kRef + 4;
        Access::fragment(stream, kId, 20, 0, 100);
        Access::fragment(stream, kId, 19, 0, 100, 2, 2);
        CHECK(emitted[kId].size() == 1);
        if (!emitted[kId].isEmpty()) {
            CHECK(emitted[kId][0].size() == 2);
        }
    }

    if (g_failures == 0) {
        std::printf("panadapter_fft_straggler_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
