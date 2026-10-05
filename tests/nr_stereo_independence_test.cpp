// Every client RX noise-reduction method denoises L and R independently, the
// way RN2 does: one side's output never depends on the other side's input, and
// a hard pan change reaches the output within the filter's own latency. See
// nr_stereo_independence.h for what each check rules out.
//
// NR2, NR4 and NNR always run. DFNR runs when its model is found beside the
// executable. BNR needs a real NVIDIA GPU and AFX pack, so it runs only when
// AETHER_NVAFX_DIR points at one; nr_rate_domain_test covers both wrappers on
// every platform through deterministic stand-in C APIs.

#include "core/NnrFilter.h"
#include "core/SpectralNR.h"
#ifdef HAVE_SPECBLEACH
#include "core/SpecbleachFilter.h"
#endif
#ifdef HAVE_DFNR
#include "core/DeepFilterFilter.h"
#endif
#ifdef HAVE_NVIDIA_AFX
#include "core/NvidiaAfxFilter.h"
#endif

#include "nr_stereo_independence.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstdio>
#include <cstring>
#include <optional>

using namespace AetherSDR;
using NrStereoIndependence::MakeProcess;
using NrStereoIndependence::Process;

namespace {

int g_failures = 0;

void check(bool condition, const QString& name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", qPrintable(name));
    std::fflush(stdout);
    if (!condition) {
        ++g_failures;
    }
}

enum class Method { Nr2, Nr4, Nnr, Dfnr, Bnr };

struct Selection {
    std::optional<Method> method;
    std::optional<int> rate;
};

void printUsage(const char* program)
{
    std::fprintf(stderr, "usage: %s [--method=nr2|nr4|nnr|dfnr|bnr] [--rate=24000|48000]\n",
                 program);
}

std::optional<Method> parseMethod(const char* value)
{
    if (std::strcmp(value, "nr2") == 0) { return Method::Nr2; }
    if (std::strcmp(value, "nr4") == 0) { return Method::Nr4; }
    if (std::strcmp(value, "nnr") == 0) { return Method::Nnr; }
    if (std::strcmp(value, "dfnr") == 0) { return Method::Dfnr; }
    if (std::strcmp(value, "bnr") == 0) { return Method::Bnr; }
    return std::nullopt;
}

std::optional<Selection> parseSelection(int argc, char** argv)
{
    Selection selection;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (std::strncmp(argument, "--method=", 9) == 0 && !selection.method) {
            selection.method = parseMethod(argument + 9);
            if (!selection.method) { return std::nullopt; }
            continue;
        }
        if (std::strncmp(argument, "--rate=", 7) == 0 && !selection.rate) {
            if (std::strcmp(argument + 7, "24000") == 0) {
                selection.rate = 24000;
            } else if (std::strcmp(argument + 7, "48000") == 0) {
                selection.rate = 48000;
            } else {
                return std::nullopt;
            }
            continue;
        }
        return std::nullopt;
    }
    return selection;
}

bool selected(const Selection& selection, Method method)
{
    return !selection.method || *selection.method == method;
}

void printStage(const char* state, const char* method, int rate, const QElapsedTimer& elapsed)
{
    std::fprintf(stderr, "STAGE %s method=%s rate=%d elapsed_ms=%lld\n", state, method, rate,
                 static_cast<long long>(elapsed.elapsed()));
    std::fflush(stderr);
}

template <class Function>
void runMethodStage(const char* method, int rate, Function function)
{
    QElapsedTimer elapsed;
    elapsed.start();
    printStage("start", method, rate, elapsed);
    function();
    printStage("finish", method, rate, elapsed);
}

void checkMethod(const char* method, int rate, const MakeProcess& make)
{
    const QString label = QStringLiteral("%1 @ %2 Hz: ").arg(QLatin1String(method)).arg(rate);
    check(NrStereoIndependence::leftIgnoresRight(make, rate),
          label + QStringLiteral("left output ignores right input"));
    check(NrStereoIndependence::rightIgnoresLeft(make, rate),
          label + QStringLiteral("right output ignores left input"));
    check(NrStereoIndependence::panStepSettles(make, rate),
          label + QStringLiteral("hard pan step settles within 300 ms"));
    check(NrStereoIndependence::attenuatesNoise(make, rate),
          label + QStringLiteral("both channels attenuate noise"));
}

// Wrap a filter that owns its state behind a shared_ptr so the Process
// callable is copyable.
template <class Filter, class... Args>
MakeProcess makeWrapper(Args... args)
{
    return [=]() -> Process {
        auto filter = std::make_shared<Filter>(args...);
        return [filter](const QByteArray& pcm) { return filter->process(pcm); };
    };
}

