#include "RttyDecoder.h"
#include "LogManager.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <complex>

namespace AetherSDR {

namespace {
// Character score (#7422). A clean steady tone's energy share is 1/(1+k²), k being how much of the
// other tone each filter passes; a clean keyed signal reaches about 80 % of the way there from a
// coin flip (measured at 170 Hz / 45.45 baud), so that point scores 1.0.
constexpr double kCleanFraction   = 0.80;
constexpr double kEnergyWindowBits = 0.4;   // the settled part of a bit, before it is read
constexpr double kAvgWeight       = 0.10;   // weight of each new character in the average
constexpr double kLockScore       = 0.83;   // LOCKED from this average score ...
constexpr double kUnlockScore     = 0.70;   // ... until it falls below this one
constexpr double kIdleSeconds     = 1.5;    // or until no valid character for this long
constexpr int    kMinCharacters   = 2;      // LOCKED and the level need this many characters
constexpr int    kMeanCharacters  = 10;     // a fresh average is a plain mean up to this many characters

double scoreFromShare(double share, double cleanShare)
{
    return std::clamp(0.5 + 0.5 * (share - 0.5) / std::max(cleanShare - 0.5, 1e-3), 0.5, 1.0);
}
} // namespace

// Baudot/ITA2 character tables (5-bit code → ASCII)
static const char kBaudotLtrsTable[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D',  'R', 'J', 'N', 'F', 'C', 'K',
     'T', 'Z',  'L', 'W', 'H', 'Y', 'P', 'Q',
     'O', 'B',  'G', '\0', 'M', 'X', 'V', '\0'
};
static const char kBaudotFigsTable[32] = {
    '\0', '3', '\n', '-', ' ', '\a', '8', '7',
    '\r', '$',  '4','\'', ',', '!', ':', '(',
     '5', '"',  ')', '2', '#', '6', '0', '1',
     '9', '?',  '&', '\0', '.', '/', ';', '\0'
};

RttyDecoder::RttyDecoder(QObject* parent)
    : QObject(parent)
{}

RttyDecoder::~RttyDecoder()
{
    stop();
}

void RttyDecoder::start()
{
    if (m_running) {
        return;
    }

    m_running       = true;
    m_paramsChanged = true;

    resetInput();

    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("RttyDecoder");
    m_workerThread.reset(worker);
    worker->start();

    qCDebug(lcDsp) << "RttyDecoder: started baud=" << m_baudRate.load()
                   << "shift=" << m_shiftHz.load() << "mark=" << m_markFreqHz.load();
}

void RttyDecoder::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    resetInput();
    if (m_workerThread) {
        // The loop has only bounded 240-sample work and a 10 ms idle wait.
        // Join before destroying its ring/filter owner, including during restart.
        m_workerThread->wait();
        m_workerThread.reset();
    }
    qCDebug(lcDsp) << "RttyDecoder: stopped";
}

void RttyDecoder::setMarkFreqHz(int hz) {
    if (m_markFreqHz.load() == hz) return;
    m_markFreqHz = hz; m_paramsChanged = true;
}
void RttyDecoder::setShiftHz(int hz) {
    if (m_shiftHz.load() == hz) return;
    m_shiftHz = hz; m_paramsChanged = true;
}
void RttyDecoder::setBaudRate(float baud) {
    if (m_baudRate.load() == baud) return;
    m_baudRate = baud; m_paramsChanged = true;
}
void RttyDecoder::setReversePolarity(bool r) {
    if (m_reverse.load() == r) return;
    m_reverse = r; m_paramsChanged = true;
}

