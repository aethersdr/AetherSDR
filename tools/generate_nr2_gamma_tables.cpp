// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Research approximation of the two NR2 Gamma-prior gain tables from the
 * published equations.
 *
 * Reference:
 *   B. Fodor and T. Fingscheidt, "MMSE Speech Enhancement Under Speech
 *   Presence Uncertainty Assuming (Generalized) Gamma Speech Priors
 *   Throughout", ICASSP 2012, doi:10.1109/ICASSP.2012.6288803.
 *
 * This is an offline development tool. It has no runtime dependency on Qt or
 * AetherSDR and deliberately uses only the C++ standard library.
 *
 * Build:
 *   c++ -std=c++20 -O3 -pthread tools/generate_nr2_gamma_tables.cpp \
 *       -o /tmp/generate_nr2_gamma_tables
 *
 * Generate and optionally validate against the public WDSP calculus binary.
 * The comparison exits non-zero unless RMS error is at most 1e-4 and maximum
 * absolute error is at most 1e-3; AetherSDR runtime uses the exact embedded
 * calculus data, not this approximation.
 *   /tmp/generate_nr2_gamma_tables --output /tmp/calculus.generated \
 *       --reference /path/to/wdsp/Source/calculus
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kGridSize = 241;
constexpr int kPlaneSize = kGridSize * kGridSize;
constexpr int kValueCount = 2 * kPlaneSize;
constexpr long double kNu = 1.126L;
constexpr long double kSpeechAbsencePrior = 0.2L;
constexpr long double kDbMinimum = -30.0L;
constexpr long double kDbStep = 0.25L;
constexpr long double kTailLogAttenuation = 64.0L;
constexpr long double kMaximumComparisonRmsError = 1.0e-4L;
constexpr long double kMaximumComparisonAbsoluteError = 1.0e-3L;

struct IntegralPair {
    long double numerator{0.0L};
    long double denominator{0.0L};
};

struct IntegralEvaluation {
    IntegralPair values;
    long double logScale{0.0L};
};

struct Options {
    std::string outputPath;
    std::string referencePath;
    unsigned int threads{std::max(1U, std::thread::hardware_concurrency())};
};

struct ErrorMetrics {
    long double sumAbsolute{0.0L};
    long double sumSquared{0.0L};
    long double maxAbsolute{0.0L};
    long double maxRelative{0.0L};
    std::size_t maxAbsoluteIndex{0};
    std::size_t maxRelativeIndex{0};
    std::size_t bitExact{0};
    std::array<std::size_t, 4> within{};
};

constexpr std::array<long double, 8> kKronrodAbscissae = {
    0.991455371120812639206854697526329L,
    0.949107912342758524526189684047851L,
    0.864864423359769072789712788640926L,
    0.741531185599394439863864773280788L,
    0.586087235467691130294144838258730L,
    0.405845151377397166906606412076961L,
    0.207784955007898467600689403773245L,
    0.0L,
};

constexpr std::array<long double, 8> kKronrodWeights = {
    0.022935322010529224963732008058970L,
    0.063092092629978553290700663189204L,
    0.104790010322250183839876322541518L,
    0.140653259715525918745189590510238L,
    0.169004726639267902826583426598550L,
    0.190350578064785409913256402421014L,
    0.204432940075298892414161999234649L,
    0.209482141084727828012999174891714L,
};

constexpr std::array<long double, 4> kGaussWeights = {
    0.129484966168869693270611432679082L,
    0.279705391489276667901467771423780L,
    0.381830050505118944950369775488975L,
    0.417959183673469387755102040816327L,
};

void printUsage(const char* executable)
{
    std::cerr
        << "Usage: " << executable
        << " --output PATH [--reference PATH] [--threads N]\n"
        << "Produces a research approximation. Runtime tables come from the "
           "exact embedded calculus data.\n";
}

std::optional<Options> parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--output" && index + 1 < argc) {
            options.outputPath = argv[++index];
        } else if (argument == "--reference" && index + 1 < argc) {
            options.referencePath = argv[++index];
        } else if (argument == "--threads" && index + 1 < argc) {
            const long value = std::strtol(argv[++index], nullptr, 10);
            if (value <= 0 || value > 1024) {
                std::cerr << "Invalid thread count\n";
                return std::nullopt;
            }
            options.threads = static_cast<unsigned int>(value);
        } else {
            printUsage(argv[0]);
            return std::nullopt;
        }
    }
    if (options.outputPath.empty()) {
        printUsage(argv[0]);
        return std::nullopt;
    }
    return options;
}

