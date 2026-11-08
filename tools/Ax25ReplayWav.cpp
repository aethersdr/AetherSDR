#include "Ax25ReplayWav.h"

#include <QByteArray>
#include <QFile>

#include <cstring>

namespace {

quint16 readLe16(const char* b)
{
    return static_cast<quint16>(static_cast<unsigned char>(b[0]))
        | (static_cast<quint16>(static_cast<unsigned char>(b[1])) << 8);
}

quint32 readLe32(const char* b)
{
    return static_cast<quint32>(static_cast<unsigned char>(b[0]))
        | (static_cast<quint32>(static_cast<unsigned char>(b[1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(b[2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(b[3])) << 24);
}

} // namespace

namespace AetherSDR::Ax25ReplayWav {

bool loadWav(const QString& path, std::vector<float>& samples, int& sampleRate, QString& err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() < 44 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        err = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }
    quint16 format = 0, channels = 0, bits = 0;
    const char* data = nullptr;
    qsizetype dataBytes = 0;
    qsizetype pos = 12;
    while (pos + 8 <= bytes.size()) {
        const QByteArray id = bytes.mid(pos, 4);
        const quint32 sz = readLe32(bytes.constData() + pos + 4);
        pos += 8;
        if (pos + static_cast<qsizetype>(sz) > bytes.size()) {
            break;
        }
        if (id == "fmt " && sz >= 16) {
            const char* fmt = bytes.constData() + pos;
            format = readLe16(fmt);
            channels = readLe16(fmt + 2);
            sampleRate = static_cast<int>(readLe32(fmt + 4));
            bits = readLe16(fmt + 14);
        } else if (id == "data") {
            data = bytes.constData() + pos;
            dataBytes = static_cast<qsizetype>(sz);
        }
        pos += static_cast<qsizetype>(sz);
        if (sz & 1u) {
            ++pos;
        }
    }
    if (format != 3 || channels != 1 || bits != 32) {
        err = QStringLiteral("expected mono float32 WAV (got fmt=%1 ch=%2 bits=%3)")
            .arg(format).arg(channels).arg(bits);
        return false;
    }
    if (dataBytes % 4 != 0) {
        err = QStringLiteral("data chunk length %1 is not a multiple of 4 (float32 sample size)").arg(dataBytes);
        return false;
    }
    samples.resize(static_cast<size_t>(dataBytes / 4));
    std::memcpy(samples.data(), data, static_cast<size_t>(dataBytes));
    return true;
}

} // namespace AetherSDR::Ax25ReplayWav