void RttyDecoder::feedAudio(const QByteArray& pcm24kStereoFloat)
{
    if (!m_running || pcm24kStereoFloat.size() % (2 * sizeof(float)) != 0
        || pcm24kStereoFloat.size() > kRingCapacity * 2) {
        return;
    }

    const auto* src = reinterpret_cast<const float*>(pcm24kStereoFloat.constData());
    const int frames = pcm24kStereoFloat.size() / (2 * static_cast<int>(sizeof(float)));

    QByteArray mono(frames * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(mono.data());
    for (int i = 0; i < frames; ++i) {
        if (!std::isfinite(src[2 * i]) || !std::isfinite(src[2 * i + 1])) {
            resetInput();
            return;
        }
        dst[i] = src[2 * i] * 0.5f + src[2 * i + 1] * 0.5f;
    }
    appendMono(mono, {}, false, false);
}

void RttyDecoder::appendMono(const QByteArray& mono, const PcmEpochLease& source,
                             bool typed, bool discontinuity)
{
    QMutexLocker lock(&m_bufMutex);
    if (!m_running || (typed && !source.current())) {
        return;
    }
    if (discontinuity || typed != m_typedSource
        || (typed && source.stream() != m_source.stream())
        || (typed && m_ringBuf.size() + mono.size() > kRingCapacity)) {
        ++m_inputGeneration;
        m_ringBuf.clear();
        queueResetStats(m_inputGeneration.load());
    }
    m_source = source;
    m_typedSource = typed;
    m_ringBuf.append(mono);
    // The legacy byte API keeps its trim-oldest backlog policy, as CwDecoder's
    // does; typed RX overflow is a source discontinuity and retires state above.
    if (!typed && m_ringBuf.size() > kRingCapacity) {
        m_ringBuf.remove(0, m_ringBuf.size() - kRingCapacity);
    }
}

void RttyDecoder::feedPcmBlock(const DecoderPcmBlock& block)
{
    if (!m_running || !block.current() || block.samples.size() > PcmFrame::kMaxFrames) {
        return;
    }
    for (float sample : block.samples) {
        if (!std::isfinite(sample)) {
            resetInput();
            return;
        }
    }
    appendMono(QByteArray(reinterpret_cast<const char*>(block.samples.constData()),
                         block.samples.size() * sizeof(float)),
               block.source, true, block.discontinuity);
}

void RttyDecoder::resetInput()
{
    QMutexLocker lock(&m_bufMutex);
    ++m_inputGeneration;
    m_ringBuf.clear();
    m_source = {};
    m_typedSource = false;
    queueResetStats(m_inputGeneration.load());
}

void RttyDecoder::queueResetStats(quint64 generation)
{
    // Post while the ring lock is held so this precedes any new worker result.
    // Queued delivery also keeps UI callbacks outside the buffer lock and avoids
    // reentrant destruction during start/stop or the source-reset operation.
    QMetaObject::invokeMethod(this, [this, generation] {
        if (m_inputGeneration.load() == generation) {
            emit statsReset();
            emit statsUpdated(0.0f, 0.0f, false);
        }
    }, Qt::QueuedConnection);
}

// ── Filter design ────────────────────────────────────────────────────────────

RttyDecoder::BiquadCoeffs
RttyDecoder::designBandpass(double centerHz, double bwHz, double sampleRate)
{
    const double w0    = 2.0 * M_PI * centerHz / sampleRate;
    const double alpha = std::sin(w0) * bwHz / (2.0 * centerHz);
    const double a0    = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 =  alpha / a0;
    c.b1 =  0.0;
    c.b2 = -alpha / a0;
    c.a1 = -2.0 * std::cos(w0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

double RttyDecoder::processBiquad(BiquadCoeffs& c, BiquadState& s, double x)
{
    double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2
             - c.a1 * s.y1 - c.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    return y;
}

void RttyDecoder::recalcFilterCoeffs()
{
    const double markHz  = m_markFreqHz.load();
    const double shift   = m_shiftHz.load();
    const double baud    = m_baudRate.load();
    // Rev=false: space below mark (standard HF RTTY in USB)
    // Rev=true:  space above mark (LSB, or reversed polarity signal)
    const double spaceHz = m_reverse.load() ? markHz + shift : markHz - shift;
    const double bw      = baud * 3.0;  // 3× baud: selectivity vs. fast tracking

    // A tone at 0 Hz divides by zero in designBandpass(), and a negative one
    // builds the filter at its mirror image; neither gives a usable filter.
    // The decoder stays idle until both tones are above 0 Hz again.
    m_filtersValid = markHz > 0.0 && spaceHz > 0.0;
    if (m_filtersValid) {
        m_markCoeffs  = designBandpass(markHz,  bw, kSampleRate);
        m_spaceCoeffs = designBandpass(spaceHz, bw, kSampleRate);

        // Each filter's gain at the other tone, from the coefficients just designed.
        const auto gainAt = [](const BiquadCoeffs& c, double hz) {
            const std::complex<double> z1 = std::polar(1.0, -2.0 * M_PI * hz / kSampleRate);
            return std::abs((c.b0 + c.b1 * z1 + c.b2 * z1 * z1) / (1.0 + c.a1 * z1 + c.a2 * z1 * z1));
        };
        const double k = 0.5 * (gainAt(m_markCoeffs, spaceHz) + gainAt(m_spaceCoeffs, markHz));
        m_cleanShare = 0.5 + kCleanFraction * (1.0 / (1.0 + k * k) - 0.5);
    }
    m_markState   = {};
    m_spaceState  = {};
    m_markEnv     = 0;
    m_spaceEnv    = 0;

    const int energyWindow = std::max(1, static_cast<int>(std::lround(kEnergyWindowBits * kSampleRate / baud)));
    m_eRingMark.assign(energyWindow, 0.0);
    m_eRingSpace.assign(energyWindow, 0.0);
    m_eRingPos = 0;
    m_eSumMark = m_eSumSpace = 0.0;
    m_frameWeakest = 1.0;
    m_avgShare = 0.0;
    m_avgFrames = 0;
    m_lockState = false;
    m_samplesSinceFrame = 0;

    qCDebug(lcDsp) << "RttyDecoder: mark=" << markHz << "space=" << spaceHz
                   << "bw=" << bw << "baud=" << baud << "rev=" << m_reverse.load();
    if (!m_filtersValid) {
        qCDebug(lcDsp) << "RttyDecoder: idle, mark and space must both be above 0 Hz";
    }
}

// ── Baudot ───────────────────────────────────────────────────────────────────

char RttyDecoder::baudotToAscii(int code, bool figs)
{
    if (code < 0 || code > 31) return '\0';
    return figs ? kBaudotFigsTable[code] : kBaudotLtrsTable[code];
}

// ── Decode loop (worker thread) ───────────────────────────────────────────────

void RttyDecoder::decodeLoop()
{
    constexpr int kChunkSamples = 240;  // 10 ms at 24 kHz
    constexpr int kChunkBytes   = kChunkSamples * static_cast<int>(sizeof(float));

    // Envelope time constant: 0.3/baud gives crossover in ~0.2 bit periods,
    // fast enough for start-bit detection while still smoothing carrier ripple.
    auto calcEnvAlpha = [](double baud) {
        return 1.0 - std::exp(-baud / (kSampleRate * 0.3));
    };

    // Schmitt trigger hysteresis: a 10% band prevents chattering at the
    // mark/space boundary when the signal is near equal amplitude.
    constexpr double kHyst = 0.10;

    double baud        = m_baudRate.load();
    double envAlpha    = calcEnvAlpha(baud);
    int    statsTick   = 0;

    // Decoder state machine
    int    prevBit     = 1;  // idle = mark
    int    curBit      = 1;
    bool   inChar      = false;
    int    bitCount    = 0;
    int    shiftReg    = 0;
    bool   figsMode    = false;
    double bitClock    = 0.0;
    double samplesPerBit = kSampleRate / baud;
    quint64 workerGeneration = m_inputGeneration.load();

    while (m_running) {
        if (m_paramsChanged.exchange(false)) {
            recalcFilterCoeffs();
            baud          = m_baudRate.load();
            envAlpha      = calcEnvAlpha(baud);
            samplesPerBit = kSampleRate / baud;
            // Reset bit-level state but preserve figsMode: the LTRS/FIGS
            // shift is session state set by received codes, not a filter
            // parameter, so a baud/mark change mid-stream shouldn't flip
            // the character set back to LTRS unexpectedly.
            inChar   = false;
            bitCount = 0;
            shiftReg = 0;
            prevBit  = 1;
            curBit   = 1;
            bitClock = 0.0;
        }

        QByteArray chunk;
        quint64 generation = 0;
        PcmEpochLease source;
        bool typed = false;
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_ringBuf.size() < kChunkBytes) {
                lock.unlock();
                QThread::msleep(10);
                continue;
            }
            chunk = m_ringBuf.left(kChunkBytes);
            m_ringBuf.remove(0, kChunkBytes);
            generation = m_inputGeneration.load();
            source = m_source;
            typed = m_typedSource;
        }

        if (workerGeneration != generation) {
            workerGeneration = generation;
            recalcFilterCoeffs();
            prevBit = curBit = 1;
            inChar = false;
            bitCount = shiftReg = statsTick = 0;
            figsMode = false;
            bitClock = 0.0;
        }
        const auto current = [this, generation, source, typed] {
            return m_running && m_inputGeneration.load() == generation
                && (!typed || source.current());
        };
        if (!current()) {
            continue;
        }

        const auto* samples = reinterpret_cast<const float*>(chunk.constData());

        // Idle: the chunk is drained, the envelopes stay at 0, and the stats
        // below report no lock.
        for (int i = 0; m_filtersValid && i < kChunkSamples; ++i) {
            if (!current()) {
                break;
            }
            const double x = samples[i];

            // Bandpass + envelope
            const double markOut  = processBiquad(m_markCoeffs,  m_markState,  x);
            const double spaceOut = processBiquad(m_spaceCoeffs, m_spaceState, x);
            m_markEnv  += envAlpha * (std::abs(markOut)  - m_markEnv);
            m_spaceEnv += envAlpha * (std::abs(spaceOut) - m_spaceEnv);

            // Tone energy over the trailing 0.4 bit, for the character score only: the
            // envelope above lags too far to show how cleanly a bit was received.
            {
                const double em = markOut * markOut;
                const double es = spaceOut * spaceOut;
                m_eSumMark  += em - m_eRingMark[m_eRingPos];
                m_eSumSpace += es - m_eRingSpace[m_eRingPos];
                m_eRingMark[m_eRingPos]  = em;
                m_eRingSpace[m_eRingPos] = es;
                if (++m_eRingPos == static_cast<int>(m_eRingMark.size())) { m_eRingPos = 0; }
                ++m_samplesSinceFrame;
            }

            // Schmitt trigger: hysteresis prevents chattering at the boundary
            if (m_markEnv  > m_spaceEnv * (1.0 + kHyst))  curBit = 1;
            else if (m_spaceEnv > m_markEnv * (1.0 + kHyst))  curBit = 0;
            // else: keep curBit

            // Start-bit detection: falling edge (mark→space) while idle
            if (!inChar && prevBit == 1 && curBit == 0) {
                inChar      = true;
                bitCount    = 0;
                shiftReg    = 0;
                m_frameWeakest = 1.0;
                // Sample the first data bit 1.5 bit periods after this edge.
                // The +1/spb compensates for the increment at the end of this loop.
                bitClock = -(0.5 - 1.0 / samplesPerBit);
            } else if (inChar) {
                // Gentle clock correction on transitions: pull the clock
                // toward the expected zero-crossing (25% per edge).
                if (curBit != prevBit) {
                    double frac = bitClock;
                    if (frac > 0.5) frac -= 1.0;
                    bitClock -= frac * 0.25;
                }
            }

            prevBit   = curBit;
            bitClock += 1.0 / samplesPerBit;

            // Clock tick — sample a bit
            if (inChar && bitClock >= 1.0) {
                bitClock -= 1.0;

                if (bitCount < 5) {
                    // Energy share of the tone this bit was read as.
                    const double eTotal = m_eSumMark + m_eSumSpace;
                    const double share  = eTotal > 1e-18
                        ? (curBit ? m_eSumMark : m_eSumSpace) / eTotal : 0.5;
                    m_frameWeakest = std::min(m_frameWeakest, share);
                    shiftReg |= (curBit << bitCount);
                    ++bitCount;
                } else {
                    // Stop bit — must be mark; discard character if not
                    inChar = false;
                    if (curBit == 1) {
                        // After kIdleSeconds without a character the average starts again, so
                        // separate carrier events do not add up to a lock.
                        if (m_avgFrames == 0 || m_samplesSinceFrame > static_cast<qint64>(kIdleSeconds * kSampleRate)) {
                            m_avgShare = m_frameWeakest;
                            m_avgFrames = 1;
                        } else {
                            m_avgFrames = std::min(m_avgFrames + 1, kMeanCharacters);
                            const double w = std::max(kAvgWeight, 1.0 / m_avgFrames);
                            m_avgShare = (1.0 - w) * m_avgShare + w * m_frameWeakest;
                        }
                        m_samplesSinceFrame = 0;
                        if (shiftReg == kBaudotLtrs) {
                            figsMode = false;
                        } else if (shiftReg == kBaudotFigs) {
                            figsMode = true;
                        } else if (shiftReg != kBaudotNull) {
                            char ch = baudotToAscii(shiftReg, figsMode);
                            if (ch != '\0' && ch != '\a') {
                                const float conf = static_cast<float>(
                                    scoreFromShare(m_frameWeakest, m_cleanShare));
                                const QString text(QChar::fromLatin1(ch));
                                QMetaObject::invokeMethod(this, [this, current, text, conf] {
                                    if (current()) {
                                        emit textDecoded(text, conf);
                                    }
                                }, Qt::QueuedConnection);
                            }
                        }
                    }
                }
            }
        }

        // Emit stats ~2× per second
        statsTick += kChunkSamples;
        if (statsTick >= 12000) {
            statsTick = 0;
            // From the average character score, once it holds two characters: a carrier
            // switching on can frame one, and a steady carrier frames nothing more, so it
            // does not lock and leaves the level at 0.
            float level = 0.0f;
            float snrDb = 0.0f;
            if (m_avgFrames < kMinCharacters
                || m_samplesSinceFrame > static_cast<qint64>(kIdleSeconds * kSampleRate)) {
                m_lockState = false;
            } else {
                const double score = scoreFromShare(m_avgShare, m_cleanShare);
                m_lockState = score >= (m_lockState ? kUnlockScore : kLockScore);
                level = static_cast<float>(2.0 * (score - 0.5));
                const double m = std::clamp(m_avgShare, 1e-6, 1.0 - 1e-6);
                snrDb = static_cast<float>(10.0 * std::log10(m / (1.0 - m)));
            }
            const bool locked = m_lockState;
            QMetaObject::invokeMethod(this,
                [this, current, level, snrDb, locked] {
                    if (current()) {
                        emit statsUpdated(level, snrDb, locked);
                    }
                }, Qt::QueuedConnection);
        }
    }
}

} // namespace AetherSDR
