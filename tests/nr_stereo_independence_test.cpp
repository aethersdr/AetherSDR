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

#include <cstdio>

using namespace AetherSDR;
using NrStereoIndependence::MakeProcess;
using NrStereoIndependence::Process;

namespace {

int g_failures = 0;

void check(bool condition, const QString& name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", qPrintable(name));
    if (!condition) {
        ++g_failures;
    }
}

void checkMethod(const char* method, int rate, const MakeProcess& make)
{
    const QString label = QStringLiteral("%1 @ %2 Hz: ").arg(QLatin1String(method)).arg(rate);
    check(NrStereoIndependence::leftIgnoresRight(make, rate),
          label + QStringLiteral("left output ignores right input"));
    check(NrStereoIndependence::rightIgnoresLeft(make, rate),
          label + QStringLiteral("right output ignores left input"));
    check(NrStereoIndependence::panStepSettles(make, rate),
          label + QStringLiteral("hard pan step settles within latency"));
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

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    for (const int rate : {24000, 48000}) {
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

#ifdef HAVE_SPECBLEACH
        checkMethod("NR4", rate, makeWrapper<SpecbleachFilter>(rate));
#endif

        if (NnrFilter(rate).isValid()) {
            checkMethod("NNR", rate, makeWrapper<NnrFilter>(rate));
        } else {
            std::printf("SKIP NNR @ %d Hz: create_nnr() failed\n", rate);
        }

#ifdef HAVE_DFNR
        if (DeepFilterFilter(rate).isValid()) {
            checkMethod("DFNR", rate, makeWrapper<DeepFilterFilter>(rate));
        } else {
            std::printf("SKIP DFNR @ %d Hz: model not found\n", rate);
        }
#endif

#ifdef HAVE_NVIDIA_AFX
        const QString pack = qEnvironmentVariable("AETHER_NVAFX_DIR");
        if (!pack.isEmpty() && NvidiaAfxFilter(pack, rate).isValid()) {
            checkMethod("BNR", rate, makeWrapper<NvidiaAfxFilter>(pack, rate));
        } else {
            std::printf("SKIP BNR @ %d Hz: set AETHER_NVAFX_DIR to a working AFX pack\n",
                        rate);
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
