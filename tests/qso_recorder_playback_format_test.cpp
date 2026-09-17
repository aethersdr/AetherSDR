#include "core/QsoWavPlayback.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

using namespace AetherSDR;

static int failures = 0;
static int checks = 0;
static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

static void u16(QByteArray& bytes, quint16 value)
{
    char data[2];
    qToLittleEndian(value, data);
    bytes.append(data, 2);
}

static void u32(QByteArray& bytes, quint32 value)
{
    char data[4];
    qToLittleEndian(value, data);
    bytes.append(data, 4);
}

static QByteArray chunk(const char* name, const QByteArray& payload)
{
    QByteArray bytes(name, 4);
    u32(bytes, payload.size());
    bytes.append(payload);
    if (payload.size() % 2 != 0) {
        bytes.append('\0');
    }
    return bytes;
}

static QByteArray fixture(int rate, int channels, int frames, bool extraChunks = false)
{
    QByteArray fmt;
    u16(fmt, 1);
    u16(fmt, channels);
    u32(fmt, rate);
    u32(fmt, rate * channels * 2);
    u16(fmt, channels * 2);
    u16(fmt, 16);
    QByteArray pcm;
    for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const double frequency = channel == 0 ? 443.0 : 1703.0;
            const qint16 sample = static_cast<qint16>(12000 * std::sin(
                2 * std::numbers::pi * frequency * frame / rate));
            u16(pcm, static_cast<quint16>(sample));
        }
    }
    QByteArray body("WAVE");
    if (extraChunks) {
        body += chunk("JUNK", QByteArray("abc"));
        fmt.append("\0\0", 2);
    }
    body += chunk("fmt ", fmt);
    body += chunk("data", pcm);
    if (extraChunks) {
        body += chunk("LIST", QByteArray(65, 'Z'));
    }
    QByteArray wav("RIFF");
    u32(wav, body.size());
    wav += body;
    if (extraChunks) {
        wav.append("outside RIFF is not audio");
    }
    return wav;
}

static QAudioFormat sink(int rate, int channels, QAudioFormat::SampleFormat encoding)
{
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(channels);
    format.setSampleFormat(encoding);
    return format;
}

class ShortReadBuffer final : public QBuffer {
public:
    enum class Fault { None, ZeroProgress, ReadError };
    ShortReadBuffer(QByteArray* bytes, Fault fault = Fault::None)
        : QBuffer(bytes), m_fault(fault) {}
    int payloadReads = 0;

protected:
    qint64 readData(char* output, qint64 requested) override
    {
        if (pos() >= 44) {
            ++payloadReads;
            if (payloadReads > 1 && m_fault != Fault::None) {
                return m_fault == Fault::ReadError ? -1 : 0;
            }
        }
        return QBuffer::readData(output, std::min<qint64>(requested, 7));
    }

private:
    Fault m_fault;
};

static float sampleAt(const QByteArray& bytes, int index, QAudioFormat::SampleFormat encoding)
{
    if (encoding == QAudioFormat::Float) {
        float sample;
        std::memcpy(&sample, bytes.constData() + index * 4, 4);
        return sample;
    }
    qint16 sample;
    std::memcpy(&sample, bytes.constData() + index * 2, 2);
    return sample / 32768.0f;
}

