// Exercises the actual DFNR/NVIDIA wrapper source against deterministic
// local C APIs. No SDK pack, model inference, network, or GPU is used.
#include "core/DeepFilterFilter.h"
#include "core/NvidiaAfxFilter.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

unsigned aetherTestDfProcessedSamples();

namespace {
int g_failures{0};
void check(bool condition, const char* name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++g_failures;
    }
}

QByteArray tone(int rate, int first, int frames)
{
    QByteArray block(frames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(block.data());
    const double hz = rate == 48000 ? 16000.0 : 4000.0;
    for (int frame = 0; frame < frames; ++frame) {
        const float value = 0.2f * std::sin(
            2.0 * std::numbers::pi * hz * (first + frame) / rate);
        samples[2 * frame] = value;
        samples[2 * frame + 1] = value;
    }
    return block;
}

template<class Filter>
QByteArray run(Filter& filter, int rate, bool irregular = true)
{
    constexpr std::array<int, 5> kPartitions{13, 511, 960, 73, 480};
    QByteArray result;
    const int total = rate * 2;
    for (int first = 0, part = 0; first < total; ++part) {
        const int frames = std::min(total - first,
            irregular ? kPartitions[part % kPartitions.size()] : 480);
        const QByteArray input = tone(rate, first, frames);
        const QByteArray output = filter.process(input);
        if (output.size() != input.size()) {
            check(false, "wrapper preserves sample-frame count");
            return {};
        }
        result.append(output);
        first += frames;
    }
    return result;
}

double rmsTail(const QByteArray& data, int rate)
{
    const auto* samples = reinterpret_cast<const float*>(data.constData());
    const int frames = data.size() / (2 * static_cast<int>(sizeof(float)));
    double power = 0;
    for (int frame = rate; frame < frames; ++frame) {
        if (!std::isfinite(samples[2 * frame])
            || samples[2 * frame] != samples[2 * frame + 1]) {
            return -1;
        }
        power += static_cast<double>(samples[2 * frame]) * samples[2 * frame];
    }
    return frames > rate ? std::sqrt(power / (frames - rate)) : -1;
}

bool writeFixture(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("test C API fixture") > 0;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir state;
    if (!state.isValid() || argc != 2) {
        return 1;
    }
    // CMake gives this test its own executable directory. The model lookup
    // follows the production path, but the linked C API reads no model bytes.
    const QString fixture = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("DeepFilterNet3_onnx.dfmodel"));
    const bool createdFixture = !QFile::exists(fixture);
    if (createdFixture && !writeFixture(fixture)) {
        return 1;
    }

    const QString pack = state.path() + QStringLiteral("/pack");
#if defined(_WIN32)
    const QString libraryPath = pack + QStringLiteral("/bin/NVAudioEffects.dll");
#else
    const QString libraryPath = pack + QStringLiteral("/nvafx/lib/libnv_audiofx.so");
#endif
    QDir().mkpath(QFileInfo(libraryPath).absolutePath());
    if (!QFile::copy(QString::fromLocal8Bit(argv[1]), libraryPath)
        || !writeFixture(pack + QStringLiteral(
            "/features/denoiser/models/sm_test/denoiser_48k.trtpkg"))) {
        return 1;
    }
    QLibrary library(libraryPath);
    library.load();
    using Samples = unsigned (*)();
    const Samples nvSamples = reinterpret_cast<Samples>(
        library.resolve("aetherTestProcessedSamples"));
    if (!nvSamples) {
        return 1;
    }

    using AetherSDR::DeepFilterFilter;
    using AetherSDR::NvidiaAfxFilter;
    DeepFilterFilter invalidDf(44100);
    NvidiaAfxFilter invalidNv(pack, 44100);
    check(!invalidDf.isValid() && !invalidNv.isValid(), "unsupported domains fail initialization");

    DeepFilterFilter legacyDf;
    DeepFilterFilter explicitDf(24000);
    check(legacyDf.isValid() && explicitDf.isValid(), "DFNR C API test instances initialize");
    const QByteArray baselineDf = run(legacyDf, 24000);
    check(baselineDf == run(explicitDf, 24000), "DFNR default and explicit24 match bit-for-bit");
    NvidiaAfxFilter legacyNv(pack);
    NvidiaAfxFilter explicitNv(pack, 24000);
    check(legacyNv.isValid() && explicitNv.isValid(), "NVIDIA C API test instances initialize");
    const QByteArray baselineNv = run(legacyNv, 24000);
    check(baselineNv == run(explicitNv, 24000), "NVIDIA default and explicit24 match bit-for-bit");

    DeepFilterFilter nativeDf(48000);
    NvidiaAfxFilter nativeNv(pack, 48000);
    check(nativeDf.sampleRate() == 48000 && nativeNv.sampleRate() == 48000,
          "native wrappers keep immutable48 domain");
    const unsigned dfBefore = aetherTestDfProcessedSamples();
    const unsigned nvBefore = nvSamples();
    const QByteArray df48 = run(nativeDf, 48000);
    const QByteArray nv48 = run(nativeNv, 48000);
    check(aetherTestDfProcessedSamples() - dfBefore == 96000,
          "DFNR processes each native input sample exactly once");
    check(nvSamples() - nvBefore == 96000,
          "NVIDIA processes each native input sample exactly once");
    check(std::abs(rmsTail(df48, 48000) - 0.0707107) < 0.001,
          "DFNR native48 retains16k carrier with algorithm half-gain");
    check(std::abs(rmsTail(nv48, 48000) - 0.0707107) < 0.001,
          "NVIDIA native48 retains16k carrier with algorithm half-gain");
    nativeDf.reset();
    check(run(nativeDf, 48000) == df48, "DFNR reset clears algorithm and wrapper history");
    NvidiaAfxFilter replacementNv(pack, 48000);
    check(run(replacementNv, 48000) == nv48, "NVIDIA replacement creates a clean processing epoch");

    DeepFilterFilter concurrentDf24(24000);
    DeepFilterFilter concurrentDf48(48000);
    NvidiaAfxFilter concurrentNv24(pack, 24000);
    NvidiaAfxFilter concurrentNv48(pack, 48000);
    QByteArray df24Concurrent;
    QByteArray nv24Concurrent;
    constexpr std::array<int, 5> kPartitions{13, 511, 960, 73, 480};
    for (int first = 0, part = 0; first < 48000; ++part) {
        const int frames = std::min(48000 - first, kPartitions[part % kPartitions.size()]);
        const QByteArray input24 = tone(24000, first, frames);
        df24Concurrent.append(concurrentDf24.process(input24));
        nv24Concurrent.append(concurrentNv24.process(input24));
        concurrentDf48.process(tone(48000, first * 2, frames * 2));
        concurrentNv48.process(tone(48000, first * 2, frames * 2));
        first += frames;
    }
    check(df24Concurrent == baselineDf, "concurrent48 leaves DFNR24 output unchanged");
    check(nv24Concurrent == baselineNv, "concurrent48 leaves NVIDIA24 output unchanged");
    if (createdFixture) {
        QFile::remove(fixture);
    }
    return g_failures == 0 ? 0 : 1;
}
