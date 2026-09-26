// A transport sequence gap must not be transformed across.
//
// THE DEFECT. Hl2Spectrum (and, until the ANAN panadapter moved to WDSP's
// analyzer, its ANAN twin AnanSpectrum) builds one FFT frame
// out of MANY transport blocks: process() pushes samples into an accumulator and
// transforms only when it reaches exactly fftSize, carrying a partial frame
// across calls. An EP6 block is 126 IQ samples, so the 1024-point frame both
// backends actually run spans ~8 of them. When UDP packets are lost part-way through
// building a frame, the accumulator keeps its pre-gap samples and finishes the
// frame from post-gap ones. The resulting transform spans a time discontinuity,
// the phase relationship across the seam describes nothing, and the result is
// then rendered as a measurement of the band.
//
// Both spectrum classes already had the remedy -- reset() -- and NEITHER HAD A
// CALLER ANYWHERE IN THE TREE. Its own comment named a geometry change as the
// case, but a geometry change RECONSTRUCTS the object (Hl2RxDsp::configure does
// `m_spectrum = std::make_unique<Hl2Spectrum>(...)`, AnanRxDsp::installChannel
// moves a freshly built one in), which clears the accumulator implicitly. So the
// documented purpose was already covered by construction, and the case that
// actually needed it was never wired.
//
// WHAT A GAP DID REACH, before this. A counter, and only a counter:
// MetisClient's EP6 path computes the gap, adds it to m_drops, and emits
// dropsUpdated, which Hl2Backend mirrors onto the GUI thread for the health
// rows. On the ANAN side P2Client's dropsUpdated reaches a deliberately empty
// lambda in AnanBackend. Neither path touches a sample-path object. "A gap
// reaches nothing" would be too strong; "a gap reaches nothing that can act on
// it" is the accurate statement, and it is what this file closes.
//
// FIVE CLAIMS, in the order they build on each other:
//
//   1. Hl2Spectrum::reset() reports what it discarded, and a frame completed
//      after it is built from post-gap samples ONLY -- proved by equality with a
//      spectrum that never saw the pre-gap samples at all.
//   2. THE NO-LOSS PATH IS UNCHANGED. The same feed with no gap still stitches
//      across calls and still produces the identical stitched frame. This is the
//      claim that would catch a "fix" that simply stopped carrying partial
//      frames.
//   3. (Formerly: AnanSpectrum, the HL2 class's ANAN twin, behaved identically.
//      The ANAN panadapter now runs on WDSP's display analyzer; its staging
//      block plays the partial frame's part, and 4b covers it.)
//   4. The DSP stages act on the gap: Hl2RxDsp::onSequenceGap() discards the
//      partial frame, AnanRxDsp::onSequenceGap() the partly staged analyzer
//      block, and each counts it, and
//      only when there WAS one -- a gap on a frame boundary corrupts nothing and
//      must not be counted, or the counter degenerates into a worse copy of the
//      dropped-packet row.
//   5. MetisClient emits the gap on the wire edge that produces it, and emits it
//      BEFORE the IQ block from the same datagram. That ordering is the whole
//      contract: the reverse would discard the post-gap samples it had just
//      accepted.
//
// Tones are generated in WIRE convention (exp(-jwt) for a signal above centre)
// wherever a DSP stage is involved -- see HERMES.md §16 and the banner in
// tests/anan_rxdsp_handedness_test.cpp for why the textbook convention hides
// exactly this class of bug. Where only a spectrum object is under test the
// convention does not matter, because every assertion is an equality between two
// runs of the same code over the same samples.
//
// Additional cases feed both ingest handlers through the DSP, covering forward
// loss, rewinds, duplicates, wraparound, DDC isolation and rejected datagrams.
// Backend constructor wiring remains outside this test.
//
// NO RADIO, NO SOCKET, NO KEYING. Section 5 feeds MetisClient recorded-shape
// datagrams through the MetisClientTestAccess friend seam; nothing binds and
// nothing transmits.

