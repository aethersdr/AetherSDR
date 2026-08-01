#include "WsprBeacon.h"

#include <QByteArray>

#include <algorithm>
#include <bit>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr uint32_t kPolynomial1 = 0xf2d05351U;
constexpr uint32_t kPolynomial2 = 0xe4613c47U;
constexpr double kTwoPi = 6.28318530717958647692;

constexpr std::array<uint8_t, WsprBeacon::kSymbolCount> kSync{
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
    0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0
};

int callsignCharacter(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return c == ' ' ? 36 : -1;
}

uint8_t reverseByte(uint8_t value)
{
    value = static_cast<uint8_t>((value >> 4U) | (value << 4U));
    value = static_cast<uint8_t>(((value & 0xccU) >> 2U)
                                 | ((value & 0x33U) << 2U));
    return static_cast<uint8_t>(((value & 0xaaU) >> 1U)
                                | ((value & 0x55U) << 1U));
}

} // namespace

bool WsprBeacon::isStandardPower(int powerDbm)
{
    static constexpr std::array<int, 19> kPowers{
        0, 3, 7, 10, 13, 17, 20, 23, 27, 30,
        33, 37, 40, 43, 47, 50, 53, 57, 60
    };
    return std::ranges::find(kPowers, powerDbm) != kPowers.end();
}

WsprBeacon::EncodeResult WsprBeacon::encode(const QString& callsign,
                                            const QString& locator,
                                            int powerDbm)
{
    EncodeResult result;
    QString call = callsign.trimmed().toUpper();
    const QString grid = locator.trimmed().toUpper();

    if (call.size() < 3 || call.size() > 6) {
        result.error = QStringLiteral("Callsign must contain 3 to 6 characters");
        return result;
    }
    // Reject anything that is not a bare type-1 call before the padding below
    // introduces legitimate spaces. callsignCharacter() maps a space to 36, so
    // an embedded space passes the c3/c4/c5 >= 10 checks further down and
    // "K1A C" would encode and transmit as a call the operator never typed.
    // The one leading space a short-prefix call needs is prepended after this,
    // and trailing pad spaces come from leftJustified().
    for (const QChar ch : call) {
        const bool isUpperLetter =
            ch >= QLatin1Char('A') && ch <= QLatin1Char('Z');
        const bool isDigit = ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
        if (!isUpperLetter && !isDigit) {
            result.error = QStringLiteral(
                "Callsign must contain only letters and digits");
            return result;
        }
    }
    if (call.size() >= 2 && call.at(1).isDigit()) {
        call.prepend(QLatin1Char(' '));
    }
    call = call.leftJustified(6, QLatin1Char(' '));
    if (call.size() != 6 || !call.at(2).isDigit()) {
        result.error = QStringLiteral(
            "Only standard WSPR callsigns with a one-digit prefix are supported");
        return result;
    }

    const QByteArray callBytes = call.toLatin1();
    const int c0 = callsignCharacter(callBytes.at(0));
    const int c1 = callsignCharacter(callBytes.at(1));
    const int c2 = callsignCharacter(callBytes.at(2));
    const int c3 = callsignCharacter(callBytes.at(3));
    const int c4 = callsignCharacter(callBytes.at(4));
    const int c5 = callsignCharacter(callBytes.at(5));
    if (c0 < 0 || c1 < 0 || c1 > 35 || c2 < 0 || c2 > 9
        || c3 < 10 || c4 < 10 || c5 < 10) {
        result.error = QStringLiteral("Callsign is not encodable as a standard WSPR call");
        return result;
    }

    if (grid.size() != 4
        || grid.at(0) < QLatin1Char('A') || grid.at(0) > QLatin1Char('R')
        || grid.at(1) < QLatin1Char('A') || grid.at(1) > QLatin1Char('R')
        || !grid.at(2).isDigit() || !grid.at(3).isDigit()) {
        result.error = QStringLiteral("Locator must be a four-character Maidenhead grid");
        return result;
    }
    if (!isStandardPower(powerDbm)) {
        result.error = QStringLiteral("Power must be a standard WSPR dBm value");
        return result;
    }

    uint32_t packedCall = static_cast<uint32_t>(c0);
    packedCall = packedCall * 36U + static_cast<uint32_t>(c1);
    packedCall = packedCall * 10U + static_cast<uint32_t>(c2);
    packedCall = packedCall * 27U + static_cast<uint32_t>(c3 - 10);
    packedCall = packedCall * 27U + static_cast<uint32_t>(c4 - 10);
    packedCall = packedCall * 27U + static_cast<uint32_t>(c5 - 10);

    const int grid0 = grid.at(0).unicode() - 'A';
    const int grid1 = grid.at(1).unicode() - 'A';
    const int grid2 = grid.at(2).unicode() - '0';
    const int grid3 = grid.at(3).unicode() - '0';
    const uint32_t packedGrid = static_cast<uint32_t>(
        (179 - 10 * grid0 - grid2) * 180 + 10 * grid1 + grid3);
    const uint32_t packedSecond = packedGrid * 128U
                                  + static_cast<uint32_t>(powerDbm + 64);

    std::array<uint8_t, 11> bytes{};
    bytes[0] = static_cast<uint8_t>(packedCall >> 20U);
    bytes[1] = static_cast<uint8_t>(packedCall >> 12U);
    bytes[2] = static_cast<uint8_t>(packedCall >> 4U);
    bytes[3] = static_cast<uint8_t>((packedCall & 0x0fU) << 4U)
               | static_cast<uint8_t>((packedSecond >> 18U) & 0x0fU);
    bytes[4] = static_cast<uint8_t>(packedSecond >> 10U);
    bytes[5] = static_cast<uint8_t>(packedSecond >> 2U);
    bytes[6] = static_cast<uint8_t>((packedSecond & 0x03U) << 6U);

    std::array<uint8_t, kSymbolCount> encoded{};
    uint32_t shiftRegister = 0;
    for (int bitIndex = 0; bitIndex < 81; ++bitIndex) {
        const uint8_t bit = static_cast<uint8_t>(
            (bytes[bitIndex / 8] >> (7 - bitIndex % 8)) & 1U);
        shiftRegister = (shiftRegister << 1U) | bit;
        encoded[bitIndex * 2] = static_cast<uint8_t>(
            std::popcount(shiftRegister & kPolynomial1) & 1U);
        encoded[bitIndex * 2 + 1] = static_cast<uint8_t>(
            std::popcount(shiftRegister & kPolynomial2) & 1U);
    }

    std::array<uint8_t, kSymbolCount> interleaved{};
    int source = 0;
    for (int i = 0; i < 256 && source < kSymbolCount; ++i) {
        const uint8_t destination = reverseByte(static_cast<uint8_t>(i));
        if (destination < kSymbolCount) {
            interleaved[destination] = encoded[source++];
        }
    }
    for (int i = 0; i < kSymbolCount; ++i) {
        result.symbols[i] = static_cast<uint8_t>(
            kSync[i] + 2U * interleaved[i]);
    }
    return result;
}

