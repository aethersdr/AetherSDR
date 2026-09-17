// WdspChannel::processIq() distinguishes six outcomes. Until this test's
// change, Hl2RxDsp::processIqBlock and AnanRxDsp::processIqBlock collapsed all
// five non-`Ok` ones into a bare `continue` — no log line, no counter, no
// signal — so a chain producing no audio because WDSP was returning
// `EngineError` on every block was indistinguishable, from outside, from a
// chain whose pipeline was still filling. (S3-raw-iq-hardening §6.1; the
// study's own acceptance item U4.)
//
// WHAT THIS PINS, in three layers, because no one layer is enough:
//
//   1. THE RULE.  WdspProcessTally puts each of the six outcomes in its own
//      counter, and `faults()` contains the four that mean something is wrong
//      and NOT `Underrun`, which is normal. A counter is exactly the shape
//      that passes whatever the code does, so the rule is asserted per
//      enumerator rather than in aggregate.
//
//   2. THE WIRING, on a real HL2 chain and a real ANAN chain.  Both call
//      sites must actually reach the tally, and must reach it on EVERY block
//      including the ones that produced no audio: `ok + underrun + faults`
//      has to equal the number of blocks the stage handed to processIq(),
//      which this test computes independently from the sample count it fed.
//      Both backends, because a fix present in one and absent in the other is
//      worse than absent in both.
//
//   3. THAT `Underrun` IS NOT TREATED AS A FAULT.  A healthy chain underruns
//      — that is what the first blocks through a freshly opened WDSP channel
//      do — and must emit no warning for it. Asserted by counting the log
//      lines the change adds, not by reading the code.
//
// WHAT IT DOES NOT PIN, and why. `AllocationViolation`, `EngineError`,
// `InvalidBuffer` and `Busy` are not PRODUCED here through a live chain.
// Nothing in this codebase can make fexchange2 return a non-(-2) error or make
// WDSP allocate on demand; `InvalidBuffer` needs a span geometry
// processIqBlock() sizes itself and never gets wrong; and `Busy` needs a
// control operation in flight on a second thread, which §5.1's single-thread
// model makes unreachable on the HL2 path at all (S3 §9.4 says so in those
// words). Layer 1 therefore pins the CLASSIFICATION of all six by handing the
// tally each enumerator directly, and layer 2 pins that the call site hands it
// whatever processIq() actually returned. NOTHING HERE WAS MEASURED ON A
// RADIO: there is no hardware in this test and no simulator either.

#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples
#include "core/dsp/WdspProcessTally.h"

#include <QCoreApplication>
#include <QtGlobal>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;

// ── Counting the warning lines the fix adds ───────────────────────────────
//
// Matched on the message text rather than on the logging category, because
// the two backends log under two different categories and the assertion is the
// same for both.
static int g_processWarnings = 0;
static QtMessageHandler g_previousHandler = nullptr;

static void countingHandler(QtMsgType type, const QMessageLogContext& ctx,
                            const QString& msg)
{
    if (type == QtWarningMsg && msg.contains(QStringLiteral("WDSP processIq failed")))
        ++g_processWarnings;
    if (g_previousHandler)
        g_previousHandler(type, ctx, msg);
}

// ── Layer 1: the rule ─────────────────────────────────────────────────────

