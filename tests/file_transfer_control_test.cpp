// Production encoder, admission and queued writer; no socket is initialized.
#include "RadioSetupControlFixture.h"
#include "core/backends/FileTransferOperation.h"
#include "core/backends/flex/RadioConnection.h"
#include <QCoreApplication>
#include <QEvent>
#include <QtTest>

using namespace AetherSDR;
namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static void capture(RadioConnection& connection, QStringList& writes)
    {
        connection.m_state.store(ConnectionState::Connected);
        connection.m_commandSinkForTest = [&writes](quint32, const QString& text) { writes << text; };
    }
    static void replace(RadioConnection& connection) { ++connection.m_commandSession; }
};
}

struct Fixture {
    RadioModel model;
    RadioSetupTestBackend* backend{installRadioSetupBackend(model)};
    QObject owner;
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
        docker.model.flexWaveformModel().handleWfpStatus({{"power","on"},{"ready","true"}});
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

    void queuedWriterRechecksGuardAndSession()
    {
        RadioConnection connection;
        QStringList writes;
        RadioConnectionSessionTestAccess::capture(connection,writes);
        const auto enqueue = [&](FileTransferGuard guard) {
            const quint64 session = connection.commandSession();
            QMetaObject::invokeMethod(&connection, [&connection,guard,session] {
                connection.writeTransferCommand(1, QStringLiteral("file download db_package"), guard, session);
            }, Qt::QueuedConnection);
        };
        FileTransferGuard cancelled(30000);
        enqueue(cancelled); cancelled.cancel();
        QCoreApplication::sendPostedEvents(&connection,QEvent::MetaCall);
        QVERIFY(writes.isEmpty());
        enqueue(FileTransferGuard(30000));
        RadioConnectionSessionTestAccess::replace(connection);
        QCoreApplication::sendPostedEvents(&connection,QEvent::MetaCall);
        QVERIFY(writes.isEmpty());
        enqueue(FileTransferGuard(0));
        QCoreApplication::sendPostedEvents(&connection,QEvent::MetaCall);
        QVERIFY(writes.isEmpty());
        enqueue(FileTransferGuard(30000));
        QCoreApplication::sendPostedEvents(&connection,QEvent::MetaCall);
        QCOMPARE(writes.size(),1);
    }
};
QTEST_GUILESS_MAIN(FileTransferControlTest)
#include "file_transfer_control_test.moc"