static double magnitude(const QByteArray& bytes, int channel, int rate,
                        double frequency, QAudioFormat::SampleFormat encoding)
{
    const int frames = bytes.size() / (encoding == QAudioFormat::Float ? 8 : 4);
    double real = 0;
    double imag = 0;
    const int trim = rate / 100;
    for (int frame = trim; frame < frames - trim; ++frame) {
        const double phase = 2 * std::numbers::pi * frequency * frame / rate;
        const double value = sampleAt(bytes, frame * 2 + channel, encoding);
        real += value * std::cos(phase);
        imag += value * std::sin(phase);
    }
    return std::hypot(real, imag) / (frames - 2 * trim);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    for (int sourceRate : {24000, 44100, 48000}) {
        for (int outputRate : {8000, 24000, 44100, 48000, 96000}) {
            for (QAudioFormat::SampleFormat encoding : {QAudioFormat::Int16, QAudioFormat::Float}) {
                QByteArray wav = fixture(sourceRate, 2, sourceRate / 4, true);
                QBuffer source(&wav);
                check(source.open(QIODevice::ReadOnly), "open generated fixture");
                QString error = QStringLiteral("old error");
                const auto output = prepareQsoWavPlayback(source, sink(outputRate, 2, encoding), &error);
                check(output.has_value() && error.isEmpty(), "actual-format playback matrix prepares");
                if (!output) {
                    qCritical() << error << sourceRate << outputRate;
                    continue;
                }
                const qint64 frames = (qint64{sourceRate / 4} * outputRate + sourceRate / 2) / sourceRate;
                check(output->size() == frames * (encoding == QAudioFormat::Float ? 8 : 4),
                      "actual source rate sets playback duration without trailing chunks");
                check(magnitude(*output, 0, outputRate, 443, encoding) > 0.15,
                      "left pitch retained using file metadata");
                check(magnitude(*output, 1, outputRate, 1703, encoding) > 0.15,
                      "right pitch retained using file metadata");
                check(magnitude(*output, 0, outputRate, 1703, encoding) < 0.01
                          && magnitude(*output, 1, outputRate, 443, encoding) < 0.01,
                      "playback retains distinct stereo channels");
            }
        }
    }

    QByteArray mono = fixture(24000, 1, 121);
    QBuffer source(&mono);
    check(source.open(QIODevice::ReadOnly), "open short mono fixture");
    const auto stereo = prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Float));
    check(stereo && stereo->size() == 242 * 8, "short mono file retains its complete converted duration");
    bool same = stereo.has_value();
    float peak = 0;
    if (stereo) {
        for (int frame = 0; frame < 242; ++frame) {
            const float left = sampleAt(*stereo, frame * 2, QAudioFormat::Float);
            same = same && left == sampleAt(*stereo, frame * 2 + 1, QAudioFormat::Float);
            peak = std::max(peak, std::abs(left));
        }
    }
    check(same && peak > 0.2f, "short-file tail contains real audio with mono duplicated");

    QString error;
    // The budget counts converted FRAMES, so the same ceiling holds whatever
    // sample format the sink negotiated. 1936 output bytes at Float stereo
    // (8 B/frame) is 242 frames; one frame short must still refuse.
    check(!prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Float), &error, 241)
              && !error.isEmpty(), "output budget rejects before payload conversion");
    check(source.pos() == 44, "over-budget file stops at parsed data start");
    check(prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Float), &error, 242).has_value()
              && error.isEmpty(), "exact output budget succeeds");
    // The point of counting frames: an Int16 sink at the same rate gets the
    // same duration, where a byte budget would have given it twice as much.
    check(!prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Int16), &error, 241)
              && !error.isEmpty(), "frame budget is independent of sample format");
    check(prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Int16), &error, 242).has_value()
              && error.isEmpty(), "Int16 sink gets the same frame ceiling as Float");
    check(!prepareQsoWavPlayback(source, sink(48000, 2, QAudioFormat::Int32)),
          "unsupported negotiated encoding is not misinterpreted");
    check(!prepareQsoWavPlayback(source, sink(192001, 2, QAudioFormat::Int16)),
          "unsupported negotiated rate fails explicitly");
    check(!prepareQsoWavPlayback(source, sink(24000, 0, QAudioFormat::Int16)),
          "invalid negotiated layout fails explicitly");

    ShortReadBuffer shortReads(&mono);
    check(shortReads.open(QIODevice::ReadOnly | QIODevice::Unbuffered), "open short-read source");
    const auto shortOutput = prepareQsoWavPlayback(shortReads, sink(48000, 2, QAudioFormat::Float), &error);
    check(shortOutput && stereo && *shortOutput == *stereo && error.isEmpty(),
          "positive short reads preserve the complete playback PCM");
    for (ShortReadBuffer::Fault fault : {ShortReadBuffer::Fault::ZeroProgress,
                                       ShortReadBuffer::Fault::ReadError}) {
        ShortReadBuffer broken(&mono, fault);
        check(broken.open(QIODevice::ReadOnly | QIODevice::Unbuffered), "open interrupted source");
        check(!prepareQsoWavPlayback(broken, sink(48000, 2, QAudioFormat::Float), &error)
                  && !error.isEmpty() && broken.payloadReads == 2,
              "payload zero-progress or read error fails promptly without partial output");
    }
    mono.chop(1);
    check(!prepareQsoWavPlayback(source, sink(24000, 2, QAudioFormat::Int16)),
          "truncated file cannot become playback PCM");
    qInfo() << checks << "checks," << failures << "failures";
    return failures == 0 ? 0 : 1;
}