static void testClassification()
{
    using PR = WdspChannel::ProcessResult;

    // Each enumerator lands in its OWN counter. Asserted one at a time on a
    // fresh tally: a single tally fed all six and checked at the end would
    // pass even if two outcomes shared a counter, as long as the totals added
    // up — which is exactly the bug class this file exists to prevent.
    {
        WdspProcessTally t;
        t.record(PR::Ok);
        const auto c = t.snapshot();
        check(c.ok == 1 && c.underrun == 0 && c.faults() == 0, "Ok counts as Ok only");
        check(c.blocks() == 1, "Ok counts toward blocks()");
    }
    {
        WdspProcessTally t;
        t.record(PR::Underrun);
        const auto c = t.snapshot();
        // THE LOAD-BEARING ASSERTION of the whole file. Underrun is normal;
        // folding it into the fault total puts a four-figure number in front
        // of a reader on every healthy connect and teaches them to ignore the
        // row.
        check(c.underrun == 1, "Underrun counts as Underrun");
        check(c.faults() == 0, "Underrun is NOT a fault");
        check(c.ok == 0, "Underrun is not success either");
        check(c.blocks() == 1, "Underrun counts toward blocks()");
    }
    {
        WdspProcessTally t;
        t.record(PR::Busy);
        const auto c = t.snapshot();
        check(c.busy == 1 && c.faults() == 1, "Busy is a fault, in its own counter");
        check(c.underrun == 0 && c.ok == 0, "Busy is neither an underrun nor a success");
    }
    {
        WdspProcessTally t;
        t.record(PR::InvalidBuffer);
        const auto c = t.snapshot();
        check(c.invalidBuffer == 1 && c.faults() == 1,
              "InvalidBuffer is a fault, in its own counter");
        check(c.underrun == 0, "InvalidBuffer is not an underrun");
    }
    {
        WdspProcessTally t;
        t.record(PR::AllocationViolation);
        const auto c = t.snapshot();
        // The one WdspChannel builds a whole allocation-sequence guard around
        // fexchange2 to detect. Discarding it one frame above was the specific
        // complaint in S3 §6.1.
        check(c.allocationViolation == 1 && c.faults() == 1,
              "AllocationViolation is a fault, in its own counter");
        check(c.underrun == 0, "AllocationViolation is not an underrun");
    }
    {
        WdspProcessTally t;
        t.record(PR::EngineError);
        const auto c = t.snapshot();
        check(c.engineError == 1 && c.faults() == 1,
              "EngineError is a fault, in its own counter");
        check(c.underrun == 0, "EngineError is not an underrun");
    }

    // Accumulation, and that the six buckets stay independent under mixed
    // traffic at different multiplicities.
    {
        WdspProcessTally t;
        for (int i = 0; i < 7; ++i) t.record(PR::Ok);
        for (int i = 0; i < 5; ++i) t.record(PR::Underrun);
        for (int i = 0; i < 3; ++i) t.record(PR::EngineError);
        t.record(PR::Busy);
        t.record(PR::InvalidBuffer);
        t.record(PR::AllocationViolation);
        const auto c = t.snapshot();
        check(c.ok == 7, "Ok accumulates");
        check(c.underrun == 5, "Underrun accumulates");
        check(c.engineError == 3, "EngineError accumulates");
        check(c.busy == 1 && c.invalidBuffer == 1 && c.allocationViolation == 1,
              "the singleton faults accumulate");
        check(c.faults() == 6, "faults() sums the four fault kinds and nothing else");
        check(c.blocks() == 18, "blocks() sums all six");
    }

    // record() returns the new value of the counter it touched — that return
    // is what drives the bounded log schedule at the call site, so it is a
    // contract and not an implementation detail.
    {
        WdspProcessTally t;
        check(t.record(PR::EngineError) == 1, "record() returns the first occurrence as 1");
        check(t.record(PR::EngineError) == 2, "record() returns the second as 2");
        check(t.record(PR::Busy) == 1, "record() counts each kind independently");
    }

    // The log schedule: first occurrence of every kind, then progressively
    // less often, so a fault repeating at the block rate cannot put 47
    // warnings a second on the thread that also paces EP2.
    check(!WdspProcessTally::shouldLog(0), "occurrence 0 does not exist and does not log");
    check(WdspProcessTally::shouldLog(1), "the FIRST occurrence always logs");
    check(WdspProcessTally::shouldLog(2), "occurrence 2 logs");
    check(!WdspProcessTally::shouldLog(3), "occurrence 3 does not log");
    check(WdspProcessTally::shouldLog(4), "occurrence 4 logs");
    check(!WdspProcessTally::shouldLog(5) && !WdspProcessTally::shouldLog(6)
              && !WdspProcessTally::shouldLog(7), "occurrences 5-7 do not log");
    check(WdspProcessTally::shouldLog(8) && WdspProcessTally::shouldLog(1024),
          "the schedule stays on powers of two");
    check(!WdspProcessTally::shouldLog(1000), "1000 is not a power of two");

    // The six outcomes must be DISTINGUISHABLE in a log line and a health
    // row, which is the whole complaint in §6.1 restated: five of them used to
    // read identically because none of them read at all.
    const char* names[] = {
        WdspProcessTally::name(PR::Ok),
        WdspProcessTally::name(PR::Underrun),
        WdspProcessTally::name(PR::Busy),
        WdspProcessTally::name(PR::InvalidBuffer),
        WdspProcessTally::name(PR::AllocationViolation),
        WdspProcessTally::name(PR::EngineError),
    };
    for (std::size_t a = 0; a < std::size(names); ++a) {
        check(names[a] != nullptr && names[a][0] != '\0', "every outcome has a name");
        for (std::size_t b = a + 1; b < std::size(names); ++b)
            check(std::strcmp(names[a], names[b]) != 0,
                  "no two outcomes share a name");
    }

    // A SEVENTH ProcessResult must not land silently in a bucket. record()'s
    // switch carries no `default:`, so -Wswitch flags it at compile time; this
    // is the runtime half of the same guard, for a build that is not treating
    // that warning as an error. If this starts failing, someone added an
    // outcome and WdspProcessTally has not been taught about it.
    check(std::strcmp(WdspProcessTally::name(static_cast<PR>(6)), "unknown") == 0,
          "ProcessResult still has exactly six values (add a counter for the new one)");
}

