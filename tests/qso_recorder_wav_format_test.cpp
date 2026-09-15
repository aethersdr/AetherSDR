#include "core/QsoWavFormat.h"

#include <QBuffer>
#include <QByteArray>
#include <QIODevice>
#include <QMap>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void put16(QByteArray& bytes, qsizetype offset, quint16 value)
{
    bytes[offset] = static_cast<char>(value & 0xffU);
    bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
}

void put32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    for (int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 0xffU);
    }
}

QByteArray format(int rate = 24000, int channels = 2)
{
    // Independently encoded WAVEFORMAT fields; no production writer involved.
    QByteArray bytes(16, '\0');
    put16(bytes, 0, 1);
    put16(bytes, 2, static_cast<quint16>(channels));
    put32(bytes, 4, static_cast<quint32>(rate));
    put32(bytes, 8, static_cast<quint32>(rate * channels * 2));
    put16(bytes, 12, static_cast<quint16>(channels * 2));
    put16(bytes, 14, 16);
    return bytes;
}

QByteArray chunk(const char* id, const QByteArray& payload)
{
    QByteArray bytes(id, 4);
    bytes.append(4, '\0');
    put32(bytes, 4, static_cast<quint32>(payload.size()));
    bytes.append(payload);
    if ((payload.size() & 1) != 0) {
        bytes.append('\0');
    }
    return bytes;
}

QByteArray wave(const QByteArray& chunks)
{
    QByteArray bytes("RIFF\0\0\0\0WAVE", 12);
    put32(bytes, 4, static_cast<quint32>(chunks.size() + 4));
    bytes.append(chunks);
    return bytes;
}

QByteArray validWave(int rate = 24000, int channels = 2)
{
    return wave(chunk("fmt ", format(rate, channels))
                + chunk("data", QByteArray::fromHex("001001f0002002e0")));
}

bool rejected(QByteArray bytes, const char* message)
{
    QBuffer source(&bytes);
    source.open(QIODevice::ReadOnly);
    QString error;
    const std::optional<AetherSDR::QsoWavFormat> result =
        AetherSDR::parseQsoWav(source, &error);
    bool ok = expect(!result, message);
    ok &= expect(!error.isEmpty(), "rejection supplies an error");
    return ok;
}