void WsprBeacon::prepare(double sampleRate)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : kSampleRate;
}

void WsprBeacon::start(const Symbols& symbols, double toneZeroHz, float levelDb,
                       int preRollFrames, int messageSkipFrames)
{
    m_active.store(false, std::memory_order_release);
    for (int i = 0; i < kSymbolCount; ++i) {
        m_pendingSymbols[i].store(symbols[i], std::memory_order_relaxed);
    }
    m_pendingToneZeroHz.store(std::clamp(toneZeroHz, 200.0, 4000.0),
                              std::memory_order_relaxed);
    m_pendingLevelDb.store(std::clamp(levelDb, -60.0f, -3.0f),
                           std::memory_order_relaxed);
    m_pendingPreRollFrames.store(std::max(0, preRollFrames),
                                 std::memory_order_relaxed);
    // Clamped one frame short of the message so a nonsense skip cannot start
    // an already-complete transmission.
    m_pendingMessageSkipFrames.store(
        static_cast<int>(std::clamp<int64_t>(messageSkipFrames, 0,
                                             kMessageFrames - 1)),
        std::memory_order_relaxed);
    m_complete.store(false, std::memory_order_relaxed);
    m_currentSymbol.store(-1, std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
    m_active.store(true, std::memory_order_release);
}

void WsprBeacon::stop() noexcept
{
    m_active.store(false, std::memory_order_release);
    m_complete.store(false, std::memory_order_relaxed);
    m_currentSymbol.store(-1, std::memory_order_relaxed);
}

bool WsprBeacon::isActive() const noexcept
{
    return m_active.load(std::memory_order_acquire);
}

bool WsprBeacon::isComplete() const noexcept
{
    return m_complete.load(std::memory_order_acquire);
}

int WsprBeacon::currentSymbol() const noexcept
{
    return m_currentSymbol.load(std::memory_order_relaxed);
}

void WsprBeacon::beginPendingTransmission() noexcept
{
    for (int i = 0; i < kSymbolCount; ++i) {
        m_symbols[i] = m_pendingSymbols[i].load(std::memory_order_relaxed);
    }
    m_toneZeroHz = m_pendingToneZeroHz.load(std::memory_order_relaxed);
    const float levelDb = m_pendingLevelDb.load(std::memory_order_relaxed);
    m_amplitude = std::pow(10.0f, levelDb / 20.0f);
    m_preRollRemaining = m_pendingPreRollFrames.load(std::memory_order_relaxed);
    const int skip =
        m_pendingMessageSkipFrames.load(std::memory_order_relaxed);
    m_symbolIndex = skip / kFramesPerSymbol;
    m_framesIntoSymbol = skip % kFramesPerSymbol;
    m_framesEmitted = 0;
    m_phase = 0.0;
    m_phaseIncrement = 0.0;
    // One decay step below the first ramp-up step, so frame 0 of the message
    // starts at about -97.5 dB and reaches unity exactly at frame
    // kTaperFrames - 1. See kTaperFrames in the header.
    m_envelope = std::pow(kTaperDecayPerFrame,
                          static_cast<float>(kTaperFrames));
    m_complete.store(false, std::memory_order_release);
    m_currentSymbol.store(-1, std::memory_order_relaxed);
    m_cachedVersion = m_version.load(std::memory_order_acquire);
}

bool WsprBeacon::beginProcess() noexcept
{
    if (!isActive()) {
        return false;
    }
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version != m_cachedVersion) {
        beginPendingTransmission();
    }
    return true;
}

