#include "core/DvkWavTransfer.h"
#include "core/FirmwareUploader.h"
#include "core/ProfileTransfer.h"
#include "core/WaveformInstaller.h"
#include "models/DvkModel.h"

#include <QPointer>
#include <QTcpSocket>
#include <QtTest>
#include <memory>

// Only inert socket objects are used; no test connects, binds, or listens.
namespace AetherSDR {
class WaveformInstallerTestAccess {
public:
    static void acknowledge(WaveformInstaller& transfer)
    {
        transfer.m_fileData = QByteArray(4, 'x');
        transfer.m_activeGeneration = transfer.m_uploadState.begin(4);
        transfer.m_socket = new QTcpSocket(&transfer);
        transfer.m_uploadState.recordQueued(transfer.m_activeGeneration, 4, 4);
        transfer.onBytesWritten(transfer.m_activeGeneration, transfer.m_socket, 4);
    }
};
class FirmwareUploaderTestAccess {
public:
    static void acknowledge(FirmwareUploader& transfer)
    {
        transfer.beginOperation(QByteArray(4, 'x'), QStringLiteral("test.ssdr"));
        const auto generation = transfer.m_generation;
        transfer.startSending(generation, [](const char*, qint64 size) { return size; });
        transfer.acknowledgeBytes(generation, 4);
    }
};
class ProfileTransferTestAccess {
public:
    static void progress(ProfileTransfer& transfer)
    {
        transfer.begin(ProfileTransfer::Operation::ImportDatabase, ProfileTransfer::Phase::UploadImport);
        transfer.connectUploadSocket(42607);
    }
};
class DvkWavTransferTestAccess {
public:
    static void enable(DvkWavTransfer& transfer, DvkModel& dvk)
    {
        transfer.m_dvk = &dvk;
        transfer.m_transferRequester = [](const FileTransferRequest&, FileTransferCompletion) {};
    }
    static void replace(DvkWavTransfer& transfer)
    {
        transfer.begin(DvkWavTransfer::Upload, 2);
        transfer.m_uploadData = QByteArray("replacement");
        transfer.m_client = new QTcpSocket(&transfer);
    }
    static QByteArray payload(const DvkWavTransfer& transfer) { return transfer.m_uploadData; }
    static QTcpSocket* socket(const DvkWavTransfer& transfer) { return transfer.m_client; }
};
} // namespace AetherSDR

using namespace AetherSDR;
class TransferLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void waveformProgressCanDestroyOrCancel()
    {
        auto owned = std::make_unique<WaveformInstaller>(nullptr);
        QPointer<WaveformInstaller> transfer(owned.get());
        connect(transfer, &WaveformInstaller::progressChanged, this, [&] { owned.reset(); });
        WaveformInstallerTestAccess::acknowledge(*transfer);
        QVERIFY(transfer.isNull());

        WaveformInstaller cancelled(nullptr);
        QSignalSpy done(&cancelled, &WaveformInstaller::finished);
        connect(&cancelled, &WaveformInstaller::progressChanged, &cancelled, &WaveformInstaller::cancel);
        WaveformInstallerTestAccess::acknowledge(cancelled);
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.at(0).at(0).toBool(), false);
        QVERIFY(!cancelled.isInstalling());
    }
    void firmwareProgressCanDestroy()
    {
        auto owned = std::make_unique<FirmwareUploader>(nullptr);
        QPointer<FirmwareUploader> transfer(owned.get());
        connect(transfer, &FirmwareUploader::progressChanged, this, [&] { owned.reset(); });
        FirmwareUploaderTestAccess::acknowledge(*transfer);
        QVERIFY(transfer.isNull());
    }
    void profileProgressCanDestroy()
    {
        auto owned = std::make_unique<ProfileTransfer>(nullptr);
        QPointer<ProfileTransfer> transfer(owned.get());
        connect(transfer, &ProfileTransfer::progress, this, [&] { owned.reset(); });
        ProfileTransferTestAccess::progress(*transfer);
        QVERIFY(transfer.isNull());
    }
    void dvkAdmissionCanDestroyAtStart()
    {
        DvkModel dvk(nullptr);
        auto owned = std::make_unique<DvkWavTransfer>(nullptr);
        QPointer<DvkWavTransfer> transfer(owned.get());
        DvkWavTransferTestAccess::enable(*transfer, dvk);
        connect(&dvk, &DvkModel::admissionChanged, this, [&] {
            if (transfer && !dvk.canStartOperation()) { owned.reset(); }
        });
        transfer->download(1, QStringLiteral("unused.wav"));
        QVERIFY(transfer.isNull());
    }
    void dvkAdmissionCanStartReplacementAfterCleanup()
    {
        DvkModel dvk(nullptr);
        DvkWavTransfer transfer(nullptr);
        DvkWavTransferTestAccess::enable(transfer, dvk);
        transfer.download(1, QStringLiteral("unused.wav"));
        bool replaced = false;
        const auto connection = connect(&dvk, &DvkModel::admissionChanged, this, [&] {
            if (dvk.canStartOperation() && !replaced) {
                replaced = true;
                DvkWavTransferTestAccess::replace(transfer);
            }
        });
        transfer.cancel();
        QVERIFY(replaced);
        QVERIFY(transfer.isTransferring());
        QCOMPARE(DvkWavTransferTestAccess::payload(transfer), QByteArray("replacement"));
        QVERIFY(DvkWavTransferTestAccess::socket(transfer));
        disconnect(connection);
        transfer.cancel();
    }
};
QTEST_GUILESS_MAIN(TransferLifecycleTest)
#include "transfer_lifecycle_test.moc"