#ifdef HAVE_NVIDIA_AFX
// While any denoiser effect keeps the SDK loaded, an effect created after
// another was destroyed can inherit that effect's leftover state: its first
// ~0.5 s then differs from a fresh start by up to the full signal level.
// NvidiaAfxFilter resets every effect after Load; this pins that.
bool bnrRecycledEffectStartsClean(const QString& pack, int rate)
{
    const QByteArray input = NrStereoIndependence::makeStereo(
        rate, rate * 2, {0x5eedu, 0.25f, 0.05f}, {0x5eee, 0.10f, 0.10f});
    const auto run = [&]() {
        auto filter = std::make_shared<NvidiaAfxFilter>(pack, rate);
        return NrStereoIndependence::runBlocks(
            [filter](const QByteArray& pcm) { return filter->process(pcm); }, input);
    };
    const QByteArray fresh = run();
    NvidiaAfxFilter keepAlive(pack, rate);
    run();                                  // created, used, destroyed
    return run() == fresh;                  // reuses what that one left behind
}
#endif

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Selection> selection = parseSelection(argc, argv);
    if (!selection) {
        printUsage(argv[0]);
        return 2;
    }
    QCoreApplication app(argc, argv);

    for (const int rate : {24000, 48000}) {
        if (selection->rate && *selection->rate != rate) {
            continue;
        }
        if (selected(*selection, Method::Nr2)) {
            runMethodStage("nr2", rate, [rate] {
                checkMethod("NR2", rate, [rate]() -> Process {
                    auto nr2 = std::make_shared<SpectralNR>(1024, rate, 4);
                    return [nr2](const QByteArray& pcm) {
                        QByteArray out(pcm.size(), Qt::Uninitialized);
                        nr2->processStereo(reinterpret_cast<const float*>(pcm.constData()),
                                           reinterpret_cast<float*>(out.data()),
                                           pcm.size() / (2 * static_cast<int>(sizeof(float))));
                        return out;
                    };
                });
            });
        }

#ifdef HAVE_SPECBLEACH
        if (selected(*selection, Method::Nr4)) {
            runMethodStage("nr4", rate, [rate] {
                checkMethod("NR4", rate, makeWrapper<SpecbleachFilter>(rate));
            });
        }
#else
        if (selection->method == Method::Nr4) {
            std::printf("SKIP NR4 @ %d Hz: not built with Specbleach\n", rate);
            return g_failures != 0 ? 1 : 77;
        }
#endif

        if (selected(*selection, Method::Nnr) && NnrFilter(rate).isValid()) {
            runMethodStage("nnr", rate, [rate] {
                checkMethod("NNR", rate, makeWrapper<NnrFilter>(rate));
            });
        } else if (selected(*selection, Method::Nnr)) {
            std::printf("SKIP NNR @ %d Hz: create_nnr() failed\n", rate);
            if (selection->method) {
                return g_failures != 0 ? 1 : 77;
            }
        }

#ifdef HAVE_DFNR
        if (selected(*selection, Method::Dfnr) && DeepFilterFilter(rate).isValid()) {
            runMethodStage("dfnr", rate, [rate] {
                checkMethod("DFNR", rate, makeWrapper<DeepFilterFilter>(rate));
            });
        } else if (selected(*selection, Method::Dfnr)) {
            std::printf("SKIP DFNR @ %d Hz: model not found\n", rate);
            if (selection->method) {
                return g_failures != 0 ? 1 : 77;
            }
        }
#else
        if (selection->method == Method::Dfnr) {
            std::printf("SKIP DFNR @ %d Hz: not built with DeepFilterNet\n", rate);
            return g_failures != 0 ? 1 : 77;
        }
#endif

#ifdef HAVE_NVIDIA_AFX
        const QString pack = qEnvironmentVariable("AETHER_NVAFX_DIR");
        if (selected(*selection, Method::Bnr) && !pack.isEmpty()
            && NvidiaAfxFilter(pack, rate).isValid()) {
            runMethodStage("bnr", rate, [&pack, rate] {
                checkMethod("BNR", rate, makeWrapper<NvidiaAfxFilter>(pack, rate));
                check(bnrRecycledEffectStartsClean(pack, rate),
                      QStringLiteral("BNR @ %1 Hz: a filter created while another is alive, "
                                     "after a third was destroyed, matches a fresh one").arg(rate));
            });
        } else if (selected(*selection, Method::Bnr)) {
            std::printf("SKIP BNR @ %d Hz: set AETHER_NVAFX_DIR to a working AFX pack\n",
                        rate);
            if (selection->method) {
                return g_failures != 0 ? 1 : 77;
            }
        }
#else
        if (selection->method == Method::Bnr) {
            std::printf("SKIP BNR @ %d Hz: set AETHER_NVAFX_DIR to a working AFX pack\n",
                        rate);
            return g_failures != 0 ? 1 : 77;
        }
#endif
    }

    if (g_failures != 0) {
        std::printf("nr_stereo_independence_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nr_stereo_independence_test passed\n");
    return 0;
}
