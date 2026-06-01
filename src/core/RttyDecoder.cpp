#include "RttyDecoder.h"
#include "LogManager.h"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace AetherSDR {

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
    if (m_running) return;

    m_running       = true;
    m_paramsChanged = true;

    {
        QMutexLocker lock(&m_bufMutex);
        m_ringBuf.clear();
    }

    auto* worker = QThread::create([this]() { decodeLoop(); });
    worker->setObjectName("RttyDecoder");
    connect(worker, &QThread::finished, worker, &QThread::deleteLater);
    m_workerThread = worker;
    worker->start();

    qCDebug(lcDsp) << "RttyDecoder: started baud=" << m_baudRate.load()
                   << "shift=" << m_shiftHz.load() << "mark=" << m_markFreqHz.load();
}

void RttyDecoder::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_workerThread) {
        if (!m_workerThread->wait(2000))
            qCWarning(lcDsp) << "RttyDecoder: worker did not stop within 2 s — abandoning";
        m_workerThread = nullptr;
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
    if (!m_running) return;

    const auto* src = reinterpret_cast<const float*>(pcm24kStereoFloat.constData());
    const int frames = pcm24kStereoFloat.size() / (2 * static_cast<int>(sizeof(float)));

    QByteArray mono(frames * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<float*>(mono.data());
    for (int i = 0; i < frames; ++i)
        dst[i] = (src[2 * i] + src[2 * i + 1]) * 0.5f;

    QMutexLocker lock(&m_bufMutex);
    m_ringBuf.append(mono);
    if (m_ringBuf.size() > kRingCapacity)
        m_ringBuf.remove(0, m_ringBuf.size() - kRingCapacity);
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

    m_markCoeffs  = designBandpass(markHz,  bw, kSampleRate);
    m_spaceCoeffs = designBandpass(spaceHz, bw, kSampleRate);
    m_markState   = {};
    m_spaceState  = {};
    m_markEnv     = 0;
    m_spaceEnv    = 0;

    qCDebug(lcDsp) << "RttyDecoder: mark=" << markHz << "space=" << spaceHz
                   << "bw=" << bw << "baud=" << baud << "rev=" << m_reverse.load();
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
        {
            QMutexLocker lock(&m_bufMutex);
            if (m_ringBuf.size() < kChunkBytes) {
                lock.unlock();
                QThread::msleep(10);
                continue;
            }
            chunk = m_ringBuf.left(kChunkBytes);
            m_ringBuf.remove(0, kChunkBytes);
        }

        const auto* samples = reinterpret_cast<const float*>(chunk.constData());

        for (int i = 0; i < kChunkSamples; ++i) {
            const double x = samples[i];

            // Bandpass + envelope
            const double markOut  = processBiquad(m_markCoeffs,  m_markState,  x);
            const double spaceOut = processBiquad(m_spaceCoeffs, m_spaceState, x);
            m_markEnv  += envAlpha * (std::abs(markOut)  - m_markEnv);
            m_spaceEnv += envAlpha * (std::abs(spaceOut) - m_spaceEnv);

            // Schmitt trigger: hysteresis prevents chattering at the boundary
            if (m_markEnv  > m_spaceEnv * (1.0 + kHyst))  curBit = 1;
            else if (m_spaceEnv > m_markEnv * (1.0 + kHyst))  curBit = 0;
            // else: keep curBit

            // Start-bit detection: falling edge (mark→space) while idle
            if (!inChar && prevBit == 1 && curBit == 0) {
                inChar      = true;
                bitCount    = 0;
                shiftReg    = 0;
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
                    shiftReg |= (curBit << bitCount);
                    ++bitCount;
                } else {
                    // Stop bit — must be mark; discard character if not
                    inChar = false;
                    if (curBit == 1) {
                        if (shiftReg == kBaudotLtrs) {
                            figsMode = false;
                        } else if (shiftReg == kBaudotFigs) {
                            figsMode = true;
                        } else if (shiftReg != kBaudotNull) {
                            char ch = baudotToAscii(shiftReg, figsMode);
                            if (ch != '\0' && ch != '\a') {
                                const double total = m_markEnv + m_spaceEnv;
                                float conf = 0.5f;
                                if (total > 1e-8)
                                    conf = static_cast<float>(
                                        std::max(m_markEnv, m_spaceEnv) / total);
                                emit textDecoded(QString(QChar::fromLatin1(ch)), conf);
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
            const double total = m_markEnv + m_spaceEnv;
            float markLevel  = 0.5f;
            float spaceLevel = 0.5f;
            float snrDb      = 0.0f;
            bool  locked     = false;
            if (total > 1e-8) {
                markLevel  = static_cast<float>(m_markEnv  / total);
                spaceLevel = static_cast<float>(m_spaceEnv / total);
                const double ratio = std::max(m_markEnv, m_spaceEnv) / total;
                snrDb  = static_cast<float>(10.0 * std::log10(ratio / (1.0 - ratio + 1e-12)));
                locked = snrDb > 3.0f;
            }
            emit statsUpdated(markLevel, spaceLevel, snrDb, locked);
        }
    }
}

} // namespace AetherSDR
