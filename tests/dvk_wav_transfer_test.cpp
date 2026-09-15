#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>

#include "core/DvkWavTransfer.h"

#include <iostream>

namespace AetherSDR {

class DvkWavTransferTestAccess {
public:
    static QSaveFile* openDownload(DvkWavTransfer& transfer, const QString& path)
    {
        transfer.m_filePath = path;
        return transfer.openDownloadFile() ? transfer.m_file : nullptr;
    }

    static void prepareDownload(DvkWavTransfer& transfer, QSaveFile* file, int slotId = 7)
    {
        transfer.m_file = file;
        transfer.m_slotId = slotId;
        transfer.m_direction = DvkWavTransfer::Download;
        transfer.m_transferring = true;
        transfer.m_cancelled = false;
        transfer.m_finished = false;
        transfer.m_bytesReceived = 0;
    }

    static void receive(DvkWavTransfer& transfer, const QByteArray& bytes)
    {
        transfer.receiveDownloadBytes(bytes);
    }

    static void complete(DvkWavTransfer& transfer)
    {
        transfer.onDownloadFinished();
    }

    static void error(DvkWavTransfer& transfer)
    {
        transfer.onDownloadError();
    }

    static void timeout(DvkWavTransfer& transfer)
    {
        QMetaObject::invokeMethod(transfer.m_timeout, "timeout", Qt::DirectConnection);
    }

    static qint64 maxFileSize()
    {
        return DvkWavTransfer::MAX_FILE_SIZE;
    }
};

} // namespace AetherSDR

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

class FailingSaveFile final : public QSaveFile {
public:
    enum class Failure { ShortWrite, WriteError };

    FailingSaveFile(const QString& path, Failure failure, QObject* parent)
        : QSaveFile(path, parent), m_failure(failure)
    {
    }

protected:
    qint64 writeData(const char*, qint64 length) override
    {
        return m_failure == Failure::ShortWrite ? length - 1 : -1;
    }

private:
    Failure m_failure;
};

QSaveFile* openSaveFile(const QString& path, AetherSDR::DvkWavTransfer* transfer)
{
    return AetherSDR::DvkWavTransferTestAccess::openDownload(*transfer, path);
}

QByteArray baselineWav()
{
    QByteArray wav(52, '\0');
    wav.replace(0, 4, "RIFF");
    qToLittleEndian<quint32>(44, wav.data() + 4);
    wav.replace(8, 8, "WAVEfmt ");
    qToLittleEndian<quint32>(16, wav.data() + 16);
    qToLittleEndian<quint16>(3, wav.data() + 20);
    qToLittleEndian<quint16>(2, wav.data() + 22);
    qToLittleEndian<quint32>(48000, wav.data() + 24);
    qToLittleEndian<quint32>(384000, wav.data() + 28);
    qToLittleEndian<quint16>(8, wav.data() + 32);
    qToLittleEndian<quint16>(32, wav.data() + 34);
    wav.replace(36, 4, "data");
    qToLittleEndian<quint32>(8, wav.data() + 40);
    return wav;
}