#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace AetherSDR::hl2 {
// No start(), no bind, no peer: feed the ingest path the bytes a socket would
// have delivered. The same seam tests/hl2_ep4_ingest_test.cpp uses.
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void feedDatagram(MetisClient& client, std::span<const std::uint8_t> bytes)
    {
        client.handleDatagram(bytes);
    }
};
}  // namespace AetherSDR::hl2

namespace AetherSDR::anan {
struct P2ClientTestAccess {
    static void enableDdcs(P2Client& client, int count) { client.m_activeDdcCount = count; }
    static void feedDatagram(P2Client& client, std::span<const std::uint8_t> bytes, quint16 port)
    {
        client.handleDatagram(bytes, port);
    }
};
} // namespace AetherSDR::anan

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;

// A complex tone at integer bin k0 of an N-point frame, in the TEXTBOOK
// convention. Used only where a bare spectrum object is under test.
static std::vector<std::complex<float>> tone(int n, int k0, float amp, int phaseOffset = 0)
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * k0 * (i + phaseOffset) / n;
        v[static_cast<std::size_t>(i)] =
            amp * std::complex<float>(static_cast<float>(std::cos(ph)),
                                      static_cast<float>(std::sin(ph)));
    }
    return v;
}

// A tone `offsetHz` above centre in WIRE convention -- exp(-jwt), the
// handedness a real HPSDR/ANAN radio actually sends.
static std::vector<std::complex<float>> wireTone(int n, double offsetHz, double rateHz,
                                                 float amp, int phaseOffset = 0)
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * offsetHz * (i + phaseOffset) / rateHz;
        v[static_cast<std::size_t>(i)] =
            amp * std::complex<float>(static_cast<float>(std::cos(ph)),
                                      static_cast<float>(-std::sin(ph)));
    }
    return v;
}