long double scaledBesselI0(long double x)
{
    x = std::abs(x);
    if (x < 50.0L) {
        const long double y = 0.25L * x * x;
        long double term = 1.0L;
        long double sum = 1.0L;
        for (int order = 1; order < 10000; ++order) {
            const long double denominator =
                static_cast<long double>(order)
                * static_cast<long double>(order);
            term *= y / denominator;
            sum += term;
            if (term <= std::numeric_limits<long double>::epsilon() * sum) {
                break;
            }
        }
        return std::exp(-x) * sum;
    }

    long double term = 1.0L;
    long double sum = 1.0L;
    long double previousTerm = term;
    for (int order = 1; order < 10000; ++order) {
        const long double odd = static_cast<long double>(2 * order - 1);
        term *= odd * odd
            / (8.0L * static_cast<long double>(order) * x);
        if (term > previousTerm) {
            break;
        }
        sum += term;
        if (term <= std::numeric_limits<long double>::epsilon() * sum) {
            break;
        }
        previousTerm = term;
    }
    return sum / std::sqrt(2.0L * std::numbers::pi_v<long double> * x);
}

IntegralPair add(const IntegralPair& left, const IntegralPair& right)
{
    return {left.numerator + right.numerator,
            left.denominator + right.denominator};
}

IntegralPair subtract(const IntegralPair& left, const IntegralPair& right)
{
    return {left.numerator - right.numerator,
            left.denominator - right.denominator};
}

IntegralPair multiply(const IntegralPair& value, long double scale)
{
    return {value.numerator * scale, value.denominator * scale};
}

long double largestMagnitude(const IntegralPair& value)
{
    return std::max(std::abs(value.numerator),
                    std::abs(value.denominator));
}

template <typename Function>
std::pair<IntegralPair, long double> gaussKronrod15(
    const Function& function,
    long double lower,
    long double upper)
{
    const long double center = 0.5L * (lower + upper);
    const long double halfLength = 0.5L * (upper - lower);
    IntegralPair kronrod = multiply(function(center), kKronrodWeights[7]);
    IntegralPair gauss = multiply(function(center), kGaussWeights[3]);

    for (std::size_t index = 0; index < 7; ++index) {
        const long double offset = halfLength * kKronrodAbscissae[index];
        const IntegralPair pair = add(function(center - offset),
                                      function(center + offset));
        kronrod = add(kronrod, multiply(pair, kKronrodWeights[index]));
        if (index == 1) {
            gauss = add(gauss, multiply(pair, kGaussWeights[0]));
        } else if (index == 3) {
            gauss = add(gauss, multiply(pair, kGaussWeights[1]));
        } else if (index == 5) {
            gauss = add(gauss, multiply(pair, kGaussWeights[2]));
        }
    }

    kronrod = multiply(kronrod, halfLength);
    gauss = multiply(gauss, halfLength);
    return {kronrod, largestMagnitude(subtract(kronrod, gauss))};
}

template <typename Function>
IntegralPair integrateAdaptive(const Function& function,
                               long double lower,
                               long double upper,
                               long double absoluteTolerance,
                               long double relativeTolerance,
                               int depth = 0)
{
    const auto [whole, error] = gaussKronrod15(function, lower, upper);
    const long double tolerance = std::max(
        absoluteTolerance, relativeTolerance * largestMagnitude(whole));
    if (error <= tolerance || depth >= 24) {
        return whole;
    }

    const long double middle = 0.5L * (lower + upper);
    return add(
        integrateAdaptive(function, lower, middle,
                          0.5L * absoluteTolerance, relativeTolerance,
                          depth + 1),
        integrateAdaptive(function, middle, upper,
                          0.5L * absoluteTolerance, relativeTolerance,
                          depth + 1));
}

long double integrationUpperBound(long double gamma, long double a)
{
    const long double unconstrainedPeak = 1.0L - a / (2.0L * gamma);
    const long double peak = std::max(0.0L, unconstrainedPeak);
    const auto exponentCost = [gamma, a](long double value) {
        const long double delta = value - 1.0L;
        return gamma * delta * delta + a * value;
    };
    const long double target = exponentCost(peak) + kTailLogAttenuation;
    const long double linear = a - 2.0L * gamma;
    const long double discriminant = linear * linear
        - 4.0L * gamma * (gamma - target);
    return (-linear + std::sqrt(std::max(0.0L, discriminant)))
        / (2.0L * gamma);
}

