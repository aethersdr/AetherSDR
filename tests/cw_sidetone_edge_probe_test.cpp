// CwSidetoneEdgeProbe: the env-gated, sample-exact envelope capture both CW
// sidetone sinks feed their rendered buffers through (#5200).
//
// The probe is a measurement instrument whose numbers were used to justify the
// Windows callback-sink switch, so its failure modes are not cosmetic: a probe
// that under-reports edges, or that reports them at the wrong sample position,
// produces plausible-looking timing tables that are quietly wrong.
//
// No PortAudio, no audio device, no sink — scan() takes a plain interleaved
// stereo float buffer, so the whole class is exercisable as a pure function of
// the samples fed to it. The probe arms off AETHER_CW_EDGE_PROBE, which this
// test sets itself before constructing each probe.

#include "core/CwSidetoneEdgeProbe.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <cmath>
#include <cstdio>
#include <vector>

namespace AetherSDR {
Q_LOGGING_CATEGORY(lcAudio, "aether.audio")
}

using AetherSDR::CwSidetoneEdgeProbe;

namespace {

int g_failures = 0;

void expect(bool ok, const char* what, long long got, long long want)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, got, want);
        ++g_failures;
    }
}

constexpr int kRate = 48000;

// A file-local pi, deliberately not the <cmath> macro: MSVC only defines that
// when _USE_MATH_DEFINES is set before the include, and this TU pulls none of
// the Qt GUI headers whose qmath.h provides a fallback. No Windows CI job
// builds this target -- ci.yml and windows-installer.yml each build a named
// target list -- so the break would surface only for a Windows developer
// building the full tree. tests/asr_segmenter_test.cpp hit the same thing.
constexpr double kPi = 3.14159265358979323846;

// A 600 Hz tone at `amp` (0 == silence), continuing `phase` across calls so
// consecutive blocks form one continuous waveform, exactly as the generator
// feeds the sinks.
void render(std::vector<float>& buf, int frames, float amp, double& phase)
{
    buf.resize(std::size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float s = amp * float(std::sin(phase));
        phase += 2.0 * kPi * 600.0 / kRate;
        buf[2 * i]     = s;
        buf[2 * i + 1] = s;
    }
}

int ms(int milliseconds) { return kRate * milliseconds / 1000; }

void scanTone(CwSidetoneEdgeProbe& p, int milliseconds, float amp, double& phase)
{
    std::vector<float> b;
    render(b, ms(milliseconds), amp, phase);
    p.scan(b.data(), ms(milliseconds));
}

// ── The disabled probe is free and inert ─────────────────────────────────────
void testDisabledProbeRecordsNothing()
{
    qunsetenv("AETHER_CW_EDGE_PROBE");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    scanTone(p, 60, 0.5f, ph);
    expect(p.edgeCount() == 0, "disabled probe records nothing", p.edgeCount(), 0);
}

// ── Keyed elements produce one rising and one falling edge each ──────────────
void testKeyedElementsProduceEdgePairs()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    for (int i = 0; i < 5; ++i) {
        scanTone(p, 60, 0.5f, ph);   // dit
        scanTone(p, 60, 0.0f, ph);   // inter-element gap
    }
    expect(p.edgeCount() == 10, "5 dits -> 10 edges", p.edgeCount(), 10);
}

// A 5 ms element at 40 WPM is well under the 96-sample quiet run; the probe
// must not need a minimum ON duration to see it.
void testShortElementStillDetected()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    for (int i = 0; i < 3; ++i) {
        scanTone(p, 5,  0.5f, ph);
        scanTone(p, 60, 0.0f, ph);
    }
    expect(p.edgeCount() == 6, "short 5ms elements detected", p.edgeCount(), 6);
}

