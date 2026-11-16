// Production encoder and admission; no socket is initialized. The queued
// writer is pinned in session_control_test.
#include "RadioSetupControlFixture.h"
#include "core/FirmwareUploader.h"
#include "core/WaveformInstaller.h"
#include "core/ZipArchive.h"
#include "core/backends/FileTransferOperation.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace AetherSDR;
namespace AetherSDR {
class WaveformInstallerTestAccess {
public:
    static void finish(WaveformInstaller& uploader, bool success)
    { uploader.finishOperation(uploader.m_activeGeneration, success, {}); }
};
class FirmwareUploaderTestAccess {
public:
    // A radio-settled failure after bytes were sent, which releases the
    // firmware retry barrier.
    static void settleAfterBytes(FirmwareUploader& uploader, FileTransferHandle transfer)
    {
        uploader.beginOperation(QByteArray(4, 'x'), QStringLiteral("test.ssdr"));
        uploader.m_transfer = std::move(transfer);
        uploader.m_bytesQueued = 4;
        uploader.m_outcomeSettledByRadio = true;
        uploader.finishOperation(uploader.m_generation, FirmwareUploader::Outcome::Failed, {});
    }
};
}

class Owner : public QObject {
public:
    int destroyedLinks() const { return receivers(SIGNAL(destroyed(QObject*))); }
};

struct Fixture {
    RadioModel model;
    RadioSetupTestBackend* backend{installRadioSetupBackend(model)};
    Owner owner;
    QStringList commands;
    QList<std::function<void(int, const QString&)>> replies;
    Fixture()
    {
        RadioSetupControlTestAccess::peer(model, QHostAddress(QStringLiteral("192.0.2.1")));
        backend->setTransferCommandSink(RadioSetupControlTestAccess::productionTransferSink(model,
            [this](const QString& command, const FileTransferGuard& guard,
                   const std::function<void(int, const QString&)>& reply) {
                if (!guard.current()) { return false; }
                commands << command;
                replies << reply;
                return true;
            }));
    }
    FileTransferHandle begin(FileTransferPurpose purpose)
    { return model.beginFileTransfer(purpose, &owner, model.controlSession(), 30000); }
    void reply(int code = 0, const QString& body = {})
    { const auto callback = replies.last(); callback(code, body); }
};

class FileTransferControlTest final : public QObject {
    Q_OBJECT
private slots:
    void nativeThenContainerKeepsOneExactReservation()
    {
        Fixture f;
        f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
        const FileTransferRequest native{FileTransferPurpose::LegacyWaveform, 21, "first.ssdr_waveform", 0};
        const FileTransferRequest container{FileTransferPurpose::DockerWaveform, 22, "carrier.tar.gz", 0};
        const auto operation = f.model.beginWaveformInstallTransfer(native, container, &f.owner, f.model.controlSession(), 30000);
        QVERIFY(operation);
        QVERIFY(!f.begin(FileTransferPurpose::DockerWaveform));
        QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
        FileTransferRequest wrong = native; ++wrong.size;
        QCOMPARE(f.model.requestFileTransfer(operation, wrong, {}), ReceiveDispatch::Unsupported);
        QCOMPARE(f.model.requestFileTransfer(operation, container, {}), ReceiveDispatch::Unsupported);
        QVERIFY(f.commands.isEmpty());
        f.model.requestFileTransfer(operation, native, {});
        QCOMPARE(f.commands, QStringList{"file filename first.ssdr_waveform"});
        QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
        f.reply(); // Filename acceptance is not upload-port acceptance.
        QCOMPARE(f.commands.last(), QStringLiteral("file upload 21 new_waveform"));
        QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
        f.reply(0, "42607");
        QVERIFY(f.model.advanceWaveformInstallTransfer(operation));
        QVERIFY(operation->reservedFor(FileTransferPurpose::DockerWaveform, f.model.controlSession()));
        QVERIFY(!f.begin(FileTransferPurpose::LegacyWaveform));
        QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
        QCOMPARE(f.model.requestFileTransfer(operation, native, {}), ReceiveDispatch::Unsupported);
        wrong = container; wrong.fileName = "other.tar.gz";
        QCOMPARE(f.model.requestFileTransfer(operation, wrong, {}), ReceiveDispatch::Unsupported);
        QCOMPARE(f.commands.size(), 2);
        f.model.requestFileTransfer(operation, container, {});
        QCOMPARE(f.commands.last(), QStringLiteral("file upload 22 waveform_docker_image carrier.tar.gz"));
        f.reply(0, "42607");
        QCOMPARE(f.model.requestFileTransfer(operation, container, {}), ReceiveDispatch::Unsupported);
        QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
        QVERIFY(!f.begin(FileTransferPurpose::LegacyWaveform));
        f.model.finishFileTransfer(operation);
        QVERIFY(f.begin(FileTransferPurpose::LegacyWaveform));
    }