IntegralEvaluation evaluateIntegrals(long double gamma, long double xi)
{
    const long double a = std::sqrt(kNu * (kNu + 1.0L) * gamma / xi);
    const long double upper = integrationUpperBound(gamma, a);
    const long double peak = std::max(0.0L, 1.0L - a / (2.0L * gamma));
    const long double peakDelta = peak - 1.0L;
    const long double minimumCost =
        gamma * peakDelta * peakDelta + a * peak;
    // Substitute g=t^2. The denominator contains g^(nu-1), whose derivative
    // is singular at zero for nu=1.126. The substitution changes the endpoint
    // behavior to t^(2*nu-1) and makes adaptive error estimation reliable.
    const auto integrand = [gamma, a, minimumCost](long double t) {
        if (t <= 0.0L) {
            return IntegralPair{};
        }
        const long double value = t * t;
        const long double delta = value - 1.0L;
        const long double common = 2.0L * t
            * std::pow(value, kNu - 1.0L)
            * std::exp(minimumCost - gamma * delta * delta - a * value)
            * scaledBesselI0(2.0L * gamma * value);
        return IntegralPair{value * common, common};
    };
    return {
        integrateAdaptive(integrand, 0.0L, std::sqrt(upper),
                          1.0e-17L, 2.0e-15L),
        -minimumCost,
    };
}

std::pair<double, double> generateCell(int xiIndex, int gammaIndex)
{
    const long double gammaDb = kDbMinimum
        + kDbStep * static_cast<long double>(gammaIndex);
    const long double xiDb = kDbMinimum
        + kDbStep * static_cast<long double>(xiIndex);
    const long double gamma = std::pow(10.0L, gammaDb / 10.0L);
    const long double xi = std::pow(10.0L, xiDb / 10.0L);
    const long double a = std::sqrt(kNu * (kNu + 1.0L) * gamma / xi);
    const IntegralEvaluation evaluation = evaluateIntegrals(gamma, xi);
    const IntegralPair& integrals = evaluation.values;

    const long double gain = integrals.numerator / integrals.denominator;
    const long double logLikelihood =
        std::log((1.0L - kSpeechAbsencePrior) / kSpeechAbsencePrior)
        + kNu * std::log(a) - std::lgamma(kNu) + gamma
        + evaluation.logScale
        + std::log(integrals.denominator);
    long double softWeight;
    if (logLikelihood >= 0.0L) {
        softWeight = 1.0L / (1.0L + std::exp(-logLikelihood));
    } else {
        const long double likelihood = std::exp(logLikelihood);
        softWeight = likelihood / (1.0L + likelihood);
    }
    return {static_cast<double>(gain), static_cast<double>(softWeight)};
}

std::vector<double> generateTables(unsigned int threadCount)
{
    std::vector<double> values(kValueCount);
    std::atomic<int> nextXiIndex{0};
    const auto worker = [&values, &nextXiIndex] {
        while (true) {
            const int xiIndex = nextXiIndex.fetch_add(1);
            if (xiIndex >= kGridSize) {
                return;
            }
            for (int gammaIndex = 0; gammaIndex < kGridSize; ++gammaIndex) {
                const auto [gain, softWeight] =
                    generateCell(xiIndex, gammaIndex);
                const int index = xiIndex * kGridSize + gammaIndex;
                values[index] = gain;
                values[kPlaneSize + index] = softWeight;
            }
        }
    };

    const unsigned int boundedThreads = std::clamp(
        threadCount, 1U, static_cast<unsigned int>(kGridSize));
    std::vector<std::thread> workers;
    workers.reserve(boundedThreads);
    for (unsigned int index = 0; index < boundedThreads; ++index) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) {
        thread.join();
    }
    return values;
}

bool writeLittleEndianDoubles(const std::string& path,
                              std::span<const double> values)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    for (double value : values) {
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
        std::array<unsigned char, 8> bytes{};
        for (int byte = 0; byte < 8; ++byte) {
            bytes[byte] = static_cast<unsigned char>(bits >> (8 * byte));
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return output.good();
}

std::optional<std::vector<double>> readLittleEndianDoubles(
    const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(
            kValueCount * static_cast<int>(sizeof(double)))) {
        return std::nullopt;
    }
    input.seekg(0);
    std::vector<double> values(kValueCount);
    for (double& value : values) {
        std::array<unsigned char, 8> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        std::uint64_t bits = 0;
        for (int byte = 0; byte < 8; ++byte) {
            bits |= static_cast<std::uint64_t>(bytes[byte]) << (8 * byte);
        }
        value = std::bit_cast<double>(bits);
    }
    if (!input) {
        return std::nullopt;
    }
    return values;
}

