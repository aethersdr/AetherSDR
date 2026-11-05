// Model lifecycle only: no sockets, peer process or RF.
#include "models/NavtexModel.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>
#include <limits>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* label)
{
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
quint64 latest(const QSignalSpy& spy) { return spy.last().last().toULongLong(); }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        NavtexModel model;
        QSignalSpy requests(&model, &NavtexModel::controlRequested);
        QSignalSpy settled(&model, &NavtexModel::operationSettled);
        model.sendMessage('A', 'B', "TEST MESSAGE", 123);
        check(requests.size() == 1, "one typed submission");
        const NavtexRequest request = requests.last().first().value<NavtexRequest>();
        check(request.transmitter == 'A' && request.subject == 'B'
            && request.text == "TEST MESSAGE" && request.serial == 123, "intent preserves message fields");
        const quint64 token = latest(requests);
        model.handleSendResponse(token, {TransmitServiceReply::Outcome::Accepted, 0, "42"});
        check(model.messages().size() == 1 && model.messages().first().status == NavtexMsgStatus::Queued
            && settled.isEmpty(), "acceptance queues without claiming completed transmission");
        model.parseStatus("navtex sent", {{"idx", "42"}, {"serial_num", "124"}});
        check(model.messages().first().status == NavtexMsgStatus::Sent && settled.size() == 1
            && model.messages().first().serialNum == 124
            && !model.messages().first().dateTime.isEmpty(), "sent status completes the matching operation");
        model.parseStatus("navtex sent", {{"idx", "42"}});
        model.handleSendResponse(token, {TransmitServiceReply::Outcome::Rejected, 7, {}});
        check(settled.size() == 1 && model.messages().size() == 1
            && model.messages().first().status == NavtexMsgStatus::Sent, "duplicates cannot regress sent");
    }
    {
        NavtexModel model;
        QSignalSpy requests(&model, &NavtexModel::controlRequested);
        QSignalSpy settled(&model, &NavtexModel::operationSettled);
        model.sendMessage('A', 'B', "early status");
        model.parseStatus("navtex sent", {{"idx", "7"}, {"serial_num", "5"}});
        model.handleSendResponse(latest(requests), {TransmitServiceReply::Outcome::Accepted, 0, "7"});
        check(model.messages().size() == 1 && model.messages().first().msgText == "early status"
            && model.messages().first().status == NavtexMsgStatus::Sent && settled.size() == 1,
            "status before reply merges into one message with original text");
        model.reset();
        model.sendMessage('A', 'B', "new session");
        model.handleSendResponse(latest(requests), {TransmitServiceReply::Outcome::Accepted, 0, "7"});
        check(model.messages().size() == 2 && model.messages().last().status == NavtexMsgStatus::Queued,
            "radio index reuse does not inherit another session's sent status");
    }
    {
        NavtexModel model;
        QSignalSpy requests(&model, &NavtexModel::controlRequested);
        QSignalSpy settled(&model, &NavtexModel::operationSettled);
        model.sendMessage('A', 'B', "unknown");
        const quint64 old = latest(requests);
        model.handleSendResponse(old, {TransmitServiceReply::Outcome::Unknown, 0, {}});
        check(model.messages().last().status == NavtexMsgStatus::Unknown && settled.isEmpty(),
            "timeout never claims rejected, sent or cancelled at the radio");
        model.sendMessage('A', 'B', "missing index");
        model.handleSendResponse(latest(requests), {TransmitServiceReply::Outcome::Accepted, 0, "not-an-index"});
        check(model.messages().last().status == NavtexMsgStatus::Unknown, "malformed acceptance has unknown outcome");
        model.sendMessage('A', 'B', "rejected");
        model.handleSendResponse(latest(requests), {TransmitServiceReply::Outcome::Rejected, 0x50001000, {}});
        check(model.messages().last().status == NavtexMsgStatus::Error && settled.size() == 1,
            "confirmed rejection settles only its own request");
        model.sendMessage('A', 'B', "retired");
        const quint64 retired = latest(requests);
        model.reset();
        const qsizetype count = model.messages().size();
        model.handleSendResponse(retired, {TransmitServiceReply::Outcome::Accepted, 0, "9"});
        check(model.messages().size() == count && model.messages().last().status == NavtexMsgStatus::Unknown,
            "reset retires pending work and ignores late replies");
    }
    {
        NavtexModel model;
        QSignalSpy requests(&model, &NavtexModel::controlRequested);
        for (const QString& text : {QString(), QString("a\nb"), QString("a\rb"),
                                   QString("a|b"), QString(QChar(0)), QString(4097, 'x')}) {
            model.sendMessage('A', 'B', text);
        }
        model.sendMessage('\n', 'B', "injected");
        model.sendMessage('A', ' ', "injected");
        check(requests.isEmpty(), "malformed framing and bounded fields refuse before dispatch");
        model.sendMessage('A', 'B', QString(4096, 'x'), std::numeric_limits<uint>::max());
        check(requests.size() == 1, "maximum valid payload and serial accepted as intent");
        for (int i = 0; i < 100; ++i) { model.sendMessage('A', 'B', "bounded"); }
        check(requests.size() == 16, "pending submissions are bounded");
        model.reset();
        for (int i = 0; i < 400; ++i) {
            model.parseStatus("navtex sent", {{"idx", QString::number(i)}});
        }
        check(model.messages().size() == 256, "history is bounded separately from pending intent");
    }
    {
        NavtexModel model;
        model.parseStatus("navtex", {{"status", "Active"}});
        check(model.status() == NavtexStatus::Active, "status case accepted");
        model.parseStatus("navtex", {{"status", "TRANSMITTING"}});
        check(model.status() == NavtexStatus::Transmitting, "transmitting status observed");
        model.parseStatus("navtex", {{"status", "queuefull"}});
        check(model.status() == NavtexStatus::QueueFull, "queue full preserved");
        model.parseStatus("navtex", {{"status", "garbage"}});
        check(model.status() == NavtexStatus::Error, "unknown status is an error");
    }
    {
        NavtexModel model;
        QSignalSpy requests(&model, &NavtexModel::controlRequested);
        QSignalSpy settled(&model, &NavtexModel::operationSettled);
        model.sendMessage('A', 'B', "retain ownership independently of history");
        model.handleSendResponse(latest(requests), {TransmitServiceReply::Outcome::Accepted, 0, "500"});
        for (int i = 0; i < 300; ++i) {
            model.parseStatus("navtex sent", {{"idx", QString::number(i)}});
        }
        model.parseStatus("navtex sent", {{"idx", "500"}, {"serial_num", "invalid"}});
        check(settled.isEmpty(), "malformed status cannot retire queue ownership");
        model.parseStatus("navtex sent", {{"idx", "500"}, {"serial_num", "1"}});
        check(settled.size() == 1, "history eviction cannot lose active queue ownership");
    }
    return failures ? 1 : 0;
}