bool writeBaseline(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray wav = baselineWav();
    return file.write(wav) == wav.size();
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    QTemporaryDir directory;
    ok &= expect(directory.isValid(), "temporary directory is available");
    if (!directory.isValid()) {
        return 1;
    }

    const QString path = directory.filePath("slot.wav");
    const QByteArray baseline = baselineWav();

    // Every failed terminal route must discard the staged file and leave the
    // last successful export in place. No listener or radio socket is used.
    for (const QString& terminal : {QStringLiteral("cancel"), QStringLiteral("empty"),
                                    QStringLiteral("error"), QStringLiteral("timeout")}) {
        ok &= expect(writeBaseline(path), "baseline output can be created");
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QSaveFile* file = openSaveFile(path, &transfer);
        ok &= expect(file != nullptr, "staged output can be opened");
        if (!file) {
            continue;
        }
        ok &= expect(!file->directWriteFallback(), "production staging refuses direct-write fallback");
        ok &= expect(readFile(path) == baseline, "production open preserves the existing WAV");
        AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file);
        if (terminal != QStringLiteral("empty") && terminal != QStringLiteral("timeout")) {
            AetherSDR::DvkWavTransferTestAccess::receive(transfer, QByteArray("new WAV"));
        }
        QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
        if (terminal == QStringLiteral("cancel")) {
            transfer.cancel();
        } else if (terminal == QStringLiteral("empty")) {
            AetherSDR::DvkWavTransferTestAccess::complete(transfer);
        } else if (terminal == QStringLiteral("error")) {
            AetherSDR::DvkWavTransferTestAccess::error(transfer);
        } else {
            AetherSDR::DvkWavTransferTestAccess::timeout(transfer);
        }
        ok &= expect(finished.count() == 1, "a failed download emits one terminal signal");
        ok &= expect(finished.count() == 1 && !finished.at(0).at(0).toBool(), "failed download reports failure");
        ok &= expect(readFile(path) == baseline, "failed download preserves the baseline output");
    }

    {
        ok &= expect(writeBaseline(path), "baseline output can be recreated");
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QSaveFile* file = openSaveFile(path, &transfer);
        ok &= expect(file != nullptr, "staged output can be opened for success");
        if (file) {
            AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file, 3);
            QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
            QByteArray visibleOnFinished;
            QObject::connect(&transfer, &AetherSDR::DvkWavTransfer::finished,
                             [&path, &visibleOnFinished](bool, const QString&) {
                                 visibleOnFinished = readFile(path);
                             });
            AetherSDR::DvkWavTransferTestAccess::receive(transfer, QByteArray("replacement WAV"));
            AetherSDR::DvkWavTransferTestAccess::complete(transfer);
            ok &= expect(finished.count() == 1 && finished.at(0).at(0).toBool(),
                         "complete staged download reports success once");
            ok &= expect(readFile(path) == QByteArray("replacement WAV"),
                         "successful completion commits the replacement before finished");
            ok &= expect(visibleOnFinished == QByteArray("replacement WAV"),
                         "finished observers see the committed replacement");
            AetherSDR::DvkWavTransferTestAccess::complete(transfer);
            ok &= expect(finished.count() == 1, "duplicate terminal signals remain idempotent");
        }
    }

    for (const FailingSaveFile::Failure failure : {FailingSaveFile::Failure::ShortWrite,
                                                   FailingSaveFile::Failure::WriteError}) {
        ok &= expect(writeBaseline(path), "baseline output can be recreated for write failure");
        AetherSDR::DvkWavTransfer transfer(nullptr);
        auto* file = new FailingSaveFile(path, failure, &transfer);
        file->setDirectWriteFallback(false);
        ok &= expect(file->open(QIODevice::WriteOnly), "failing staged file can be opened");
        if (!file->isOpen()) {
            continue;
        }
        AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file);
        QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
        AetherSDR::DvkWavTransferTestAccess::receive(transfer, QByteArray("new WAV"));
        ok &= expect(finished.count() == 1 && !finished.at(0).at(0).toBool(),
                     "short or failed write is a terminal failure");
        ok &= expect(readFile(path) == baseline, "write failure preserves the baseline output");
    }

    {
        ok &= expect(writeBaseline(path), "baseline output can be recreated for commit failure");
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QSaveFile* file = openSaveFile(path, &transfer);
        ok &= expect(file != nullptr, "staged output can be opened for commit failure");
        if (file) {
            AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file);
            AetherSDR::DvkWavTransferTestAccess::receive(transfer, QByteArray("new WAV"));
            file->cancelWriting();
            QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
            AetherSDR::DvkWavTransferTestAccess::complete(transfer);
            ok &= expect(finished.count() == 1 && !finished.at(0).at(0).toBool(),
                         "failed QSaveFile commit is a terminal failure");
            ok &= expect(readFile(path) == baseline, "failed commit preserves the baseline output");
        }
    }

    {
        ok &= expect(writeBaseline(path), "baseline output can be recreated for oversize stream");
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QSaveFile* file = openSaveFile(path, &transfer);
        ok &= expect(file != nullptr, "staged output can be opened for oversize stream");
        if (file) {
            AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file);
            QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
            AetherSDR::DvkWavTransferTestAccess::receive(transfer,
                QByteArray(AetherSDR::DvkWavTransferTestAccess::maxFileSize() + 1, 'x'));
            ok &= expect(finished.count() == 1 && !finished.at(0).at(0).toBool(),
                         "oversized stream fails instead of reporting truncation success");
            ok &= expect(readFile(path) == baseline, "oversized stream preserves the baseline output");
        }
    }

    ok &= expect(writeBaseline(path), "baseline created for active destruction");
    {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        QSaveFile* file = openSaveFile(path, &transfer);
        ok &= expect(file != nullptr, "production staging opens for active destruction");
        if (file) {
            AetherSDR::DvkWavTransferTestAccess::prepareDownload(transfer, file);
            AetherSDR::DvkWavTransferTestAccess::receive(transfer, QByteArray("partial WAV"));
            ok &= expect(readFile(path) == baseline, "active download keeps original visible");
        }
    }
    ok &= expect(readFile(path) == baseline, "destroying active download preserves existing WAV");

    return ok ? 0 : 1;
}