ErrorMetrics compare(std::span<const double> generated,
                     std::span<const double> reference)
{
    constexpr std::array<long double, 4> kTolerances = {
        1.0e-12L, 1.0e-9L, 1.0e-6L, 1.0e-3L};
    ErrorMetrics metrics;
    for (std::size_t index = 0; index < generated.size(); ++index) {
        const long double actual = generated[index];
        const long double expected = reference[index];
        const long double absolute = std::abs(actual - expected);
        const long double relative = absolute
            / std::max(std::abs(expected), 1.0e-300L);
        metrics.sumAbsolute += absolute;
        metrics.sumSquared += absolute * absolute;
        if (absolute > metrics.maxAbsolute) {
            metrics.maxAbsolute = absolute;
            metrics.maxAbsoluteIndex = index;
        }
        if (relative > metrics.maxRelative) {
            metrics.maxRelative = relative;
            metrics.maxRelativeIndex = index;
        }
        if (std::bit_cast<std::uint64_t>(generated[index])
                == std::bit_cast<std::uint64_t>(reference[index])) {
            ++metrics.bitExact;
        }
        for (std::size_t tolerance = 0;
             tolerance < kTolerances.size(); ++tolerance) {
            if (absolute <= kTolerances[tolerance]) {
                ++metrics.within[tolerance];
            }
        }
    }
    return metrics;
}

std::string describeIndex(std::size_t index)
{
    const bool softWeight = index >= static_cast<std::size_t>(kPlaneSize);
    const std::size_t planeIndex = index
        % static_cast<std::size_t>(kPlaneSize);
    const int xiIndex = static_cast<int>(planeIndex / kGridSize);
    const int gammaIndex = static_cast<int>(planeIndex % kGridSize);
    const long double gammaDb = kDbMinimum + kDbStep * gammaIndex;
    const long double xiDb = kDbMinimum + kDbStep * xiIndex;
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s gamma=%+.2Lf dB xi=%+.2Lf dB",
                  softWeight ? "GGS" : "GG", gammaDb, xiDb);
    return buffer;
}

bool printComparison(std::span<const double> generated,
                     std::span<const double> reference)
{
    const ErrorMetrics metrics = compare(generated, reference);
    const long double count = static_cast<long double>(generated.size());
    const long double rmsError = std::sqrt(metrics.sumSquared / count);
    const bool passed = rmsError <= kMaximumComparisonRmsError
        && metrics.maxAbsolute <= kMaximumComparisonAbsoluteError;
    std::cout << std::scientific << std::setprecision(12)
              << "Reference comparison across " << generated.size()
              << " values:\n"
              << "  mean absolute error: "
              << metrics.sumAbsolute / count << '\n'
              << "  RMS error:           "
              << rmsError << '\n'
              << "  maximum abs error:   " << metrics.maxAbsolute
              << " at " << describeIndex(metrics.maxAbsoluteIndex) << '\n'
              << "  generated/reference: "
              << generated[metrics.maxAbsoluteIndex] << " / "
              << reference[metrics.maxAbsoluteIndex] << '\n'
              << "  maximum rel error:   " << metrics.maxRelative
              << " at " << describeIndex(metrics.maxRelativeIndex) << '\n'
              << "  bit-exact values:    " << metrics.bitExact << '\n'
              << "  abs error <= 1e-12:  " << metrics.within[0] << '\n'
              << "  abs error <= 1e-9:   " << metrics.within[1] << '\n'
              << "  abs error <= 1e-6:   " << metrics.within[2] << '\n'
              << "  abs error <= 1e-3:   " << metrics.within[3] << '\n'
              << "  required RMS error:  <= "
              << kMaximumComparisonRmsError << '\n'
              << "  required max error:  <= "
              << kMaximumComparisonAbsoluteError << '\n'
              << "  comparison result:   "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = parseOptions(argc, argv);
    if (!options) {
        return 2;
    }

    std::cout << "Generating an approximate pair of " << kGridSize << 'x'
              << kGridSize << " NR2 Gamma-prior tables with "
              << options->threads
              << " threads...\n";
    const std::vector<double> generated = generateTables(options->threads);
    if (!writeLittleEndianDoubles(options->outputPath, generated)) {
        std::cerr << "Failed to write " << options->outputPath << '\n';
        return 1;
    }
    std::cout << "Wrote " << generated.size() << " doubles to "
              << options->outputPath << '\n';

    if (!options->referencePath.empty()) {
        const std::optional<std::vector<double>> reference =
            readLittleEndianDoubles(options->referencePath);
        if (!reference) {
            std::cerr << "Reference must contain exactly " << kValueCount
                      << " little-endian doubles\n";
            return 1;
        }
        if (!printComparison(generated, *reference)) {
            std::cerr
                << "Approximation does not meet the documented reference "
                   "thresholds; do not use it as runtime calculus data.\n";
            return 1;
        }
    }
    return 0;
}
