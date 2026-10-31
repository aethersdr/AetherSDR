// A late FFT packet or waterfall tile from the previous frame must not
// restart assembly and drop the frame in progress, and is counted as late
// (#6883). FlexLib discards a late FFT packet:
// reference/FlexLib/FlexLib/Panadapter.cs:805 (v4.3.16). The frame maps are
// also removed from another thread while packets decode.

#include "core/backends/flex/PanadapterStream.h"
#include "core/VitaBinCoverage.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtEndian>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace AetherSDR;

namespace AetherSDR {
struct PanadapterLatePacketTestAccess {
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

    // One waterfall tile of kHalf bins starting at firstBin, all set to value.
    static void tile(PanadapterStream& stream, quint32 id,
                     quint32 timecode, quint16 firstBin, qint16 value) {
        QByteArray packet(64 + 2 * kHalf, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<quint16>(kHalf, raw + 48);
        qToBigEndian<quint16>(1, raw + 50);
        qToBigEndian<quint32>(timecode, raw + 52);
        qToBigEndian<quint16>(kTotalBins, raw + 60);
        qToBigEndian<quint16>(firstBin, raw + 62);
        for (int i = 0; i < kHalf; ++i) {
            qToBigEndian<qint16>(value, raw + 64 + 2 * i);
        }
        stream.decodeWaterfallTile(raw, packet.size(), false, id);
    }

    static void unregister(PanadapterStream& stream, quint32 id) {
        stream.unregisterPanStream(id);
        stream.unregisterWfStream(id);
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
    TestSettingsProfile profile(QStringLiteral("panadapter-late-packet"));
    QCoreApplication app(argc, argv);

    using Access = PanadapterLatePacketTestAccess;

    CHECK(!isLateVitaFrame(10, 10));
    CHECK(isLateVitaFrame(10, 9));
    CHECK(isLateVitaFrame(10, 10 - kVitaLateFrameLookback));
    CHECK(!isLateVitaFrame(10, 9 - kVitaLateFrameLookback));
    CHECK(!isLateVitaFrame(10, 11));
    CHECK(isLateVitaFrame(1, 0xFFFFFFFFu));
    CHECK(!isLateVitaFrame(0xFFFFFFFFu, 0));

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

    // A resize: an old-width straggler arriving inside the first new-width
    // frame is dropped too, and the new-width frame is emitted whole.
    {
        constexpr quint32 kId = kRef + 4;
        constexpr quint16 kWide = Access::kTotalBins * 2;
        Access::frame(stream, kId, 20, 200);
        Access::fragment(stream, kId, 21, 0, 100, Access::kTotalBins, kWide);
        Access::fragment(stream, kId, 20, Access::kHalf, 300);
        Access::fragment(stream, kId, 21, Access::kTotalBins, 200,
                         Access::kTotalBins, kWide);
        CHECK(emitted[kId].size() == 2);
        if (emitted[kId].size() == 2) {
            const QVector<float>& bins = emitted[kId][1];
            CHECK(bins.size() == kWide);
            for (int i = 0; i < Access::kTotalBins; ++i) {
                CHECK(bins[i] == v100);
                CHECK(bins[Access::kTotalBins + i] == v200);
            }
        }
    }

    // One late packet each from the ordinary, wrap and resize cases.
    CHECK(stream.categoryStats(PanadapterStream::CatFFT).late == 3);

    // A counter restart that lands 1-8 below the current frame is read as
    // late, at either width, until the count passes the old frame: at most
    // kVitaLateFrameLookback + 1 frames are lost, once. Accepted because
    // FlexLib keeps its counter across a resize and drops any older frame
    // with no limit (Panadapter.cs:805), and a FLEX-8600 capture showed
    // waterfall timecodes stepping +1 across 6 width changes.
    {
        constexpr quint32 kId = kRef + 6;
        Access::frame(stream, kId, 10, 100);
        for (quint32 n = 5; n <= 10; ++n) {
            Access::frame(stream, kId, n, 200);
        }
        Access::fragment(stream, kId, 6, 0, 200, Access::kTotalBins * 2,
                         Access::kTotalBins * 2);
        CHECK(emitted[kId].size() == 1);
        CHECK(stream.categoryStats(PanadapterStream::CatFFT).late == 3 + 10 + 1);
        Access::frame(stream, kId, 11, 200);
        CHECK(emitted[kId].size() == 2);
    }

    // Waterfall: a late tile from row 41 inside row 42 is dropped, and row 42
    // is emitted whole with its own values.
    {
        constexpr quint32 kId = kRef + 0x100;
        QList<QVector<float>> rows;
        QList<quint32> timecodes;
        QObject::connect(&stream, &PanadapterStream::waterfallRowReady, &receiver,
            [&](quint32 id, const QVector<float>& bins, double, double,
                quint32 timecode, qint64) {
                if (id == kId) {
                    rows.append(bins);
                    timecodes.append(timecode);
                }
            });
        Access::tile(stream, kId, 41, 0, 1280);
        Access::tile(stream, kId, 41, Access::kHalf, 1280);
        Access::tile(stream, kId, 42, 0, 2560);
        Access::tile(stream, kId, 41, Access::kHalf, 3840);
        Access::tile(stream, kId, 42, Access::kHalf, 2560);
        CHECK(rows.size() == 2);
        CHECK(timecodes == (QList<quint32>{41, 42}));
        if (rows.size() == 2) {
            CHECK(rows[1].size() == Access::kTotalBins);
            for (float v : rows[1]) {
                CHECK(v == 20.0f);
            }
        }
        CHECK(stream.categoryStats(PanadapterStream::CatWaterfall).late == 1);
    }

    // Pan teardown on the main thread while the network thread decodes. Run
    // under ASan/TSan, the frame maps must never be freed under the decoder.
    {
        PanadapterStream racing;
        constexpr quint32 kId = kRef + 0x200;
        std::atomic<bool> stop{false};
        std::atomic<quint32> decoded{0};
        std::thread decoder([&]() {
            for (quint32 n = 0; !stop.load(); ++n) {
                Access::frame(racing, kId, n, 100);
                Access::tile(racing, kId, n, 0, 1280);
                Access::tile(racing, kId, n, Access::kHalf, 1280);
                decoded.store(n + 1);
            }
        });
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; clock.elapsed() < 300 || decoded.load() < 1000; ++i) {
            Access::unregister(racing, kId);
            if (i % 64 == 0) {
                racing.clearRegisteredStreams();
            }
        }
        stop.store(true);
        decoder.join();
        CHECK(decoded.load() >= 1000);
    }

    if (g_failures == 0) {
        std::printf("panadapter_late_packet_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