// ── The regression pin for the dump() early-return reset leak ────────────────
//
// dump() used to `return` on m_count == 0 ABOVE the reset, so a stream that
// recorded no edges leaked its whole sample count into the next stream: after
// 10 s of silence the next stream's first edge reported samplePos 480001
// instead of ~0. "CW mode entered, operator keys a while later" is the common
// path, not an edge case.
//
// edgeCount() cannot see m_samplePos, so the leak is observed through its
// consequence: the probe must be able to start a fresh stream after an empty
// one and behave identically to a probe that never saw the empty stream.
void testEmptyStreamDoesNotLeakIntoTheNext()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    double ph = 0;

    CwSidetoneEdgeProbe p;
    scanTone(p, 10000, 0.0f, ph);               // 10 s, CW mode idle, never keyed
    expect(p.edgeCount() == 0, "silent stream records no edges", p.edgeCount(), 0);
    p.dump("empty", kRate);                      // must reset despite recording nothing

    // A dit at sample 0 of the NEXT stream. Its rising edge must be reported
    // near 0, not near 480000 — the probe's positions are documented as the
    // stream's own clock, and the stream just started.
    scanTone(p, 60, 0.5f, ph);
    expect(p.edgeCount() == 1, "new stream records the dit's rising edge",
           p.edgeCount(), 1);
    const long long pos = p.edgeCount() > 0 ? p.edgeAt(0).samplePos : -1;
    // Tolerance covers the ramp up to kThreshold, not a leaked stream offset:
    // the pre-fix value here was 480001 (the full 10 s of silence).
    expect(pos >= 0 && pos < ms(5), "rising edge lands at the new stream's start",
           pos, 0);
}

// dump() must reset after a NON-empty stream too — the path that always worked,
// pinned so the refactor that fixed the empty case cannot break this one.
void testDumpResetsAfterRecordedEdges()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    scanTone(p, 60, 0.5f, ph);
    scanTone(p, 60, 0.0f, ph);
    expect(p.edgeCount() == 2, "one dit -> 2 edges", p.edgeCount(), 2);
    p.dump("recorded", kRate);
    expect(p.edgeCount() == 0, "dump() resets a non-empty probe", p.edgeCount(), 0);
}

// ── Threshold behaviour, stated rather than assumed ──────────────────────────
//
// kThreshold is absolute (0.02) while the sidetone level is operator-adjustable,
// so a quiet sidetone is invisible to the probe. That is a real limitation, not
// a bug — this pins it so it stays a deliberate property, and so the
// "armed, but nothing crossed the threshold" line in dump() keeps a reason to
// exist.
void testSubThresholdSidetoneRecordsNothing()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    scanTone(p, 60, 0.015f, ph);
    expect(p.edgeCount() == 0, "sub-threshold tone records no edges",
           p.edgeCount(), 0);
    p.dump("quiet", kRate);   // must not crash, and must reset
}

// A tone crosses zero every ~40 samples at 600 Hz / 48 kHz, far short of the
// 96-sample quiet run, so instantaneous silence inside a cycle must never
// register as key-up. This is the assumption the whole falling-edge rule rests
// on, so it is pinned rather than left to the header comment.
void testZeroCrossingsDoNotRegisterAsKeyUp()
{
    qputenv("AETHER_CW_EDGE_PROBE", "1");
    CwSidetoneEdgeProbe p;
    double ph = 0;
    scanTone(p, 500, 0.5f, ph);   // 500 ms of continuous tone
    expect(p.edgeCount() == 1, "continuous tone -> exactly one rising edge",
           p.edgeCount(), 1);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testDisabledProbeRecordsNothing();
    testKeyedElementsProduceEdgePairs();
    testShortElementStillDetected();
    testEmptyStreamDoesNotLeakIntoTheNext();
    testDumpResetsAfterRecordedEdges();
    testSubThresholdSidetoneRecordsNothing();
    testZeroCrossingsDoNotRegisterAsKeyUp();

    if (g_failures) {
        std::fprintf(stderr, "cw_sidetone_edge_probe_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("cw_sidetone_edge_probe_test: all checks passed\n");
    return 0;
}
