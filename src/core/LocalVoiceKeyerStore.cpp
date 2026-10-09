#include "core/LocalVoiceKeyerStore.h"

#include "core/VoiceKeyerWavDecoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {

namespace {

constexpr int kHeaderBytes = 44;
constexpr int kFrameBytes  = LocalVoiceKeyerStore::kChannels * sizeof(qint16);

bool validSlot(int id)
{
    return id >= 1 && id <= LocalVoiceKeyerStore::kSlotCount;
}

// Canonical 44-byte PCM header, the same layout QsoRecorder writes.
QByteArray wavHeader(quint32 dataBytes)
{
    constexpr quint16 channels = LocalVoiceKeyerStore::kChannels;
    constexpr quint32 rate     = LocalVoiceKeyerStore::kSampleRate;
    QByteArray header(kHeaderBytes, '\0');
    char* p = header.data();
    std::memcpy(p, "RIFF", 4);
    qToLittleEndian<quint32>(dataBytes + kHeaderBytes - 8, p + 4);
    std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "fmt ", 4);
    qToLittleEndian<quint32>(16, p + 16);                     // fmt chunk size
    qToLittleEndian<quint16>(1, p + 20);                      // PCM
    qToLittleEndian<quint16>(channels, p + 22);
    qToLittleEndian<quint32>(rate, p + 24);
    qToLittleEndian<quint32>(rate * kFrameBytes, p + 28);     // byte rate
    qToLittleEndian<quint16>(kFrameBytes, p + 32);            // block align
    qToLittleEndian<quint16>(16, p + 34);                     // bits per sample
    std::memcpy(p + 36, "data", 4);
    qToLittleEndian<quint32>(dataBytes, p + 40);
    return header;
}

} // namespace

LocalVoiceKeyerStore::LocalVoiceKeyerStore(QString dir)
    : m_dir(std::move(dir))
{
}

QString LocalVoiceKeyerStore::slotPath(int id) const
{
    return QDir(m_dir).filePath(
        QStringLiteral("slot-%1.wav").arg(id, 2, 10, QLatin1Char('0')));
}

int LocalVoiceKeyerStore::durationMs(int id) const
{
    if (!validSlot(id))
        return 0;
    const QString path = slotPath(id);
    if (!QFileInfo::exists(path))
        return 0;
    int ms = 0;
    QString error;
    return VoiceKeyerWavDecoder::probeDurationMs(path, ms, error) ? ms : 0;
}

bool LocalVoiceKeyerStore::writeSlot(int id, const QByteArray& int16Stereo, QString& error)
{
    if (!validSlot(id)) {
        error = QStringLiteral("There is no slot %1.").arg(id);
        return false;
    }
    const qint64 usable = int16Stereo.size() - int16Stereo.size() % kFrameBytes;
    if (usable <= 0) {
        error = QStringLiteral("There is no audio to save.");
        return false;
    }
    const qint64 maxBytes = qint64(kMaxDurationMs) * kSampleRate / 1000 * kFrameBytes;
    if (usable > maxBytes) {
        error = QStringLiteral("A recording can be at most %1 seconds.").arg(kMaxDurationMs / 1000);
        return false;
    }
    if (!QDir().mkpath(m_dir)) {
        error = QStringLiteral("Cannot create the recordings folder %1.").arg(m_dir);
        return false;
    }
    QSaveFile file(slotPath(id));
    if (!file.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot save the recording: %1").arg(file.errorString());
        return false;
    }
    file.write(wavHeader(static_cast<quint32>(usable)));
    file.write(int16Stereo.constData(), usable);
    if (!file.commit()) {
        error = QStringLiteral("Cannot save the recording: %1").arg(file.errorString());
        return false;
    }
    return true;
}

bool LocalVoiceKeyerStore::importSlot(int id, const QString& sourcePath, QString& error)
{
    if (!validSlot(id)) {
        error = QStringLiteral("There is no slot %1.").arg(id);
        return false;
    }
    QByteArray mono;
    int rate = 0;
    if (!VoiceKeyerWavDecoder::decodeToMonoFloat(sourcePath, mono, rate, error))
        return false;
    const qint64 frames = mono.size() / qint64(sizeof(float));
    if (frames * 1000 / rate > kMaxDurationMs) {
        error = QStringLiteral("The file is longer than %1 seconds.").arg(kMaxDurationMs / 1000);
        return false;
    }
    const QByteArray stereo = VoiceKeyerWavDecoder::toTxStereo24k(mono, rate);
    if (stereo.isEmpty()) {
        error = QStringLiteral("Cannot convert the file's audio.");
        return false;
    }
    const qsizetype samples = stereo.size() / qsizetype(sizeof(float));
    QByteArray pcm(samples * qsizetype(sizeof(qint16)), Qt::Uninitialized);
    const auto* in = reinterpret_cast<const float*>(stereo.constData());
    auto* out = reinterpret_cast<qint16*>(pcm.data());
    for (qsizetype i = 0; i < samples; ++i)
        out[i] = static_cast<qint16>(std::lround(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f));
    return writeSlot(id, pcm, error);
}

bool LocalVoiceKeyerStore::exportSlot(int id, const QString& destPath, QString& error) const
{
    QFile in(slotPath(id));
    if (!validSlot(id) || !in.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Slot %1 has no recording.").arg(id);
        return false;
    }
    const QByteArray bytes = in.readAll();
    QSaveFile out(destPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        error = QStringLiteral("Cannot write %1: %2").arg(destPath, out.errorString());
        return false;
    }
    return true;
}

bool LocalVoiceKeyerStore::removeSlot(int id, QString& error)
{
    const QString path = slotPath(id);
    if (!validSlot(id) || !QFileInfo::exists(path))
        return true;
    if (!QFile::remove(path)) {
        error = QStringLiteral("Cannot delete %1.").arg(path);
        return false;
    }
    return true;
}

} // namespace AetherSDR