float WsprBeacon::nextFrameSample() noexcept
{
    if (m_preRollRemaining > 0) {
        --m_preRollRemaining;
        return 0.0f;
    }
    if (m_symbolIndex >= kSymbolCount) {
        return 0.0f;
    }

    m_currentSymbol.store(m_symbolIndex, std::memory_order_relaxed);

    // Tone 0 sits at the requested frequency and the constellation runs
    // upward from there, exactly as WSJT-X does it — see start()'s comment.
    const double tone =
        m_toneZeroHz + static_cast<double>(m_symbols[m_symbolIndex])
                           * kToneSpacingHz;
    m_phaseIncrement = kTwoPi * tone / m_sampleRate;

    // Phase is continuous across symbol boundaries (m_phase is never reset),
    // so the ONLY discontinuities in the whole frame are its two ends — which
    // is what the envelope below is for. The ramp-up keys off frames emitted
    // and the tail off position within the message: on a late start those
    // differ, and the ramp has to follow the audio, not the symbol clock.
    //
    // The tail is tested FIRST so that it always wins. A messageSkipFrames deep
    // enough to land inside the tail region would otherwise ramp UP over the
    // few frames left and then stop dead at whatever level it reached — a step
    // discontinuity, i.e. the exact click this taper exists to prevent. Letting
    // the tail win leaves such a frame decaying from the seed envelope, so it
    // ends silent instead. Unreachable from updateBeaconState(), where the skip
    // is bounded by kBeaconMaxSlotLatenessMs, but start() is public and clamps
    // only to kMessageFrames - 1. The ordering costs nothing on the reachable
    // path: there the ramp runs ~24000 frames in, nowhere near the tail region.
    const int64_t framePos =
        static_cast<int64_t>(m_symbolIndex) * kFramesPerSymbol
        + m_framesIntoSymbol;
    if (framePos >= kMessageFrames - kTaperFrames) {
        m_envelope *= kTaperDecayPerFrame;
    } else if (m_framesEmitted < kTaperFrames) {
        m_envelope = std::min(1.0f, m_envelope / kTaperDecayPerFrame);
    } else {
        m_envelope = 1.0f;
    }
    ++m_framesEmitted;

    const float sample = static_cast<float>(std::sin(m_phase))
                         * m_amplitude * m_envelope;
    m_phase += m_phaseIncrement;
    if (m_phase >= kTwoPi) {
        m_phase -= kTwoPi;
    }

    ++m_framesIntoSymbol;
    if (m_framesIntoSymbol >= kFramesPerSymbol) {
        m_framesIntoSymbol = 0;
        ++m_symbolIndex;
        if (m_symbolIndex >= kSymbolCount) {
            m_complete.store(true, std::memory_order_release);
            m_currentSymbol.store(kSymbolCount, std::memory_order_relaxed);
        }
    }
    return sample;
}

void WsprBeacon::process(float* interleaved, int frames, int channels) noexcept
{
    if (interleaved == nullptr || frames <= 0 || channels < 1 || channels > 2
        || !beginProcess()) {
        return;
    }
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = nextFrameSample();
        interleaved[frame * channels] = sample;
        if (channels == 2) {
            interleaved[frame * channels + 1] = sample;
        }
    }
}

} // namespace AetherSDR
