// Socket-free tests of our own HTTP transport. The injected manager returns
// controlled replies; it never opens a listener or models radio firmware.
#include "AetherPDTestNetwork.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using AetherSDR::AetherPDClient;
    using namespace AetherSDR::aetherpd;
    using Failure = AetherPDClient::Failure;
    bool ok = true;
    Network network;
    AetherPDClient client(network);
    const QString token(43, u'a');
    {
        Network freshNetwork;
        AetherPDClient freshClient(freshNetwork);
        freshClient.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.100")));
        if (!check(freshClient.submit(Operation::Status, QString(64, u'a')),
                   "first-install identity can use ordinary AetherPD control")) { return 1; }
        if (!check(freshNetwork.sent.back().request.rawHeader("Authorization") == "Bearer " + QByteArray(64, 'a'),
                   "first-install token preserved exactly")) { return 1; }
    }
    {
        Network selectedNetwork;
        AetherPDClient sender(selectedNetwork);
        sender.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.100")));
        const StartIdentity identity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"),
            0x0321b4de, IQSelection{3, 0x40000001}};
        ok &= check(sender.submit(Operation::Start, token, identity)
            && selectedNetwork.sent.back().request.url().path() == QStringLiteral("/v2/aetherpd/start")
            && selectedNetwork.sent.back().body == *startBody(identity), "selected request changed or used legacy endpoint");
        selectedNetwork.last->complete(404, "{}");
        ok &= check(selectedNetwork.sent.size() == 1 && !sender.busy(), "unsupported selection retried legacy start");
    }
    {
        Network configured;
        AetherPDClient sender(configured);
        sender.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.100")));
        ok &= check(!sender.submit(Operation::Status, token, std::nullopt, 1001)
            && !sender.submit(Operation::ArmMeasurement, token, std::nullopt, 199) && configured.sent.empty(),
            "invalid configured operation reached transport");
        ok &= check(sender.submit(Operation::ArmMeasurement, token, std::nullopt, 1001)
            && configured.sent.back().request.url().path() == QStringLiteral("/v2/aetherpd/measurement/arm")
            && configured.sent.back().body == QByteArrayLiteral("{\"spacing_hz\":1001}"), "configured arm altered spacing or version");
        configured.last->complete(501, "{}");
        ok &= check(configured.sent.size() == 1 && !sender.busy(), "configured request silently fell back");
        ok &= check(sender.submit(Operation::ArmMeasurement, token, std::nullopt, 2610), "DC-offset configuration refused by transport");
        Reply* armedRequest = configured.last;
        ok &= check(sender.submit(Operation::CancelCalibration, token) && armedRequest->aborted,
            "cancel failed to preempt configured arm");
    }
    int statusCount = 0;
    std::vector<Acknowledgment> acknowledgments;
    std::vector<Failure> failures;
    std::vector<Operation> cancellations;
    QObject::connect(&client, &AetherPDClient::statusReceived, &app, [&](const Status&) { ++statusCount; });
    QObject::connect(&client, &AetherPDClient::acknowledged, &app,
                     [&](Operation, Acknowledgment acknowledgment) { acknowledgments.push_back(acknowledgment); });
    QObject::connect(&client, &AetherPDClient::failed, &app,
                     [&](Operation, Failure failure) { failures.push_back(failure); });
    QObject::connect(&client, &AetherPDClient::requestCanceled, &app,
                     [&](Operation operation) { cancellations.push_back(operation); });
    ok &= check(!client.submit(Operation::ArmCalibration, token) && network.sent.empty(), "missing address refuses before transport");
    client.setRadioAddress(QHostAddress(QStringLiteral("8.8.8.8")));
    ok &= check(!client.submit(Operation::Status, token) && network.sent.empty(), "public target refuses token disclosure");
    client.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.100")));
    ok &= check(!client.submit(Operation::Status, QStringLiteral("bad\r\ntoken")) && network.sent.empty(), "invalid token refuses before transport");
    ok &= check(!client.submit(static_cast<Operation>(99), token) && network.sent.empty(), "unknown operation refuses");
    ok &= check(client.submit(Operation::Status, token), "status submitted");
    ok &= check(network.sent.size() == 1 && network.sent.back().method == QNetworkAccessManager::GetOperation
        && network.sent.back().request.url() == QUrl(QStringLiteral("http://192.168.50.100:48992/v1/aetherpd/status"))
        && network.sent.back().body.isEmpty()
        && network.sent.back().request.rawHeader("Authorization") == "Bearer " + token.toLatin1()
        && network.sent.back().request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy,
        "fixed request, empty body, explicit auth and no redirect");
    QFile fixture(QStringLiteral(AETHERPD_TESTDATA_DIR "/idle.json"));
    ok &= check(fixture.open(QIODevice::ReadOnly), "measured Go fixture readable");
    const QByteArray idle = fixture.readAll();
    network.last->complete(200, idle);
    ok &= check(statusCount == 1 && !client.busy(), "measured status delivered");
    ok &= check(client.submit(Operation::ArmCalibration, token), "arm submitted");
    Reply* old = network.last;
    const size_t beforeBusy = network.sent.size();
    ok &= check(!client.submit(Operation::ArmCalibration, token) && network.sent.size() == beforeBusy
        && failures.back() == Failure::Busy, "duplicate arm is not queued");
    ok &= check(client.submit(Operation::CancelCalibration, token) && old->aborted
        && cancellations.back() == Operation::ArmCalibration, "cancel preempts arm");
    ok &= check(network.sent.back().request.url().path() == QStringLiteral("/v1/aetherpd/calibration/cancel")
        && network.sent.back().method == QNetworkAccessManager::PostOperation && network.sent.back().body.isEmpty(), "parameterless cancel POST");
    const size_t ackCount = acknowledgments.size();
    old->complete(200, R"({"state":"calibration_arm_accepted"})");
    ok &= check(acknowledgments.size() == ackCount && client.busy(), "late arm reply cannot override cancel");
    network.last->complete(200, R"({"state":"calibration_cancel_accepted"})");
    ok &= check(!acknowledgments.empty() && acknowledgments.back() == Acknowledgment::CancelAccepted, "cancel acceptance delivered");
    client.submit(Operation::RestoreReceive, token);
    old = network.last;
    ok &= check(client.submit(Operation::Stop, token) && old->aborted, "stop preempts recovery");
    const size_t beforeStop = network.sent.size();
    ok &= check(!client.submit(Operation::CancelCalibration, token)
        && !client.submit(Operation::Stop, token) && network.sent.size() == beforeStop
        && client.busy(), "cancel or duplicate stop cannot replace an in-flight stop");
    network.last->complete(202, R"({"state":"stop_requested"})");
    ok &= check(!acknowledgments.empty() && acknowledgments.back() == Acknowledgment::StopRequested, "202 means requested only");
    client.submit(Operation::Status, token);
    old = network.last;
    client.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.101")));
    old->complete(200, idle);
    ok &= check(old->aborted && !client.busy() && statusCount == 1, "address change cancels stale status");
    for (const auto& scenario : std::vector<std::pair<int, Failure>>{{401, Failure::Unauthorized}, {302, Failure::Refused}, {503, Failure::Refused}, {0, Failure::Transport}}) {
        client.submit(Operation::Status, token);
        const size_t before = network.sent.size();
        network.last->complete(scenario.first, "{}");
        ok &= check(failures.back() == scenario.second && !client.busy() && network.sent.size() == before, "HTTP/transport failure never retries");
    }
    client.submit(Operation::Status, token);
    network.last->complete(200, idle, QNetworkReply::RemoteHostClosedError);
    ok &= check(failures.back() == Failure::Transport && statusCount == 1, "truncated transport cannot publish valid-looking status");
    client.submit(Operation::Status, token);
    network.last->complete(200, QByteArray(16 * 1024 + 1, ' '));
    ok &= check(failures.back() == Failure::OversizedReply && !client.busy(), "oversized reply aborted");
    client.submit(Operation::Status, token);
    network.last->complete(200, "{}");
    ok &= check(failures.back() == Failure::InvalidReply, "invalid status rejected");
    // A cancellation observer selecting a new radio must not receive the old
    // caller's token through a request resumed after that observer returns.
    client.submit(Operation::ArmCalibration, token);
    const size_t beforeRetarget = network.sent.size();
    const QMetaObject::Connection retarget = QObject::connect(&client, &AetherPDClient::requestCanceled, &app,
        [&](Operation) { client.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.102"))); });
    ok &= check(!client.submit(Operation::Stop, token) && network.sent.size() == beforeRetarget
        && failures.back() == Failure::InvalidRequest, "reentrant retarget cannot carry old authority to new radio");
    QObject::disconnect(retarget);
    client.submit(Operation::Status, token);
    QEventLoop loop;
    const QMetaObject::Connection timeout = QObject::connect(&client, &AetherPDClient::failed, &loop,
        [&](Operation, Failure) { loop.quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(timeout);
    ok &= check(!client.busy() && failures.back() == Failure::Timeout && network.last->aborted, "absolute status deadline aborts pending request");
    {
        AetherPDClient temporary(network);
        temporary.setRadioAddress(QHostAddress::LocalHost);
        temporary.submit(Operation::ArmCalibration, token);
    }
    ok &= check(network.last && network.last->aborted, "client destruction aborts pending arm");
    {
        std::unique_ptr<Network> transient = std::make_unique<Network>();
        AetherPDClient detached(*transient);
        detached.setRadioAddress(QHostAddress::LocalHost);
        int lostTransport = 0;
        QObject::connect(&detached, &AetherPDClient::failed, &app,
            [&](Operation, Failure failure) { lostTransport += failure == Failure::Transport ? 1 : 0; });
        detached.submit(Operation::Status, token);
        transient.reset();
        ok &= check(!detached.busy() && lostTransport == 1
            && !detached.submit(Operation::ArmCalibration, token), "lost manager fails pending request and refuses further arms");
    }
    {
        std::unique_ptr<AetherPDClient> removed = std::make_unique<AetherPDClient>(network);
        removed->setRadioAddress(QHostAddress::LocalHost);
        removed->submit(Operation::ArmCalibration, token);
        QObject::connect(removed.get(), &AetherPDClient::requestCanceled, &app,
            [&](Operation) { removed.reset(); });
        AetherPDClient* active = removed.get();
        ok &= check(!active->submit(Operation::Stop, token) && !removed,
                    "cancellation observer can destroy client without resuming old request");
    }
    {
        Network startNetwork;
        AetherPDClient starter(startNetwork);
        starter.setRadioAddress(QHostAddress(QStringLiteral("192.168.50.100")));
        const StartIdentity identity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 0x0321b4de};
        ok &= check(!starter.submit(Operation::Start, token) && startNetwork.sent.empty(),
                    "start cannot use a missing identity");
        ok &= check(!starter.submit(Operation::Status, token, identity) && startNetwork.sent.empty(),
                    "identity cannot become parameters for another verb");
        starter.submit(Operation::Status, token);
        Reply* pendingPoll = startNetwork.last;
        ok &= check(starter.submit(Operation::Start, token, identity) && pendingPoll->aborted,
                    "explicit start preempts a status poll");
        const Network::Sent& sent = startNetwork.sent.back();
        ok &= check(sent.method == QNetworkAccessManager::PostOperation
            && sent.request.url().path() == QStringLiteral("/v1/aetherpd/start")
            && sent.request.header(QNetworkRequest::ContentTypeHeader).toString() == QStringLiteral("application/json")
            && sent.body == *startBody(identity) && sent.request.transferTimeout() == 22'000,
            "start uses the fixed endpoint, bounded deadline and identity-only JSON");
        Reply* pendingStart = startNetwork.last;
        const size_t beforeDuplicate = startNetwork.sent.size();
        ok &= check(!starter.submit(Operation::Start, token, identity)
            && startNetwork.sent.size() == beforeDuplicate, "duplicate start is not queued");
        ok &= check(starter.submit(Operation::Stop, token) && pendingStart->aborted,
                    "stop preempts startup without waiting for its long deadline");
        pendingStart->complete(202, R"({"state":"start_requested"})");
        ok &= check(starter.busy(), "late start receipt cannot finish the stop request");
        startNetwork.last->complete(202, R"({"state":"stop_requested"})");
        starter.submit(Operation::Start, token, identity);
        const size_t beforeLost = startNetwork.sent.size();
        startNetwork.last->complete(0, {}, QNetworkReply::RemoteHostClosedError);
        ok &= check(!starter.busy() && startNetwork.sent.size() == beforeLost,
                    "lost start reply cannot cause an automatic retry");
    }
    return ok ? 0 : 1;
}