// ── Layer 2: the wiring, on real chains ───────────────────────────────────

// Feed `seconds` of a 1 kHz tone, in wire order, as EP6-shaped 126-sample
// blocks. Returns how many samples were delivered, so the caller can compute
// the number of DSP blocks independently of the counter under test.
template <typename Dsp>
static std::size_t feedTone(Dsp& dsp, int rateHz, double seconds)
{
    const double f = 1000.0;
    const int total = static_cast<int>(rateHz * seconds);
    std::vector<std::complex<float>> blk;
    blk.reserve(AetherSDR::hl2::kEp6BlockSamples);
    for (int n = 0; n < total; ++n) {
        const double ph = 2.0 * kPi * f * n / rateHz;
        // NEGATIVE sine: the HPSDR wire is the conjugate of the analytic
        // convention, and both stages feed the demodulator the raw wire.
        // See HERMES.md §16 and hl2_rxdsp_rate_test's note.
        blk.emplace_back(0.3f * static_cast<float>(std::cos(ph)),
                         0.3f * static_cast<float>(-std::sin(ph)));
        if (static_cast<int>(blk.size()) == AetherSDR::hl2::kEp6BlockSamples) {
            dsp.processIqBlock(blk);
            blk.clear();
        }
    }
    if (!blk.empty())
        dsp.processIqBlock(blk);
    return static_cast<std::size_t>(total);
}

// Configure one stage, feed it half a second of tone, and return both the
// counts it accumulated and the number of blocks it must have processed —
// computed from the sample count, independently of anything the stage says
// about itself.
template <typename Dsp, typename Cfg>
static bool runChain(Dsp& dsp, Cfg cfg, bool blockForOutput,
                     WdspProcessTally::Counts* out, std::uint64_t* expected,
                     int* warnings)
{
    cfg.inputSampleRateHz = 48000;
    cfg.audioSampleRateHz = 24000;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 1024;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    cfg.blockForOutput = blockForOutput;

    // Before any IQ: a chain that has never processed a block reports zeros
    // rather than looking healthy. Hl2Backend::healthSnapshot() reads
    // blocks() == 0 as "not reported" for exactly this reason.
    check(dsp.processTally().blocks() == 0, "the tally starts empty");

    std::string err;
    if (!dsp.configure(cfg, &err)) {
        std::fprintf(stderr, "FAIL: chain did not configure: %s\n", err.c_str());
        ++g_failures;
        return false;
    }
    check(dsp.processTally().blocks() == 0, "configure() alone processes no blocks");

    const int before = g_processWarnings;
    const std::size_t fed = feedTone(dsp, cfg.inputSampleRateHz, 0.5);
    *out = dsp.processTally();
    *expected = fed / static_cast<std::size_t>(cfg.dspBlockSize);
    *warnings = g_processWarnings - before;
    return true;
}