// Exposes declared multi-gigabyte ranges without allocating them. Read/seek
// failures and short reads exercise the parser's actual QIODevice boundary.
class SparseSource : public QIODevice {
public:
    explicit SparseSource(qint64 length) : m_length(length)
    {
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    qint64 size() const override { return m_length; }
    bool isSequential() const override { return sequential; }
    bool seek(qint64 offset) override
    {
        if (offset == failedSeek || offset < 0 || offset > m_length) {
            return false;
        }
        return QIODevice::seek(offset);
    }

    QMap<qint64, QByteArray> regions;
    qint64 failedSeek = -1;
    qint64 failedRead = -1;
    qint64 readLimit = std::numeric_limits<qint64>::max();
    qint64 maxRead = 0;
    qint64 totalRead = 0;
    bool sequential = false;

protected:
    qint64 readData(char* output, qint64 requested) override
    {
        maxRead = std::max(maxRead, requested);
        if (pos() >= failedRead && failedRead >= 0) {
            return -1;
        }
        const qint64 length = std::min({requested, m_length - pos(), readLimit});
        if (length <= 0) {
            return 0;
        }
        std::memset(output, 0, static_cast<size_t>(length));
        for (QMap<qint64, QByteArray>::const_iterator it = regions.cbegin();
             it != regions.cend(); ++it) {
            const qint64 start = std::max(pos(), it.key());
            const qint64 end = std::min(pos() + length, it.key() + it.value().size());
            if (start < end) {
                std::memcpy(output + start - pos(),
                            it.value().constData() + start - it.key(),
                            static_cast<size_t>(end - start));
            }
        }
        totalRead += length;
        return length;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    qint64 m_length;
};

} // namespace

int main()
{
    bool ok = true;
    for (int rate : {24000, 44100, 48000}) {
        for (int channels : {1, 2}) {
            QByteArray bytes = validWave(rate, channels);
            QBuffer source(&bytes);
            source.open(QIODevice::ReadOnly);
            source.seek(7); // Parsing is always from byte zero.
            QString error = QStringLiteral("stale error");
            const std::optional<AetherSDR::QsoWavFormat> parsed =
                AetherSDR::parseQsoWav(source, &error);
            ok &= expect(parsed.has_value(), "PCM16 supported rate/channel pair accepted");
            if (parsed) {
                ok &= expect(parsed->sampleRate == rate, "actual sample rate retained");
                ok &= expect(parsed->channelCount == channels, "actual channel count retained");
                ok &= expect(parsed->dataOffset == 44, "canonical data offset is 44");
                ok &= expect(parsed->dataBytes == 8, "data size excludes header");
                ok &= expect(parsed->frameCount == (channels == 1 ? 4 : 2),
                             "frame count uses complete channel frames");
                ok &= expect(source.pos() == 44 && source.read(4) == QByteArray::fromHex("001001f0"),
                             "success positions source at first PCM sample");
                ok &= expect(error.isEmpty(), "success clears stale error");
            }
        }
    }

    {
        QByteArray extended = format(48000, 1);
        extended.append(QByteArray::fromHex("0300aabbcc"));
        QByteArray bytes = wave(chunk("JUNK", QByteArray("abc"))
            + chunk("data", QByteArray::fromHex("3412cced"))
            + chunk("fmt ", extended) + chunk("LIST", QByteArray("metadata")));
        bytes.append("outside RIFF is not PCM");
        QBuffer source(&bytes);
        source.open(QIODevice::ReadOnly);
        const std::optional<AetherSDR::QsoWavFormat> parsed = AetherSDR::parseQsoWav(source);
        ok &= expect(parsed && parsed->sampleRate == 48000 && parsed->channelCount == 1
                     && parsed->dataOffset == 32 && parsed->dataBytes == 4
                     && parsed->frameCount == 2,
                     "odd unknown/extended fmt chunks and data-before-fmt preserve PCM bounds");
    }

    const QByteArray valid = validWave();
    for (qsizetype length = 0; length < valid.size(); ++length) {
        ok &= rejected(valid.first(length), "every truncated canonical WAV prefix rejected");
    }
    for (const char* signature : {"RIFX", "RF64", "OggS"}) {
        QByteArray bytes = valid;
        bytes.replace(qsizetype(0), 4, signature, 4);
        ok &= rejected(bytes, "unsupported container rejected");
    }
    {
        QByteArray bytes = valid;
        bytes.replace(8, 4, "AVI ");
        ok &= rejected(bytes, "non-WAVE RIFF rejected");
        for (quint32 size : {0U, 3U, 4U, 35U, 37U, 0xffffffffU}) {
            bytes = valid;
            put32(bytes, 4, size);
            ok &= rejected(bytes, "RIFF bounds cannot expose absent or partial chunks");
        }
    }
    ok &= rejected(wave(chunk("data", QByteArray(4, '\0'))), "missing fmt rejected");
    ok &= rejected(wave(chunk("fmt ", format())), "missing data rejected");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", {})), "empty data rejected");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(6, '\0'))),
                   "stereo data ending mid-frame rejected");
    ok &= rejected(wave(chunk("fmt ", format(24000, 1)) + chunk("data", QByteArray(3, '\0'))),
                   "mono data ending mid-sample rejected");
    ok &= rejected(wave(chunk("fmt ", format().first(15)) + chunk("data", QByteArray(4, '\0'))),
                   "short fmt rejected");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0'))
                        + chunk("fmt ", format())), "duplicate fmt after data rejected");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0'))
                        + chunk("data", QByteArray(4, '\0'))), "duplicate data rejected");

    for (quint16 tag : {0U, 2U, 3U, 0xfffeU, 0xffffU}) {
        QByteArray bytes = valid;
        put16(bytes, 20, tag);
        ok &= rejected(bytes, "compressed/float/extensible/unknown encoding rejected");
    }
    for (quint16 channels : {0U, 3U, 0xffffU}) {
        QByteArray bytes = valid;
        put16(bytes, 22, channels);
        ok &= rejected(bytes, "unsupported channel count rejected");
    }
    for (quint32 rate : {0U, 1U, 23999U, 48001U, 0xffffffffU}) {
        QByteArray bytes = valid;
        put32(bytes, 24, rate);
        ok &= rejected(bytes, "invalid or unsupported rate rejected");
    }
    for (quint16 bits : {0U, 8U, 24U, 32U, 0xffffU}) {
        QByteArray bytes = valid;
        put16(bytes, 34, bits);
        ok &= rejected(bytes, "unsupported sample depth rejected");
    }
    for (quint16 align : {0U, 2U, 6U, 0xffffU}) {
        QByteArray bytes = valid;
        put16(bytes, 32, align);
        ok &= rejected(bytes, "inconsistent block alignment rejected");
    }
    for (quint32 byteRate : {0U, 48000U, 96001U, 0xffffffffU}) {
        QByteArray bytes = valid;
        put32(bytes, 28, byteRate);
        ok &= rejected(bytes, "inconsistent byte rate rejected");
    }
    for (qsizetype offset : {qsizetype(16), qsizetype(40)}) {
        QByteArray bytes = valid;
        put32(bytes, offset, 0xffffffffU);
        ok &= rejected(bytes, "chunk lengths cannot overflow or exceed RIFF boundary");
    }
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0'))
                        + QByteArray("JUNK\xff\xff\xff\xff", 8)),
                   "invalid unknown chunk after data is still validated");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0'))
                        + QByteArray("JUNK\1\0\0\0x", 9)),
                   "missing final odd-chunk padding rejected");
    ok &= rejected(wave(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0'))
                        + QByteArray("junk", 4)), "partial trailing chunk header rejected");

    {
        QByteArray chunks;
        for (int index = 0; index < 4094; ++index) {
            chunks.append(chunk("JUNK", {}));
        }
        chunks.append(chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0')));
        QByteArray bytes = wave(chunks);
        QBuffer source(&bytes);
        source.open(QIODevice::ReadOnly);
        ok &= expect(AetherSDR::parseQsoWav(source).has_value(), "4096 total chunks accepted");
        ok &= rejected(wave(chunk("JUNK", {}) + chunks), "chunk scan budget bounds CPU work");
    }

    {
        QByteArray prefix = valid.first(44);
        put32(prefix, 4, 0xfffffffcU);
        put32(prefix, 40, 0xffffffd8U);
        SparseSource source(0x100000004LL);
        source.regions.insert(0, prefix);
        const std::optional<AetherSDR::QsoWavFormat> parsed = AetherSDR::parseQsoWav(source);
        ok &= expect(parsed && parsed->dataBytes == 4294967256LL
                     && parsed->frameCount == 1073741814LL && parsed->dataOffset == 44,
                     "multi-gigabyte data keeps 64-bit metadata without payload allocation");
        ok &= expect(source.maxRead <= 16 && source.totalRead <= 44,
                     "metadata inspection never requests PCM payload");
        ok &= rejected(prefix, "huge data declaration in physically short file rejected");
    }
    {
        QByteArray prefix("RIFF\0\0\0\0WAVEJUNK\0\0\0\0", 20);
        put32(prefix, 4, 0xfffff030U);
        put32(prefix, 16, 0xfffff000U);
        SparseSource source(0xfffff038LL);
        source.regions.insert(0, prefix);
        source.regions.insert(0xfffff014LL,
                              chunk("fmt ", format()) + chunk("data", QByteArray(4, '\0')));
        const std::optional<AetherSDR::QsoWavFormat> parsed = AetherSDR::parseQsoWav(source);
        ok &= expect(parsed && parsed->dataOffset == 0xfffff034LL && parsed->frameCount == 1,
                     "large unknown chunk skipped with 64-bit offsets");
        ok &= expect(source.maxRead <= 16 && source.totalRead <= 52,
                     "large unknown chunk does not cause proportional reads");
    }
    {
        SparseSource source(valid.size());
        source.regions.insert(0, valid);
        source.readLimit = 1;
        ok &= expect(AetherSDR::parseQsoWav(source).has_value(), "short positive reads are assembled");
        source.failedRead = 20;
        ok &= expect(!AetherSDR::parseQsoWav(source), "header read failure rejected");
        source.failedRead = -1;
        source.readLimit = 0;
        ok &= expect(!AetherSDR::parseQsoWav(source), "zero-progress read fails without retry loop");
        source.readLimit = 16;
        source.failedSeek = 44;
        ok &= expect(!AetherSDR::parseQsoWav(source), "failure to position at PCM is rejected");
        source.failedSeek = 0;
        ok &= expect(!AetherSDR::parseQsoWav(source), "initial seek failure rejected");
        source.failedSeek = -1;
        source.sequential = true;
        ok &= expect(!AetherSDR::parseQsoWav(source), "sequential source rejected");
    }
    {
        QByteArray bytes = valid;
        QBuffer source(&bytes);
        ok &= expect(!AetherSDR::parseQsoWav(source), "closed source rejected");
        source.open(QIODevice::WriteOnly);
        ok &= expect(!AetherSDR::parseQsoWav(source), "write-only source rejected");
        source.close();
        source.open(QIODevice::ReadOnly | QIODevice::Text);
        ok &= expect(!AetherSDR::parseQsoWav(source), "text-mode source rejected");
    }

    if (ok) {
        std::cout << "QSO WAV format checks passed\n";
    }
    return ok ? 0 : 1;
}