static bool binsEqual(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

// Full-sized wire blocks, injected into the production ingest handlers.
static std::vector<std::uint8_t> makeEp6(std::uint32_t seq, bool postGap = false)
{
    using namespace AetherSDR::hl2;
    std::vector<std::uint8_t> packet(kUsbPacketSize, 0);
    packet[0] = 0xEF; packet[1] = 0xFE; packet[2] = 0x01; packet[3] = 0x06;
    for (int i = 0; i < 4; ++i) {
        packet[4 + i] = static_cast<std::uint8_t>(seq >> (24 - 8 * i));
    }
    for (const std::size_t fs : {std::size_t{8}, std::size_t{8 + kFrameSize}}) {
        packet[fs] = packet[fs + 1] = packet[fs + 2] = 0x7F;
        for (std::size_t i = 0; i < 63; ++i) {
            packet[fs + 8 + i * 8] = postGap ? 0x20 : 0; // I = 0.25 or 0
        }
    }
    return packet;
}

static std::vector<std::uint8_t> makeDdc(std::uint32_t seq, bool postGap = false)
{
    using namespace AetherSDR::anan;
    constexpr int samples = 126;
    std::vector<std::uint8_t> packet(kDdcHeaderLen + samples * kDdcSampleBytes, 0);
    for (int i = 0; i < 4; ++i) {
        packet[i] = static_cast<std::uint8_t>(seq >> (24 - 8 * i));
    }
    packet[13] = 24;
    packet[15] = samples;
    for (int i = 0; i < samples; ++i) {
        packet[kDdcHeaderLen + i * kDdcSampleBytes] = postGap ? 0x20 : 0;
    }
    return packet;
}

template<class Dsp, class Client, class Wire, class Feed>
static void verifyContinuity(const char* name, std::uint32_t first, std::uint32_t next,
                             bool discontinuous, quint64 expectedDrops, Wire wire, Feed feed)
{
    std::fprintf(stderr, "Continuity case: %s\n", name);
    Dsp dsp;
    Client client;
    // HL2 builds one 1024-point FFT synchronously from its accumulator. ANAN's
    // panadapter is WDSP's analyzer, whose frames arrive from worker threads
    // after 16k samples, so the FRAME checks below are HL2-only; the ordering,
    // drop-accounting and discard checks apply to both. ANAN's discard counter
    // keeps the same meaning: a 1024-sample staging block stands where the
    // partial FFT frame did.
    constexpr bool kSyncFrames = !std::is_same_v<Dsp, anan::AnanRxDsp>;
    typename Dsp::Config cfg;
    if constexpr (kSyncFrames)
        cfg.fftSize = 1024;
    else
        cfg.panPoints = 1024;
    cfg.inputSampleRateHz = 48000;
    cfg.audioSampleRateHz = 24000;
    cfg.dspBlockSize = 1024;
    cfg.blockForOutput = true;
    std::string error;
    if (!dsp.configure(cfg, &error)) {
        check(false, error.c_str());
        return;
    }
    std::string order;
    int frames = 0;
    std::vector<float> bins;
    QObject::connect(&dsp, &Dsp::spectrumReady, &dsp,
                     [&](const std::vector<float>& frame) { bins = frame; ++frames; });
    wire(client, dsp, order);
    for (std::uint32_t i = 0; i < 8; ++i) {
        feed(client, first + i, false);
    }
    if constexpr (kSyncFrames)
        check(frames == 0, "eight 126-sample blocks leave a partial 1024-point FFT");
    check(dsp.spectrumGapDiscards() == 0, "initial contiguous run discards nothing");
    order.clear();
    feed(client, next, true);
    check(order == (discontinuous ? "GB" : "B"), "discontinuity precedes sample delivery");
    check(client.droppedPackets() == expectedDrops, "legacy packet-loss accounting is unchanged");
    if constexpr (kSyncFrames)
        check(frames == (discontinuous ? 0 : 1), "only contiguous samples may complete the pending FFT");
    check(dsp.spectrumGapDiscards() == (discontinuous ? 1u : 0u), "only discontinuities discard the pending window");
    if (kSyncFrames && discontinuous) {
        for (std::uint32_t i = 1; i < 9; ++i) {
            feed(client, next + i, true);
        }
        check(frames == 1, "a complete post-discontinuity window resumes the spectrum");
        Dsp clean;
        if (!clean.configure(cfg, &error)) {
            check(false, error.c_str());
            return;
        }
        std::vector<float> cleanBins;
        QObject::connect(&clean, &Dsp::spectrumReady, &clean,
                         [&](const std::vector<float>& frame) { cleanBins = frame; });
        clean.processIqBlock(std::vector<std::complex<float>>(1024, {0.25f, 0.0f}));
        check(binsEqual(bins, cleanBins), "resumed frame contains only post-discontinuity samples");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    constexpr int N = 64;   // small frame: the accumulator behaviour is the
                            // subject, not the transform's resolution
    constexpr int kPartial = 30;   // samples in flight when the gap lands

    // ---- 1 · Hl2Spectrum::reset() reports and discards ------------------
    {
        hl2::Hl2Spectrum spec(N);
        std::vector<float> bins;
        check(spec.reset() == 0, "a fresh accumulator discards nothing");

        // Two DIFFERENT signals either side of the seam, so a frame built from
        // both is distinguishable from one built from either alone.
        const auto preGap = tone(N, 10, 0.5f);
        const auto postGap = tone(N, 21, 0.5f);

        check(spec.process(std::span(preGap).subspan(0, kPartial), bins) == 0,
              "30 of 64 samples produce no frame");
        check(spec.reset() == static_cast<std::size_t>(kPartial),
              "reset() reports the partial frame it discarded");
        check(spec.reset() == 0, "...and the second reset has nothing left to discard");

        check(spec.process(postGap, bins) == 1,
              "a full frame of post-gap samples completes one frame");

        // The claim, stated as an equality rather than as a peak-bin argument:
        // the frame after the gap must be the frame a spectrum that NEVER SAW
        // the pre-gap samples would have produced. Bit-for-bit -- the same code
        // over the same input.
        hl2::Hl2Spectrum clean(N);
        std::vector<float> cleanBins;
        check(clean.process(postGap, cleanBins) == 1, "control produces one frame");
        check(binsEqual(bins, cleanBins),
              "the frame after a gap is built from post-gap samples ONLY");
    }

    // ---- 2 · THE NO-LOSS PATH IS UNCHANGED ------------------------------
    //
    // The fix must not turn the accumulator into a per-call buffer. With no
    // gap, a frame still spans calls and still stitches them, and it is still
    // the stitched frame -- NOT the clean one section 1 produced.
    {
        const auto preGap = tone(N, 10, 0.5f);
        // Phase-continuous with preGap's first 30 samples, as real contiguous
        // IQ would be: the seam this section proves is preserved is a seam in
        // the SIGNAL (bin 10 to bin 21), not a phase step.
        const auto postGap = tone(N, 21, 0.5f, kPartial);

        hl2::Hl2Spectrum spec(N);
        std::vector<float> stitched;
        check(spec.process(std::span(preGap).subspan(0, kPartial), stitched) == 0,
              "no-loss: 30 of 64 produce no frame");
        check(spec.process(std::span(postGap).subspan(0, N - kPartial), stitched) == 1,
              "no-loss: the remaining 34 complete the frame across the call boundary");

        // Identical to a single-call feed of the same 64 samples: carrying a
        // partial frame across calls is behaviour, not an accident.
        std::vector<std::complex<float>> oneShot;
        oneShot.insert(oneShot.end(), preGap.begin(), preGap.begin() + kPartial);
        oneShot.insert(oneShot.end(), postGap.begin(), postGap.begin() + (N - kPartial));
        hl2::Hl2Spectrum control(N);
        std::vector<float> controlBins;
        check(control.process(oneShot, controlBins) == 1, "control: one frame");
        check(binsEqual(stitched, controlBins),
              "no-loss: a frame spanning two calls is identical to one delivered whole");

        // And the two cases are genuinely different -- without this the whole
        // suite would pass on a class that discarded unconditionally.
        hl2::Hl2Spectrum cleanOnly(N);
        std::vector<float> cleanBins;
        cleanOnly.process(tone(N, 21, 0.5f), cleanBins);
        check(!binsEqual(stitched, cleanBins),
              "a stitched frame and a clean frame are not the same frame");
    }

    // ---- 4a · Hl2RxDsp acts on the gap ------------------------------------
    //
    // One stage up: the DSP owns the spectrum and is what the backend calls.
    // Fed EP6-shaped blocks, exactly as MetisClient delivers them.
    {
        const int rate = 48000;
        auto makeDsp = [&](hl2::Hl2RxDsp& dsp, std::string* err) {
            hl2::Hl2RxDsp::Config cfg;
            cfg.inputSampleRateHz = rate;
            cfg.audioSampleRateHz = 24000;
            cfg.dspBlockSize = 1024;
            cfg.fftSize = N;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.filterLowHz = 150.0;
            cfg.filterHighHz = 3000.0;
            cfg.blockForOutput = true;   // deterministic for an offline feed
            return dsp.configure(cfg, err);
        };

        // Pre-gap and post-gap IQ, in wire convention, at two different offsets.
        const auto preGap = wireTone(kPartial, 1000.0, rate, 0.5f);
        const auto postGap = wireTone(N, 5000.0, rate, 0.5f);

        // Run A: partial frame in flight, gap, then a full frame's worth.
        std::vector<float> afterGap;
        hl2::Hl2RxDsp dspA;
        std::string errA;
        check(makeDsp(dspA, &errA), ("HL2 DSP configures: " + errA).c_str());
        QObject::connect(&dspA, &hl2::Hl2RxDsp::spectrumReady, &dspA,
                         [&](const std::vector<float>& b) { afterGap = b; });
        check(dspA.spectrumGapDiscards() == 0, "HL2: no discards before any gap");
        dspA.processIqBlock(preGap);
        check(afterGap.empty(), "HL2: a partial frame emits nothing");
        dspA.onSequenceGap();
        check(dspA.spectrumGapDiscards() == 1,
              "HL2: a gap with a partial frame in flight is counted once");
        dspA.processIqBlock(postGap);
        check(afterGap.size() == static_cast<std::size_t>(N),
              "HL2: a frame is emitted after the gap");

        // Run B: the SAME post-gap samples, from a stage that never saw the
        // pre-gap ones. Equality is the claim.
        std::vector<float> cleanOnly;
        hl2::Hl2RxDsp dspB;
        std::string errB;
        check(makeDsp(dspB, &errB), ("HL2 control DSP configures: " + errB).c_str());
        QObject::connect(&dspB, &hl2::Hl2RxDsp::spectrumReady, &dspB,
                         [&](const std::vector<float>& b) { cleanOnly = b; });
        dspB.processIqBlock(postGap);
        check(binsEqual(afterGap, cleanOnly),
              "HL2: the post-gap frame contains no pre-gap sample");
        check(dspB.spectrumGapDiscards() == 0, "HL2 control: nothing discarded");

        // Run C: the no-loss path through the DSP is unchanged, and it is a
        // DIFFERENT frame from the clean one.
        std::vector<float> stitched;
        hl2::Hl2RxDsp dspC;
        std::string errC;
        check(makeDsp(dspC, &errC), ("HL2 no-loss DSP configures: " + errC).c_str());
        QObject::connect(&dspC, &hl2::Hl2RxDsp::spectrumReady, &dspC,
                         [&](const std::vector<float>& b) { stitched = b; });
        dspC.processIqBlock(preGap);
        dspC.processIqBlock(postGap);
        check(stitched.size() == static_cast<std::size_t>(N),
              "HL2 no-loss: a frame is emitted");
        check(dspC.spectrumGapDiscards() == 0,
              "HL2 no-loss: nothing is discarded when nothing was lost");
        check(!binsEqual(stitched, cleanOnly),
              "HL2 no-loss: the stitched frame still stitches -- the fix is gap-only");

        // A gap that lands ON a frame boundary has corrupted nothing, and must
        // not be counted. Without this the row degenerates into a second, worse
        // copy of the dropped-packet counter.
        hl2::Hl2RxDsp dspD;
        std::string errD;
        check(makeDsp(dspD, &errD), ("HL2 boundary DSP configures: " + errD).c_str());
        dspD.processIqBlock(postGap);          // exactly N samples: frame completes
        dspD.onSequenceGap();
        check(dspD.spectrumGapDiscards() == 0,
              "HL2: a gap on a frame boundary discards nothing and is not counted");
    }

    // ---- 4b · AnanRxDsp acts on the gap -----------------------------------
    //
    // ANAN's panadapter is WDSP's analyzer. AnanRxDsp stages IQ into
    // 1024-sample blocks for it, and a gap drops the partly staged block --
    // which is what a kPartial-sample pre-gap feed is, so those samples never
    // reach the analyzer at all and the post-gap frame is exactly the frame a
    // stage fed only the post-gap samples produces. A frame needs one full
    // analyzer FFT (16384 samples) and arrives from a WDSP worker thread, so
    // this section feeds that many and waits for it.
    {
        const int rate = 48000;
        constexpr int kAnalyzerFft = 16384;   // AnanPanAnalyzer::kMinFftSize
        auto makeDsp = [&](anan::AnanRxDsp& dsp, std::string* err) {
            anan::AnanRxDsp::Config cfg;
            cfg.inputSampleRateHz = rate;
            cfg.audioSampleRateHz = 24000;
            cfg.dspBlockSize = 1024;
            cfg.panPoints = 1024;
            cfg.mode = WdspChannel::Mode::Usb;
            cfg.filterLowHz = 150.0;
            cfg.filterHighHz = 3000.0;
            cfg.blockForOutput = true;
            return dsp.configure(cfg, err);
        };
        // Feed `iq`, then poll with empty blocks until the first frame lands.
        auto firstFrameAfter = [](anan::AnanRxDsp& dsp,
                                  const std::vector<std::complex<float>>& iq) {
            std::vector<float> frame;
            bool got = false;
            const auto conn = QObject::connect(&dsp, &anan::AnanRxDsp::spectrumReady,
                [&](const std::vector<float>& b) {
                    if (!got) { frame = b; got = true; }
                });
            dsp.processIqBlock(iq);
            const std::vector<std::complex<float>> none;
            QElapsedTimer t;
            t.start();
            while (!got && t.elapsed() < 10000) {
                QThread::msleep(2);
                dsp.processIqBlock(none);
            }
            QObject::disconnect(conn);
            return frame;
        };

        const auto preGap = wireTone(kPartial, 1000.0, rate, 0.5f);
        const auto postGap = wireTone(kAnalyzerFft, 5000.0, rate, 0.5f);

        anan::AnanRxDsp dspA;
        std::string errA;
        check(makeDsp(dspA, &errA), ("ANAN DSP configures: " + errA).c_str());
        check(dspA.spectrumGapDiscards() == 0, "ANAN: no discards before any gap");
        dspA.processIqBlock(preGap);
        dspA.onSequenceGap();
        check(dspA.spectrumGapDiscards() == 1,
              "ANAN: a gap with a partly staged block is counted once");
        const std::vector<float> afterGap = firstFrameAfter(dspA, postGap);
        check(afterGap.size() == 1024u, "ANAN: a frame is emitted after the gap");

        // Equality against a stage fed only the post-gap samples. Each stage's
        // first frame is exactly one FFT (the analyzer seeds its average from
        // it), so any difference would be a surviving pre-gap sample.
        anan::AnanRxDsp dspB;
        std::string errB;
        check(makeDsp(dspB, &errB), ("ANAN control DSP configures: " + errB).c_str());
        const std::vector<float> cleanOnly = firstFrameAfter(dspB, postGap);
        check(binsEqual(afterGap, cleanOnly),
              "ANAN: a sub-block gap (the partly staged block dropped) leaves the "
              "post-gap frame free of pre-gap samples");

        // A WHOLE staging block fed before the gap is already inside the
        // analyzer's input ring, and the analyzer's history is deliberately
        // left alone (AnanRxDsp::onSequenceGap()), so the next 16k-point FFT
        // straddles the seam: the post-gap frame is a documented blend, not a
        // clean frame. Pinned so the tradeoff is stated rather than implied.
        anan::AnanRxDsp dspE;
        std::string errE;
        check(makeDsp(dspE, &errE), ("ANAN whole-block DSP configures: " + errE).c_str());
        dspE.processIqBlock(wireTone(1024 + kPartial, 1000.0, rate, 0.5f));
        dspE.onSequenceGap();
        check(dspE.spectrumGapDiscards() == 1,
              "ANAN whole-block: only the partial block is discarded");
        const std::vector<float> blended = firstFrameAfter(dspE, postGap);
        check(!blended.empty() && !binsEqual(blended, cleanOnly),
              "ANAN: whole blocks fed before a gap stay in the analyzer's ring and "
              "blend into the post-gap frame -- the documented tradeoff, not a purge");

        anan::AnanRxDsp dspC;
        std::string errC;
        check(makeDsp(dspC, &errC), ("ANAN no-loss DSP configures: " + errC).c_str());
        dspC.processIqBlock(preGap);
        const std::vector<float> stitched = firstFrameAfter(dspC, postGap);
        check(dspC.spectrumGapDiscards() == 0,
              "ANAN no-loss: nothing is discarded when nothing was lost");
        check(!stitched.empty() && !binsEqual(stitched, cleanOnly),
              "ANAN no-loss: the stitched frame still stitches -- the fix is gap-only");

        // A whole number of staging blocks leaves nothing in flight.
        anan::AnanRxDsp dspD;
        std::string errD;
        check(makeDsp(dspD, &errD), ("ANAN boundary DSP configures: " + errD).c_str());
        dspD.processIqBlock(postGap);
        dspD.onSequenceGap();
        check(dspD.spectrumGapDiscards() == 0,
              "ANAN: a gap on a block boundary discards nothing and is not counted");
    }

    // ---- 5 · MetisClient emits the gap, and emits it FIRST -----------------
    //
    // The wire edge. Without this the DSP-side machinery above is reachable
    // only from a test.
    {
        using namespace AetherSDR::hl2;

        MetisClient c;
        MetisClientTestAccess::setStreaming(c);

        // The order in which the two signals arrive, recorded as a string so a
        // failure names the actual sequence rather than a bare false.
        std::string order;
        std::vector<quint32> gaps;
        QObject::connect(&c, &MetisClient::rxSequenceGap, &c, [&](quint32 lost) {
            gaps.push_back(lost);
            order += 'G';
        });
        QObject::connect(&c, &MetisClient::iqBlockReady, &c,
                         [&](const std::vector<std::complex<float>>&) { order += 'B'; });

        MetisClientTestAccess::feedDatagram(c, makeEp6(0));
        MetisClientTestAccess::feedDatagram(c, makeEp6(1));
        MetisClientTestAccess::feedDatagram(c, makeEp6(2));
        check(gaps.empty(), "an in-order EP6 run reports no sequence gap");
        check(order == "BBB", "...and delivers three IQ blocks");

        // Three packets missing: 3, 4, 5.
        MetisClientTestAccess::feedDatagram(c, makeEp6(6));
        check(gaps.size() == 1, "a forward skip reports exactly one gap");
        check(!gaps.empty() && gaps[0] == 3, "...carrying the gap's own size, not the total");
        check(c.droppedPackets() == 3, "...and the cumulative counter still agrees");
        // THE ORDERING CONTRACT. The gap must precede the block from the SAME
        // datagram, or a consumer discards the post-gap samples it had just
        // been handed.
        check(order == "BBBGB",
              "the gap is emitted BEFORE the IQ block of the datagram that revealed it");

        // A rewind does not count as forward loss, but its accepted samples
        // must still invalidate the partial spectrum before delivery.
        order.clear();
        MetisClientTestAccess::feedDatagram(c, makeEp6(2));
        check(gaps.size() == 2 && gaps.back() == 0, "rewind notifies continuity without claiming loss");
        check(c.droppedPackets() == 3 && order == "GB", "rewind preserves loss total and precedes samples");
    }

    // ---- 6 · Wire -> DSP continuity, with both production ingest paths ----
    {
        const auto hl2Wire = [](hl2::MetisClient& client, hl2::Hl2RxDsp& dsp, std::string& order) {
            QObject::connect(&client, &hl2::MetisClient::rxSequenceGap, &dsp,
                [&dsp, &order](quint32) { order += 'G'; dsp.onSequenceGap(); });
            QObject::connect(&client, &hl2::MetisClient::iqBlockReady, &dsp,
                [&dsp, &order](const std::vector<std::complex<float>>& block) {
                    order += 'B'; dsp.processIqBlock(block);
                });
        };
        const auto hl2Feed = [](hl2::MetisClient& client, std::uint32_t seq, bool post) {
            hl2::MetisClientTestAccess::feedDatagram(client, makeEp6(seq, post));
        };
        const auto ananWire = [](anan::P2Client& client, anan::AnanRxDsp& dsp, std::string& order) {
            QObject::connect(&client, &anan::P2Client::ddcSequenceGap, &dsp,
                [&dsp, &order](int ddc) {
                    if (ddc == 0) { order += 'G'; dsp.onSequenceGap(); }
                });
            QObject::connect(&client, &anan::P2Client::ddc0IqReady, &dsp,
                [&dsp, &order](const std::vector<std::complex<float>>& block) {
                    order += 'B'; dsp.processIqBlock(block);
                });
        };
        const auto ananFeed = [](anan::P2Client& client, std::uint32_t seq, bool post) {
            anan::P2ClientTestAccess::feedDatagram(client, makeDdc(seq, post), anan::kDdc0DefaultPort);
        };
        struct Case {
            const char* name;
            std::uint32_t first;
            std::uint32_t next;
            bool discontinuous;
            quint64 hl2Drops;
        };
        const Case cases[] = {
            {"forward loss", 0, 11, true, 3},
            {"rewind", 0, 0, true, 0},
            {"duplicate", 0, 7, true, 0},
            {"in order", 0, 8, false, 0},
            {"uint32 wrap", 0xfffffff8u, 0, false, 0},
            {"loss across wrap", 0xfffffff8u, 2, true, 2},
            {"ambiguous half-range jump", 0, 0x80000008u, true, 0},
        };
        for (const Case& c : cases) {
            verifyContinuity<hl2::Hl2RxDsp, hl2::MetisClient>(
                c.name, c.first, c.next, c.discontinuous, c.hl2Drops, hl2Wire, hl2Feed);
            verifyContinuity<anan::AnanRxDsp, anan::P2Client>(
                c.name, c.first, c.next, c.discontinuous, c.discontinuous ? 1 : 0, ananWire, ananFeed);
        }
    }

    // ---- 7 · P2 extraction preserves rejection and per-DDC ownership ----
    {
        using namespace AetherSDR::anan;
        P2Client client;
        P2ClientTestAccess::enableDdcs(client, 2);
        std::vector<int> gaps;
        std::vector<int> blocks;
        int ddc0Blocks = 0;
        int links = 0;
        QObject::connect(&client, &P2Client::ddcSequenceGap, &client,
                         [&](int ddc) { gaps.push_back(ddc); });
        QObject::connect(&client, &P2Client::ddcIqReady, &client,
                         [&](int ddc, const std::vector<std::complex<float>>&) { blocks.push_back(ddc); });
        QObject::connect(&client, &P2Client::ddc0IqReady, &client,
                         [&](const std::vector<std::complex<float>>&) { ++ddc0Blocks; });
        QObject::connect(&client, &P2Client::linkUp, &client, [&] { ++links; });
        P2ClientTestAccess::feedDatagram(client, makeDdc(0), kDdc0DefaultPort + 1);
        P2ClientTestAccess::feedDatagram(client, makeDdc(4), kDdc0DefaultPort + 1);
        check(gaps == std::vector<int>{1}, "DDC1 gap is attributed only to DDC1");
        check(links == 0 && ddc0Blocks == 0, "DDC1 cannot connect or feed DDC0");
        P2ClientTestAccess::feedDatagram(client, makeDdc(99), kDdc0DefaultPort);
        P2ClientTestAccess::feedDatagram(client, makeDdc(100), kDdc0DefaultPort);
        check(gaps.size() == 1 && links == 1 && ddc0Blocks == 2, "DDC0 has its own first/next sequence");
        const auto before = blocks.size();
        P2ClientTestAccess::feedDatagram(client, makeDdc(110), kDdc0DefaultPort + 2);
        std::vector<std::uint8_t> malformed{0, 0, 0};
        P2ClientTestAccess::feedDatagram(client, malformed, kDdc0DefaultPort);
        check(blocks.size() == before && gaps.size() == 1, "invalid port and malformed payload deliver nothing");
        P2ClientTestAccess::feedDatagram(client, makeDdc(101), kDdc0DefaultPort);
        check(gaps.size() == 1 && client.droppedPackets() == 1, "rejected datagrams do not advance sequence state");
    }

    // ---- 8 · Reset also clears the display-rate rolling window ----
    {
        const auto pre = tone(128, 10, 0.5f);
        const auto post = tone(64, 21, 0.5f);
        // HL2 only: ANAN has no rolling window -- its analyzer staging block
        // is covered in 4b.
        hl2::Hl2Spectrum h(64), hc(64);
        std::vector<float> hb, hcb;
        h.accumulate(pre);
        check(h.reset() == 63, "HL2 reset discards capped rolling window");
        h.process(post, hb); hc.process(post, hcb);
        check(binsEqual(hb, hcb), "HL2 accumulated pre-gap data is absent");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "spectrum_sequence_gap_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