// The assertions that hold for BOTH stages in BOTH configurations. Named
// separately because the reason the ANAN is in this test at all is that a
// reader who finds the counter on one of two near-identical stages will assume
// the other has it; the way to keep that assumption true is to assert exactly
// the same things about both rather than to document the intent.
static void checkCommon(const char* who, const WdspProcessTally::Counts& c,
                        std::uint64_t expected, int warnings)
{
    std::fprintf(stderr, "%-22s blocks=%llu (expected %llu) ok=%llu underrun=%llu "
                         "faults=%llu warnings=%d\n",
                 who,
                 static_cast<unsigned long long>(c.blocks()),
                 static_cast<unsigned long long>(expected),
                 static_cast<unsigned long long>(c.ok),
                 static_cast<unsigned long long>(c.underrun),
                 static_cast<unsigned long long>(c.faults()),
                 warnings);
    // THE WIRING ASSERTION. The stage consumes whole dspBlockSize blocks out
    // of its accumulator and hands each one to processIq(); the number of
    // outcomes recorded must equal the number of calls made — including the
    // calls that produced no audio, which are precisely the ones the old bare
    // `continue` made invisible.
    check(c.blocks() == expected,
          "counts EVERY block handed to processIq(), not only the ones that "
          "produced audio");
    check(c.faults() == 0, "no processing FAULT on a chain that is working");
    check(c.ok + c.underrun == c.blocks(),
          "every block on a healthy chain is either Ok or Underrun");
    // Layer 3: a healthy chain underruns, and must say nothing about it. A log
    // line per underrun would be ~47 a second on the thread that also paces
    // EP2 and would bury the four outcomes worth reading.
    check(warnings == 0, "no warning logged for a healthy chain");
}

// blockForOutput FALSE — production. fexchange2 returns -2 whenever the
// asynchronous output side has nothing ready, which the FIRST block through a
// freshly opened channel never does, so `Underrun` is reachable here for real
// rather than only through the classification test above.
//
// `ok` is deliberately NOT asserted non-zero in this configuration: how many
// blocks the async worker completes during an offline burst feed depends on
// how busy the machine is, and an assertion on it would be a load-sensitive
// flake. The blocking pass below pins the `Ok` side deterministically instead.
static void testUnderrunIsReached()
{
    {
        AetherSDR::hl2::Hl2RxDsp dsp;
        WdspProcessTally::Counts c;
        std::uint64_t expected = 0;
        int warnings = 0;
        if (runChain(dsp, AetherSDR::hl2::Hl2RxDsp::Config{}, false, &c, &expected,
                     &warnings)) {
            checkCommon("HL2  non-blocking :", c, expected, warnings);
            check(c.underrun > 0,
                  "HL2 underran while the pipeline filled, and counted it as an "
                  "underrun rather than as a fault");
        }
    }
    {
        AetherSDR::anan::AnanRxDsp dsp;
        WdspProcessTally::Counts c;
        std::uint64_t expected = 0;
        int warnings = 0;
        if (runChain(dsp, AetherSDR::anan::AnanRxDsp::Config{}, false, &c, &expected,
                     &warnings)) {
            checkCommon("ANAN non-blocking :", c, expected, warnings);
            check(c.underrun > 0,
                  "ANAN underran while the pipeline filled, and counted it as an "
                  "underrun rather than as a fault");
        }
    }
}

// blockForOutput TRUE — the deterministic offline feed the other RX-DSP tests
// use. processIq() waits for each output block, so every call returns `Ok` and
// the `Ok` counter is pinned exactly rather than as "more than zero".
static void testOkIsCounted()
{
    {
        AetherSDR::hl2::Hl2RxDsp dsp;
        WdspProcessTally::Counts c;
        std::uint64_t expected = 0;
        int warnings = 0;
        if (runChain(dsp, AetherSDR::hl2::Hl2RxDsp::Config{}, true, &c, &expected,
                     &warnings)) {
            checkCommon("HL2  blocking     :", c, expected, warnings);
            check(c.ok == expected, "HL2 counted every blocking block as Ok");
            check(c.underrun == 0, "HL2 cannot underrun when it waits for output");
        }
    }
    {
        AetherSDR::anan::AnanRxDsp dsp;
        WdspProcessTally::Counts c;
        std::uint64_t expected = 0;
        int warnings = 0;
        if (runChain(dsp, AetherSDR::anan::AnanRxDsp::Config{}, true, &c, &expected,
                     &warnings)) {
            checkCommon("ANAN blocking     :", c, expected, warnings);
            check(c.ok == expected, "ANAN counted every blocking block as Ok");
            check(c.underrun == 0, "ANAN cannot underrun when it waits for output");
        }
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    g_previousHandler = qInstallMessageHandler(countingHandler);

    testClassification();
    testUnderrunIsReached();
    testOkIsCounted();

    qInstallMessageHandler(g_previousHandler);

    if (g_failures == 0)
        std::fprintf(stderr, "wdsp_process_tally_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
