// #6244 — a DVK slot plays its samples at 24 kHz and uses only the first
// channel (FLEX-8600, fw 4.2.20: a 2 s 48 kHz file previews for 4 s). Every
// import must therefore come out as 24 kHz mono 16-bit PCM of the same length
// and pitch, whatever WAV the user picked.
#include "core/DvkWavConverter.h"

#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

using namespace AetherSDR;

namespace {

constexpr double kPi = 3.14159265358979323846;

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

void put16(QByteArray& b, quint16 v) { char c[2]; qToLittleEndian(v, c); b.append(c, 2); }
void put32(QByteArray& b, quint32 v) { char c[4]; qToLittleEndian(v, c); b.append(c, 4); }

QByteArray chunk(const char* id, const QByteArray& body)
{
    QByteArray c(id, 4);
    put32(c, static_cast<quint32>(body.size()));
    c += body;
    if (body.size() & 1) {
        c.append('\0');
    }
    return c;
}

struct Spec {
    quint16 tag = 1;            // 1 PCM, 3 float
    int bits = 16;
    int channels = 2;
    int rate = 48'000;
    double seconds = 2.0;
    double hz = 700.0;
    bool extensible = false;
    bool listChunkFirst = false;
    bool dataBeforeFmt = false;
};

QByteArray makeWav(const Spec& s)
{
    const int frames = static_cast<int>(std::lround(s.seconds * s.rate));
    const int bytes = s.bits / 8;
    QByteArray data;
    for (int i = 0; i < frames; ++i) {
        const double v = 0.5 * std::sin(2.0 * kPi * s.hz * i / s.rate);
        for (int c = 0; c < s.channels; ++c) {
            if (s.tag == 3) {
                const float f = static_cast<float>(v);
                quint32 raw = 0;
                std::memcpy(&raw, &f, 4);
                put32(data, raw);
            } else if (bytes == 2) {
                put16(data, static_cast<quint16>(static_cast<qint16>(std::lround(v * 32767))));
            } else if (bytes == 3) {
                const qint32 x = static_cast<qint32>(std::lround(v * 8388607));
                data.append(static_cast<char>(x & 0xFF));
                data.append(static_cast<char>((x >> 8) & 0xFF));
                data.append(static_cast<char>((x >> 16) & 0xFF));
            } else if (bytes == 4) {
                put32(data, static_cast<quint32>(static_cast<qint32>(std::lround(v * 2147483647.0))));
            } else {
                data.append(static_cast<char>(std::lround(v * 127) + 128));
            }
        }
    }

    QByteArray fmt;
    put16(fmt, s.extensible ? 0xFFFE : s.tag);
    put16(fmt, static_cast<quint16>(s.channels));
    put32(fmt, static_cast<quint32>(s.rate));
    put32(fmt, static_cast<quint32>(s.rate * s.channels * bytes));
    put16(fmt, static_cast<quint16>(s.channels * bytes));
    put16(fmt, static_cast<quint16>(s.bits));
    if (s.extensible) {
        put16(fmt, 22);
        put16(fmt, static_cast<quint16>(s.bits));
        put32(fmt, 0);
        put16(fmt, s.tag);  // first two bytes of the sub-format GUID
        fmt.append(QByteArray(14, '\x01'));
    }

    QByteArray body("WAVE");
    if (s.listChunkFirst) {
        body += chunk("LIST", QByteArray("INFOISFT\x05\0\0\0test\0", 17));
    }
    if (s.dataBeforeFmt) {
        body += chunk("data", data);
        body += chunk("fmt ", fmt);
    } else {
        body += chunk("fmt ", fmt);
        body += chunk("data", data);
    }
    QByteArray wav("RIFF");
    put32(wav, static_cast<quint32>(body.size()));
    return wav + body;
}

struct Decoded {
    bool ok = false;
    int tag = 0, channels = 0, rate = 0, bits = 0;
    std::vector<double> samples;
};

Decoded decode(const QByteArray& wav)
{
    Decoded d;
    if (wav.size() < 44 || !wav.startsWith("RIFF")) {
        return d;
    }
    const char* p = wav.constData();
    d.tag = qFromLittleEndian<quint16>(p + 20);
    d.channels = qFromLittleEndian<quint16>(p + 22);
    d.rate = static_cast<int>(qFromLittleEndian<quint32>(p + 24));
    d.bits = qFromLittleEndian<quint16>(p + 34);
    const quint32 dataBytes = qFromLittleEndian<quint32>(p + 40);
    if (wav.mid(36, 4) != "data" || 44 + static_cast<qint64>(dataBytes) != wav.size()) {
        return d;
    }
    for (quint32 i = 0; i < dataBytes; i += 2) {
        d.samples.push_back(qFromLittleEndian<qint16>(p + 44 + i) / 32768.0);
    }
    d.ok = true;
    return d;
}

double estimateHz(const std::vector<double>& x, int rate)
{
    // Count rising zero crossings over the middle of the clip, away from the
    // resampler's edges.
    const size_t from = x.size() / 10;
    const size_t to = x.size() - x.size() / 10;
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0 && x[i] >= 0.0) {
            ++crossings;
        }
    }
    return crossings * static_cast<double>(rate) / static_cast<double>(to - from);
}

