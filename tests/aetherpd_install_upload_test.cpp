#include "core/AetherPDInstallUpload.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>

using Upload = AetherSDR::AetherPDInstallUpload;
using State = Upload::State;
using Reason = Upload::Reason;
namespace {
bool check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; }
    return value;
}
QJsonObject vectors;
QByteArray bytes(const char* key) { return QByteArray::fromHex(vectors.value(QLatin1String(key)).toString().toLatin1()); }
Upload::Inputs inputs()
{
    Upload::Inputs result;
    const QJsonArray entries = vectors.value("inputs").toArray();
    for (std::size_t i = 0; i < result.size(); ++i) { result[i] = QByteArray::fromHex(entries[static_cast<int>(i)].toString().toLatin1()); }
    return result;
}
class Channel final : public AetherSDR::AetherPDInstallChannel {
public:
    QByteArray incoming, outgoing;
    qint64 pending{0}, maximumPending{0}, maxWrite{65536};
    bool connectedOnce{false}, closedOnce{false}, halfClosed{false}, halfCloseSucceeds{true};
    std::function<void()> onWrite, onClose, onHalfClose;
    void connectTo(const QHostAddress& address) override { target = address; connectedOnce = true; }
    qint64 write(const char* data, qint64 size) override
    {
        const qint64 count = std::min(maxWrite, size);
        if (count > 0) { outgoing.append(data, count); pending += count; maximumPending = std::max(maximumPending, pending); }
        if (onWrite) { onWrite(); }
        return count;
    }
    qint64 pendingBytes() const override { return pending; }
    qint64 availableBytes() const override { return incoming.size(); }
    QByteArray read(qint64 limit) override { QByteArray result = incoming.first(std::min(limit, static_cast<qint64>(incoming.size()))); incoming.remove(0, result.size()); return result; }
    bool finishWrites() override
    {
        halfClosed = pending == 0;
        if (onHalfClose) { onHalfClose(); }
        return halfCloseSucceeds && halfClosed;
    }
    void close() override { closedOnce = true; if (onClose) { onClose(); } emit closed(); }
    void establish() { emit connected(); }
    void feed(const QByteArray& data) { incoming += data; emit readable(); }
    void drain() { pending = 0; emit writable(); }
    QHostAddress target;
};
const QHostAddress address(QStringLiteral("192.168.50.100"));
bool start(Upload& upload, Channel& channel)
{
    if (!upload.begin(address, bytes("private"), inputs())) { return false; }
    channel.establish(); channel.feed(bytes("hello"));
    for (int i = 0; i < 20000 && upload.state() == State::Sending; ++i) { channel.drain(); }
    return upload.state() == State::AwaitingVerification;
}
bool success()
{
    bool ok = true;
    QByteArray expected = bytes("header");
    for (const QByteArray& part : inputs()) { expected += part; }
    for (const int code : {0,3,4,5,6}) {
        for (const qint64 chunk : {qint64(1), qint64(17), qint64(65536)}) {
            Channel channel; channel.maxWrite = chunk; qint64 time = 0;
            Upload upload(channel, [&] { return time; }); int verified = 0, finished = 0, result = -1;
            QObject::connect(&upload, &Upload::inputsVerified, [&] { ++verified; });
            QObject::connect(&upload, &Upload::finished, [&](State, Reason reason, int value) { ++finished; result = value; ok &= check(reason == Reason::None, "successful receipt has no error"); });
            ok &= check(start(upload, channel), "authenticate and stream partial writes");
            ok &= check(channel.outgoing == expected && channel.halfClosed && channel.maximumPending <= 65536, "Python wire match and bounded EOF after drain");
            const QByteArray response = bytes("ack_1_0") + bytes(("ack_2_" + QByteArray::number(code)).constData());
            // Exercise both fragmented frames and coalesced stage acknowledgements.
            if (chunk == 1) { for (const char byte : response) { channel.feed(QByteArray(1, byte)); } }
            else { channel.feed(response); }
            ok &= check(upload.state() == State::Finished && result == code && verified == 1 && finished == 1 && channel.closedOnce, "authenticated result, not installed commit");
            ok &= check(!upload.begin(address, bytes("private"), inputs()), "no replay on used uploader");
        }
    }
    return ok;
}
bool refusals()
{
    bool ok = true;
    for (const int part : {0,1,2,3}) {
        Channel channel; qint64 time = 0; Upload upload(channel, [&] { return time; });
        Upload::Inputs bad = inputs(); bad[part][bad[part].size()-1] ^= 1;
        ok &= check(!upload.begin(address, bytes("private"), bad) && !channel.connectedOnce, "mismatched artifact refused before connecting");
    }
    for (const int part : {0,1,2,3}) {
        Channel channel; qint64 time = 0; Upload upload(channel, [&] { return time; });
        Upload::Inputs bad = inputs(); bad[part].clear();
        ok &= check(!upload.begin(address, bytes("private"), bad), "empty artifact refused");
    }
    {
        Channel channel; qint64 time = 0; Upload upload(channel, [&] { return time; });
        Upload::Inputs bad = inputs(); bad[2] = QByteArray(18*1024*1024+1, 'x');
        ok &= check(!upload.begin(address, bytes("private"), bad) && !channel.connectedOnce, "oversized release refused before hashing or connecting");
    }
    for (const int attack : {0,1,2,3,4,5,6,7,8,9}) {
        Channel channel; qint64 time = 0; Upload upload(channel, [&] { return time; });
        int finished = 0, verified = 0;
        QObject::connect(&upload, &Upload::finished, [&](State, Reason, int) { ++finished; });
        QObject::connect(&upload, &Upload::inputsVerified, [&] { ++verified; });
        if (attack < 3) {
            upload.begin(address, bytes("private"), inputs()); channel.establish();
            QByteArray hello = bytes("hello");
            if (attack == 0) { hello[40] ^= 1; }
            if (attack == 1) { hello[8] ^= 1; }
            if (attack == 2) { hello += QByteArray(25, 'x'); }
            channel.feed(hello);
            ok &= check(channel.outgoing.isEmpty(), "no artifact sent to unauthenticated server");
        } else {
            ok &= check(start(upload, channel), "attack test setup");
            QByteArray ack = bytes("ack_1_0");
            if (attack == 3) { ack[16] ^= 1; }
            if (attack == 4) { ack = bytes("ack_2_0"); }
            if (attack == 5) { ack[15] = 3; }
            if (attack == 6) { ack = QByteArray(97, 'x'); }
            channel.feed(ack);
            if (attack >= 7) {
                QByteArray last = bytes("ack_2_0");
                if (attack == 7) { last[16] ^= 1; }
                if (attack == 8) { last = bytes("ack_1_0"); }
                if (attack == 9) { last += 'x'; }
                channel.feed(last);
            }
        }
        ok &= check(upload.state() == State::Unconfirmed && finished == 1 && verified == (attack >= 7 ? 1 : 0), "malformed, wrong-stage or wrong-MAC reply refused");
    }
    return ok;
}
bool boundsAndLifetime()
{
    bool ok = true;
    for (const int phase : {0,1,2,3,4}) {
        Channel channel; qint64 time = 0; Upload upload(channel, [&] { return time; });
        Reason reason = Reason::None;
        QObject::connect(&upload, &Upload::finished, [&](State, Reason value, int) { reason = value; });
        upload.begin(address, bytes("private"), inputs());
        if (phase > 0) { channel.establish(); }
        if (phase > 1) { channel.feed(bytes("hello")); }
        if (phase > 2) { for (int i=0;i<20 && !channel.halfClosed;++i) { channel.drain(); } }
        if (phase > 3) { time = 80000; channel.feed(bytes("ack_1_0")); }
        time = phase > 3 ? 270000 : 90000; upload.poll();
        ok &= check(upload.state() == State::Unconfirmed && reason == Reason::Deadline, "total deadline covers connection, input and delivery");
    }
    Channel channel; qint64 time = 0;
    auto upload = std::make_unique<Upload>(channel, [&] { return time; });
    upload->begin(address, bytes("private"), inputs()); channel.establish();
    for (const char byte : bytes("hello")) { ++time; channel.feed(QByteArray(1, byte)); }
    channel.pending = 65536; time = 89999; upload->poll();
    ok &= check(upload->state() == State::Sending, "backpressure stays bounded without renewing deadline");
    time = 90000; upload->poll();
    ok &= check(upload->state() == State::Unconfirmed, "slow progress does not renew deadline");
    time = 0;
    auto transport = std::make_unique<Channel>();
    Upload lost(*transport, [&] { return time; }); lost.begin(address, bytes("private"), inputs()); transport.reset();
    ok &= check(lost.state() == State::Unconfirmed, "channel destruction retires upload");
    Channel reentrant;
    upload = std::make_unique<Upload>(reentrant, [&] { return time; });
    upload->begin(address, bytes("private"), inputs()); reentrant.establish();
    reentrant.onWrite = [&] { upload.reset(); };
    reentrant.feed(bytes("hello"));
    ok &= check(!upload && reentrant.closedOnce, "destruction inside write is safe");
    Channel cancel;
    Upload canceled(cancel, [&] { return time; }); start(canceled, cancel);
    QObject::connect(&canceled, &Upload::inputsVerified, [&] { canceled.cancel(); });
    cancel.feed(bytes("ack_1_0") + bytes("ack_2_0"));
    ok &= check(canceled.state() == State::Canceled, "cancel in stage-one callback never reports delivery");
    Channel halfFailure; halfFailure.halfCloseSucceeds = false;
    Upload failedEof(halfFailure, [&] { return time; });
    start(failedEof, halfFailure);
    ok &= check(failedEof.state() == State::Unconfirmed, "failed write-side EOF is not input verification");
    Channel writeFailure; writeFailure.maxWrite = -1;
    Upload failedWrite(writeFailure, [&] { return time; });
    start(failedWrite, writeFailure);
    ok &= check(failedWrite.state() == State::Unconfirmed, "write failure terminates without replay");
    Channel deleted;
    upload = std::make_unique<Upload>(deleted, [&] { return time; });
    start(*upload, deleted);
    QObject::connect(upload.get(), &Upload::inputsVerified, [&] { upload.reset(); });
    deleted.feed(bytes("ack_1_0") + bytes("ack_2_0"));
    ok &= check(!upload, "stage-one observer can destroy uploader with second frame buffered");
    Channel backwards; Upload invalidClock(backwards, [&] { return time; });
    time = 10; invalidClock.begin(address, bytes("private"), inputs()); --time; invalidClock.poll();
    ok &= check(invalidClock.state() == State::Unconfirmed, "backwards clock fails closed");
    return ok;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QFile file(QStringLiteral(AETHERPD_UPLOAD_TESTDATA));
    if (!file.open(QIODevice::ReadOnly)) { return 2; }
    vectors = QJsonDocument::fromJson(file.readAll()).object();
    const bool ok = success() & refusals() & boundsAndLifetime();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return ok ? 0 : 1;
}