    void waveformSequenceCannotSurviveRefusalOrSessionChange()
    {
        for (const bool rejected : {false, true}) {
            Fixture f;
            f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
            f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
            const FileTransferRequest native{FileTransferPurpose::LegacyWaveform, 21, "first.ssdr_waveform", 0};
            const FileTransferRequest container{FileTransferPurpose::DockerWaveform, 22, "carrier.tar.gz", 0};
            QVERIFY(!f.model.beginWaveformInstallTransfer(container, native, &f.owner, f.model.controlSession(), 30000));
            const auto operation = f.model.beginWaveformInstallTransfer(native, container, &f.owner, f.model.controlSession(), 30000);
            QVERIFY(operation);
            f.model.requestFileTransfer(operation, native, {}); f.reply(); f.reply(rejected ? 1 : 0, "42607");
            if (!rejected) { RadioSetupControlTestAccess::replaceConnectionGeneration(f.model); }
            QVERIFY(!f.model.advanceWaveformInstallTransfer(operation));
            QCOMPARE(f.model.requestFileTransfer(operation, container, {}), ReceiveDispatch::Unsupported);
            QCOMPARE(f.commands.size(), 2);
        }
    }

    void reservedNativeUploadUsesTheEnclosingReservation()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("First.ssdr_waveform"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray archive = writeDeflatedZip({
            {"First/First.cfg", "[header]\nName: First\nVersion: 0.1.0\nExecutable: start.sh\n\n[setup]\n\n[end]\n"},
            {"First/start.sh", "#!/bin/sh\nexit 0\n"}});
        QCOMPARE(file.write(archive), archive.size()); file.close();
        Fixture f;
        f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
        const auto operation = f.model.beginWaveformInstallTransfer(
            {FileTransferPurpose::LegacyWaveform, archive.size(), "First.ssdr_waveform", 0},
            {FileTransferPurpose::DockerWaveform, 22, "carrier.tar.gz", 0}, &f.owner, f.model.controlSession(), 30000);
        QVERIFY(operation);
        WaveformInstaller uploader(&f.model);
        uploader.installReservedLegacyWaveform(path, f.model.controlSession(), operation);
        QTRY_COMPARE(f.commands.size(), 1);
        QCOMPARE(f.commands.first(), QStringLiteral("file filename First.ssdr_waveform"));
        // Do not answer the port request: no socket opens in this test.
        uploader.cancel();
        QVERIFY(f.model.fileTransferCurrent(operation));
        QVERIFY(!f.begin(FileTransferPurpose::LegacyWaveform));
        f.model.finishFileTransfer(operation);
    }

    void reservedWaveformUploadKeepsTransactionOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("carrier.tar.gz"));
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write(QByteArray::fromHex("1f8b0000")), qint64(4));
        image.close();
        for (const int outcome : {0, 1, 2, 3}) {
            Fixture f;
            f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
            f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
            const FileTransferHandle reservation = f.begin(FileTransferPurpose::DockerWaveform);
            QVERIFY(reservation);
            auto upload = std::make_unique<WaveformInstaller>(&f.model);
            upload->installReservedDockerWaveform(path, f.model.controlSession(), reservation);
            // Inspect the real typed command admission, but do not answer it:
            // no TCP socket can open. Inject only the final uploader event.
            QTRY_COMPARE(f.commands.size(), 1);
            if (outcome < 2) { WaveformInstallerTestAccess::finish(*upload, outcome == 0); }
            else if (outcome == 2) { upload->cancel(); }
            else { upload.reset(); }
            QVERIFY(f.model.fileTransferCurrent(reservation));
            QVERIFY(!f.begin(FileTransferPurpose::LegacyWaveform));
            f.model.finishFileTransfer(reservation);
            QVERIFY(f.begin(FileTransferPurpose::LegacyWaveform));
        }
    }

    void invalidWaveformReservationNeverReacquires()
    {
        Fixture f;
        f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
        WaveformInstaller uploader(&f.model);
        QSignalSpy done(&uploader, &WaveformInstaller::finished);
        uploader.installReservedDockerWaveform(QStringLiteral("unused.tar.gz"), f.model.controlSession(), {});
        QCOMPARE(done.size(), 1);
        QVERIFY(!done.last().first().toBool());
        const FileTransferHandle wrongPurpose = f.begin(FileTransferPurpose::LegacyWaveform);
        QVERIFY(wrongPurpose);
        uploader.installReservedDockerWaveform(QStringLiteral("unused.tar.gz"), f.model.controlSession(), wrongPurpose);
        QCOMPARE(done.size(), 2);
        QVERIFY(f.model.fileTransferCurrent(wrongPurpose));
        f.model.finishFileTransfer(wrongPurpose);
        const FileTransferHandle expired = f.begin(FileTransferPurpose::DockerWaveform);
        QVERIFY(expired);
        expired->cancel();
        uploader.installReservedDockerWaveform(QStringLiteral("unused.tar.gz"), f.model.controlSession(), expired);
        QCOMPARE(done.size(), 3);
        QVERIFY(f.commands.isEmpty());
        QVERIFY(f.begin(FileTransferPurpose::LegacyWaveform));
    }

    void revokedWaveformReservationDuringPreparationNeverReacquires()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("carrier.tar.gz"));
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write(QByteArray::fromHex("1f8b0000")), qint64(4));
        image.close();
        Fixture f;
        f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        f.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
        const FileTransferHandle reservation = f.begin(FileTransferPurpose::DockerWaveform);
        QVERIFY(reservation);
        WaveformInstaller uploader(&f.model);
        QSignalSpy done(&uploader, &WaveformInstaller::finished);
        uploader.installReservedDockerWaveform(path, f.model.controlSession(), reservation);
        reservation->cancel();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done.first().first().toBool());
        QVERIFY(f.commands.isEmpty());
        QVERIFY(f.begin(FileTransferPurpose::LegacyWaveform));
    }

    void encodeEveryPurpose()
    {
        using P = FileTransferPurpose;
        const QList<FileTransferRequest> requests{
            {P::Firmware, 21, QStringLiteral("radio.ssdr"), 0},
            {P::LegacyWaveform, 21, QStringLiteral("FreeDV.ssdr_waveform"), 0},
            {P::DockerWaveform, 21, QStringLiteral("waveform.tar.gz"), 0},
            {P::ProfileMetadata, 21, {}, 0}, {P::ProfileImport, 21, {}, 0},
            {P::ProfileExport, 0, {}, 0}, {P::DvkUpload, 21, {}, 3}, {P::DvkDownload, 0, {}, 3}};
        const QList<QStringList> expected{
            {"file filename radio.ssdr", "file upload 21 update"},
            {"file filename FreeDV.ssdr_waveform", "file upload 21 new_waveform"},
            {"file upload 21 waveform_docker_image waveform.tar.gz"},
            {"file upload 21 db_meta_subset"}, {"file upload 21 db_import"},
            {"file download db_package"}, {"dvk upload id=3", "file upload 21 dvk_recording"},
            {"dvk download id=3"}};
        for (int i = 0; i < requests.size(); ++i) {
            Fixture f;
            if (requests[i].purpose == P::DockerWaveform) {
                f.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
            }
            // Encoder fixtures deliberately bypass model runtime admission; that is tested separately.
            int completions = 0;
            const FileTransferGuard guard(30000);
            const auto result = f.backend->requestFileTransfer(requests[i], [&](const FileTransferReply& reply) {
                ++completions;
                QCOMPARE(reply.result.kind, RadioControlReply::Kind::Accepted);
                QCOMPARE(reply.port, quint16(4995));
            }, guard, [] { return true; });
            QCOMPARE(result, ReceiveDispatch::Dispatched);
            QCOMPARE(f.commands.size(), 1);
            if (expected[i].size() == 2) { f.reply(); QCOMPARE(completions, 0); }
            QCOMPARE(f.commands, expected[i]);
            f.reply(0, QStringLiteral("4995"));
            f.reply(0, QStringLiteral("42607"));
            QCOMPARE(completions, 1);
        }
    }

    void portFormatsAndLegacyFallbacks()
    {
        using P = FileTransferPurpose;
        const QList<QPair<QString, quint16>> cases{{"4995",4995}, {"port=42607",42607},
            {"endpoint=192.0.2.1:4995",4995}, {"tcp_port: 5000",5000},
            {"no usable port",42607}, {"65536",42607}, {"0",42607}};
        for (const auto& test : cases) {
            Fixture f;
            quint16 port = 0;
            f.backend->requestFileTransfer({P::ProfileExport, 0, {}, 0},
                [&](const FileTransferReply& reply) { port = reply.port; }, FileTransferGuard(30000), {});
            f.reply(0, test.first);
            QCOMPARE(port, test.second);
        }
        for (P purpose : {P::Firmware, P::LegacyWaveform, P::ProfileMetadata, P::DvkUpload, P::DvkDownload}) {
            Fixture f;
            FileTransferRequest request{purpose, purpose == P::DvkDownload ? 0 : 10,
                purpose == P::Firmware || purpose == P::LegacyWaveform ? QStringLiteral("image.bin") : QString(),
                purpose == P::DvkUpload || purpose == P::DvkDownload ? 1 : 0};
            FileTransferReply result{{RadioControlReply::Kind::Cancelled,-1,{}},0};
            f.backend->requestFileTransfer(request, [&](const auto& reply) { result = reply; }, FileTransferGuard(30000), {});
            if (purpose == P::Firmware || purpose == P::LegacyWaveform || purpose == P::DvkUpload) { f.reply(); }
            f.reply();
            QCOMPARE(result.port, quint16(purpose == P::DvkDownload ? 0
                : purpose == P::Firmware || purpose == P::ProfileMetadata ? 4995 : 42607));
            QCOMPARE(result.result.kind, purpose == P::DvkDownload ? RadioControlReply::Kind::Failed
                                                                  : RadioControlReply::Kind::Accepted);
        }
    }

    void invalidRequestsNeverDispatch()
    {
        using P = FileTransferPurpose;
        Fixture f;
        for (const FileTransferRequest& request : QList<FileTransferRequest>{
            {P::Firmware,0,"image.ssdr",0}, {P::Firmware,1,"bad\nfile",0},
            {P::Firmware,1,"../bad",0}, {P::LegacyWaveform,1,"bad file",0},
            {P::DockerWaveform,1,"bad|file",0}, {P::ProfileImport,1,"unexpected",0},
            {P::ProfileExport,1,{},0}, {P::DvkUpload,5'000'001,{},1},
            {P::DvkDownload,0,{},0}, {P::DvkDownload,0,{},13}}) {
            QCOMPARE(f.backend->requestFileTransfer(request, {}, FileTransferGuard(30000), {}),
                     ReceiveDispatch::Unsupported);
        }
        QVERIFY(f.commands.isEmpty());
    }

    void reservationAndOriginalSession()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::ProfileMetadata);
        QVERIFY(operation);
        QVERIFY(!f.begin(FileTransferPurpose::LegacyWaveform));
        f.model.finishFileTransfer(operation);
        auto replacement = f.begin(FileTransferPurpose::LegacyWaveform);
        QVERIFY(replacement);
        f.model.finishFileTransfer(operation, 3000);
        QVERIFY(f.model.fileTransferCurrent(replacement));
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        QVERIFY(!f.model.fileTransferCurrent(replacement));
        int calls = 0;
        f.model.requestFileTransfer(replacement, {FileTransferPurpose::LegacyWaveform,1,"image",0},
            [&](const auto&) { ++calls; });
        QCOMPARE(calls,1);
        QVERIFY(f.commands.isEmpty());
        QVERIFY(f.begin(FileTransferPurpose::ProfileImport));
    }

    void settleWindowAndUnknownPeer()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::ProfileMetadata);
        QVERIFY(operation);
        f.model.finishFileTransfer(operation, 3000);
        QVERIFY(!f.begin(FileTransferPurpose::ProfileImport));
        RadioSetupControlTestAccess::replaceConnectionGeneration(f.model);
        QVERIFY(f.begin(FileTransferPurpose::ProfileImport));
        Fixture unknown;
        RadioSetupControlTestAccess::peer(unknown.model, {});
        QVERIFY(!unknown.begin(FileTransferPurpose::ProfileImport));
        QVERIFY(!transferPeerMatches(QHostAddress::LocalHost, {}));
        QVERIFY(transferPeerMatches(QHostAddress("::ffff:192.0.2.1"), QHostAddress("192.0.2.1")));
        QVERIFY(!transferPeerMatches(QHostAddress("192.0.2.2"), QHostAddress("192.0.2.1")));
    }

    // A firmware attempt that sent bytes holds the file server's settle window
    // like every other transfer owner.
    void firmwareBytesHoldSettleWindow()
    {
        Fixture f;
        FirmwareUploader uploader(&f.model);
        auto operation = f.begin(FileTransferPurpose::Firmware);
        QVERIFY(operation);
        FirmwareUploaderTestAccess::settleAfterBytes(uploader, operation);
        QVERIFY(!f.model.firmwareRetryBlocked());
        QVERIFY(!f.begin(FileTransferPurpose::ProfileImport));
    }

    void cancelledPreludeDoesNotSendNextStep()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::LegacyWaveform);
        QVERIFY(operation);
        int completions = 0;
        f.model.requestFileTransfer(operation, {FileTransferPurpose::LegacyWaveform,10,"image",0},
            [&](const FileTransferReply& result) {
                ++completions; QCOMPARE(result.result.kind, RadioControlReply::Kind::Cancelled);
            });
        QCOMPARE(f.commands.size(),1);
        operation->cancel();
        f.reply(); f.reply();
        QCOMPARE(f.commands.size(),1);
        QCOMPARE(completions,1);
    }

    void firmwareBarrierSurvivesUnansweredRequest()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::Firmware);
        QVERIFY(operation);
        f.model.requestFileTransfer(operation, {FileTransferPurpose::Firmware,10,"image.ssdr",0}, {});
        QVERIFY(!f.model.firmwareRetryBlocked());
        f.reply();
        QCOMPARE(f.commands.size(),2);
        QVERIFY(f.model.firmwareRetryBlocked());
        f.model.finishFileTransfer(operation);
        QVERIFY(f.model.firmwareRetryBlocked());
        QVERIFY(!f.begin(FileTransferPurpose::Firmware));
        f.reply(1); // a definitive refusal settles the pending update
        QVERIFY(!f.model.firmwareRetryBlocked());
        QVERIFY(f.begin(FileTransferPurpose::Firmware));
    }

    void metadataContinuationIsSingleUse()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::ProfileMetadata);
        QVERIFY(operation);
        QCOMPARE(f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileExport,0,{},0}, {}), ReceiveDispatch::Unsupported);
        f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileMetadata,10,{},0}, {});
        f.reply();
        QCOMPARE(f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileExport,0,{},0}, {}), ReceiveDispatch::Dispatched);
        QCOMPARE(f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileExport,0,{},0}, {}), ReceiveDispatch::Unsupported);
        QCOMPARE(f.commands.size(),2);
    }

    void actionTimeEligibilityAndOwnerDestruction()
    {
        Fixture f;
        auto operation = f.begin(FileTransferPurpose::ProfileMetadata);
        QVERIFY(operation);
        f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileMetadata,10,{},0}, {});
        f.reply();
        RadioSetupControlTestAccess::transmitting(f.model,true);
        QCOMPARE(f.model.requestFileTransfer(operation, {FileTransferPurpose::ProfileExport,0,{},0}, {}), ReceiveDispatch::Unsupported);
        QCOMPARE(f.commands.size(),1);
        Fixture owner;
        auto caller = std::make_unique<QObject>();
        auto held = owner.model.beginFileTransfer(FileTransferPurpose::LegacyWaveform,
            caller.get(), owner.model.controlSession(),30000);
        QVERIFY(held);
        caller.reset();
        QVERIFY(!held->current());
        QVERIFY(owner.begin(FileTransferPurpose::LegacyWaveform));
        Fixture docker;
        docker.backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        QVERIFY(!docker.begin(FileTransferPurpose::DockerWaveform));
        docker.model.handleLineForTest(QStringLiteral("S0|waveform wfp_status power=on ready=true"));
        QVERIFY(docker.begin(FileTransferPurpose::DockerWaveform));
    }

    void waveformActions()
    {
        Fixture f;
        for (const auto action : {WaveformAction::Restart,WaveformAction::Uninstall,WaveformAction::RemoveContainer}) {
            QCOMPARE(f.model.requestWaveform({action,"ThumbDV"}, &f.owner, {}), ReceiveDispatch::Dispatched);
        }
        QCOMPARE(f.commands, QStringList({"waveform restart ThumbDV", "waveform uninstall ThumbDV", "waveform remove_container ThumbDV"}));
        QCOMPARE(f.model.requestWaveform({WaveformAction::Restart,"bad\nname"}, &f.owner, {}), ReceiveDispatch::Unsupported);
    }
    // An answered waveform action leaves no connection behind on its owner.
    void answeredWaveformActionReleasesOwner()
    {
        Fixture f;
        const int before = f.owner.destroyedLinks();
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(f.model.requestWaveform({WaveformAction::Restart,"ThumbDV"}, &f.owner, {}),
                     ReceiveDispatch::Dispatched);
            f.reply();
        }
        QCOMPARE(f.owner.destroyedLinks(), before);
    }
};
QTEST_GUILESS_MAIN(FileTransferControlTest)
#include "file_transfer_control_test.moc"