double rms(const std::vector<double>& x)
{
    double sum = 0.0;
    const size_t from = x.size() / 10;
    const size_t to = x.size() - x.size() / 10;
    for (size_t i = from; i < to; ++i) {
        sum += x[i] * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

void checkConverts(const char* label, const Spec& spec)
{
    QString error;
    const Decoded out = decode(DvkWavConverter::convertForRadio(makeWav(spec), error));
    const size_t expected = static_cast<size_t>(std::lround(spec.seconds * 24'000));
    const bool format = out.ok && out.tag == 1 && out.channels == 1 && out.rate == 24'000
                     && out.bits == 16;
    const bool length = out.samples.size() == expected;
    const double hz = out.ok ? estimateHz(out.samples, 24'000) : 0.0;
    const double level = out.ok ? rms(out.samples) : 0.0;
    const bool pitch = std::abs(hz - spec.hz) < 5.0;
    const bool amplitude = std::abs(level - 0.5 / std::sqrt(2.0)) < 0.02;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%s -> 24k mono PCM16 (len %zu/%zu, %.1f Hz, rms %.3f)%s%s",
                  label, out.samples.size(), expected, hz, level,
                  error.isEmpty() ? "" : " error: ", qPrintable(error));
    report(line, format && length && pitch && amplitude);
}

// A lone click at exactly 0.5 s must land at output sample 12000: the
// resampler's group delay is trimmed from the head, not left in or over-cut.
// A click, unlike a tone, has no period a wrong trim could alias onto.
void checkClickAligned(int rate)
{
    const int frames = rate;  // 1 s, mono 16-bit
    QByteArray data(frames * 2, '\0');
    char* click = data.data() + (rate / 2) * 2;
    qToLittleEndian<qint16>(29000, click);
    QByteArray fmt;
    put16(fmt, 1); put16(fmt, 1); put32(fmt, static_cast<quint32>(rate));
    put32(fmt, static_cast<quint32>(rate * 2)); put16(fmt, 2); put16(fmt, 16);
    QByteArray body("WAVE");
    body += chunk("fmt ", fmt);
    body += chunk("data", data);
    QByteArray wav("RIFF");
    put32(wav, static_cast<quint32>(body.size()));
    QString error;
    const Decoded out = decode(DvkWavConverter::convertForRadio(wav + body, error));
    size_t peak = 0;
    for (size_t i = 1; i < out.samples.size(); ++i) {
        if (std::abs(out.samples[i]) > std::abs(out.samples[peak])) {
            peak = i;
        }
    }
    char line[160];
    std::snprintf(line, sizeof(line), "click at 0.5 s from %d Hz lands at sample %zu (want 12000)",
                  rate, peak);
    report(line, out.ok && peak + 1 >= 12000 && peak <= 12001);
}

void checkRefused(const char* label, const QByteArray& wav, const char* needle)
{
    QString error;
    const QByteArray out = DvkWavConverter::convertForRadio(wav, error);
    char line[256];
    std::snprintf(line, sizeof(line), "%s refused (\"%s\")", label, qPrintable(error));
    report(line, out.isEmpty() && error.contains(QLatin1String(needle)));
}

}  // namespace

int main()
{
    checkConverts("PCM16 stereo 48 kHz", Spec{});
    checkConverts("PCM16 mono 24 kHz passthrough", Spec{.channels = 1, .rate = 24'000});
    checkConverts("float32 stereo 48 kHz", Spec{.tag = 3, .bits = 32});
    checkConverts("PCM24 mono 44.1 kHz", Spec{.bits = 24, .channels = 1, .rate = 44'100});
    checkConverts("PCM32 stereo 96 kHz", Spec{.bits = 32, .rate = 96'000});
    checkConverts("PCM8 mono 8 kHz", Spec{.bits = 8, .channels = 1, .rate = 8'000, .hz = 500.0});
    checkConverts("WAVE_FORMAT_EXTENSIBLE float", Spec{.tag = 3, .bits = 32, .extensible = true});
    checkConverts("LIST chunk before fmt", Spec{.listChunkFirst = true});
    checkConverts("data chunk before fmt", Spec{.dataBeforeFmt = true});
    checkConverts("exactly 10 s", Spec{.channels = 1, .seconds = 10.0});

    for (int rate : {8'000, 24'000, 44'100, 48'000, 96'000, 192'000}) {
        checkClickAligned(rate);
    }

    checkRefused("10.5 s clip", makeWav(Spec{.channels = 1, .seconds = 10.5}), "at most 10 s");
    checkRefused("not RIFF", QByteArray("hello world, not a wav"), "Not a WAV");
    checkRefused("A-law", makeWav(Spec{.tag = 6, .bits = 8, .channels = 1}), "Unsupported");
    {
        QByteArray noData("WAVE");
        QByteArray fmt;
        put16(fmt, 1); put16(fmt, 1); put32(fmt, 48'000); put32(fmt, 96'000);
        put16(fmt, 2); put16(fmt, 16);
        noData += chunk("fmt ", fmt);
        QByteArray wav("RIFF");
        put32(wav, static_cast<quint32>(noData.size()));
        checkRefused("no data chunk", wav + noData, "no audio data");
    }

    std::printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
