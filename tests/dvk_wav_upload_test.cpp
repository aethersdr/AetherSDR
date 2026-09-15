#include "core/DvkWavTransfer.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVector>

#include <cstdio>
#include <utility>

namespace AetherSDR {

class DvkWavTransferTestAccess {
public:
    static void prepareUpload(DvkWavTransfer& transfer, QTcpSocket* socket,
                              const QByteArray& data)
    {
        transfer.m_uploadData = data;
        transfer.m_bytesSent = 0;
        transfer.m_bytesAccepted = 0;
        transfer.m_slotId = 1;
        transfer.m_direction = DvkWavTransfer::Upload;
        transfer.m_transferring = true;
        transfer.m_cancelled = false;
        transfer.m_finished = false;
        transfer.m_client = socket;
        QObject::connect(socket, &QTcpSocket::bytesWritten,
                         &transfer, &DvkWavTransfer::onUploadBytesWritten);
        QObject::connect(socket, &QTcpSocket::errorOccurred,
                         &transfer, [&transfer]() { transfer.onUploadError(); });
    }

    static void connected(DvkWavTransfer& transfer)
    {
        transfer.onUploadConnected();
    }

    static qint64 bytesAccepted(const DvkWavTransfer& transfer)
    {
        return transfer.m_bytesAccepted;
    }
};

} // namespace AetherSDR

namespace {

class PartialWriteSocket final : public QTcpSocket {
public:
    explicit PartialWriteSocket(QVector<qint64> results, QObject* parent)
        : QTcpSocket(parent), m_results(std::move(results))
    {
        setSocketState(QAbstractSocket::ConnectedState);
        open(QIODevice::WriteOnly);
    }

    const QByteArray& acceptedData() const { return m_acceptedData; }
    bool errorDuringWrite{false};

    void drain(qint64 bytes)
    {
        emit bytesWritten(bytes);
    }

protected:
    qint64 writeData(const char* data, qint64 maxSize) override
    {
        if (errorDuringWrite) {
            setErrorString("injected synchronous write error");
            emit errorOccurred(QAbstractSocket::NetworkError);
            return -1;
        }
        const qint64 result = m_results.isEmpty() ? maxSize : m_results.takeFirst();
        if (result <= 0) {
            return result;
        }

        const qint64 accepted = qMin(result, maxSize);
        m_acceptedData.append(data, accepted);
        return accepted;
    }

private:
    QVector<qint64> m_results;
    QByteArray m_acceptedData;
};

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

QByteArray testPayload(qsizetype size)
{
    QByteArray payload;
    payload.resize(size);
    quint32 state = 0x12345678;
    for (qsizetype i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = static_cast<char>(state & 0xff);
    }
    return payload;
}

void testFullChunkSplitNotifications()
{
    const QByteArray payload = testPayload(150'003);
    AetherSDR::DvkWavTransfer transfer(nullptr);
    auto* socket = new PartialWriteSocket({}, &transfer);
    QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
    AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, payload);
    AetherSDR::DvkWavTransferTestAccess::connected(transfer);
    check(socket->acceptedData() == payload.left(65'536), "first chunk is bounded to 64 KiB");
    socket->drain(32'768);
    check(socket->acceptedData() == payload.left(65'536),
          "half-chunk drain must not enqueue the already queued tail again");
    socket->drain(32'768);
    check(socket->acceptedData() == payload.left(131'072), "second chunk follows the first exactly");
    socket->drain(1);
    socket->drain(65'535);
    check(socket->acceptedData() == payload, "queued output equals the full source byte for byte");
    check(finished.isEmpty(), "fully queued data is not yet upload completion");
    socket->drain(18'930);
    check(finished.isEmpty(), "the final byte must drain before success");
    socket->drain(1);
    check(finished.count() == 1 && finished.at(0).at(0).toBool(), "final drain succeeds exactly once");
}

void testSynchronousWriteError()
{
    AetherSDR::DvkWavTransfer transfer(nullptr);
    auto* socket = new PartialWriteSocket({}, &transfer);
    socket->errorDuringWrite = true;
    QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
    AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, testPayload(32));
    AetherSDR::DvkWavTransferTestAccess::connected(transfer);
    check(finished.count() == 1 && !finished.at(0).at(0).toBool(),
          "a synchronous write error cleans up once without dereferencing the old socket");
    check(!transfer.isTransferring(), "a synchronous error returns the transfer to idle");
}

void testInvalidDrainAndEarlyClose()
{
    for (const qint64 count : {qint64{-1}, qint64{0}, qint64{33}}) {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        auto* socket = new PartialWriteSocket({}, &transfer);
        QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
        AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, testPayload(32));
        AetherSDR::DvkWavTransferTestAccess::connected(transfer);
        socket->drain(count);
        check(finished.count() == 1 && !finished.at(0).at(0).toBool(),
              "invalid drain counts cannot advance progress or report success");
    }

    AetherSDR::DvkWavTransfer transfer(nullptr);
    auto* socket = new PartialWriteSocket({}, &transfer);
    QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
    AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, testPayload(32));
    AetherSDR::DvkWavTransferTestAccess::connected(transfer);
    socket->drain(16);
    emit socket->errorOccurred(QAbstractSocket::RemoteHostClosedError);
    socket->drain(16);
    check(finished.count() == 1 && !finished.at(0).at(0).toBool(),
          "remote close with pending bytes fails once and ignores late drain");
}

void testPartialDrainDoesNotResendAcceptedBytes()
{
    const QByteArray payload = testPayload(70'000);
    AetherSDR::DvkWavTransfer transfer(nullptr);
    auto* socket = new PartialWriteSocket({11, 65'536, 4'453}, &transfer);
    QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);

    AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, payload);
    AetherSDR::DvkWavTransferTestAccess::connected(transfer);

    check(socket->acceptedData() == payload.left(11),
          "the first short write accepts only its leading payload span");
    socket->drain(4);
    check(socket->acceptedData() == payload.left(11),
          "a partial drain cannot queue an overlapping next chunk");
    check(finished.count() == 0, "a partial drain cannot finish the upload");

    socket->drain(7);
    check(socket->acceptedData() == payload.left(11 + 65'536),
          "the next chunk begins after all bytes accepted by the first write");
    socket->drain(65'536);
    check(socket->acceptedData() == payload,
          "all accepted spans concatenate to the original payload");
    check(finished.count() == 0,
          "accepting the final span cannot finish before it drains");

    socket->drain(4'453);
    check(finished.count() == 1, "draining the full payload finishes exactly once");
    check(finished.at(0).at(0).toBool(), "a fully drained payload succeeds");
    check(!transfer.isTransferring(), "completion returns the transfer to idle");
}

void testWriteFailuresAndCancellationLeaveNoStaleAcceptedCursor()
{
    for (const qint64 writeResult : {qint64{0}, qint64{-1}}) {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        auto* socket = new PartialWriteSocket({writeResult}, &transfer);
        QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
        AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, testPayload(32));

        AetherSDR::DvkWavTransferTestAccess::connected(transfer);
        check(finished.count() == 1 && !finished.at(0).at(0).toBool(),
              "a failed or zero-byte write cannot leave an upload stalled");
        check(AetherSDR::DvkWavTransferTestAccess::bytesAccepted(transfer) == 0,
              "cleanup resets the accepted-byte cursor after a write failure");
    }

    {
        AetherSDR::DvkWavTransfer transfer(nullptr);
        auto* socket = new PartialWriteSocket({16}, &transfer);
        QSignalSpy finished(&transfer, &AetherSDR::DvkWavTransfer::finished);
        AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, socket, testPayload(32));

        AetherSDR::DvkWavTransferTestAccess::connected(transfer);
        transfer.cancel();
        socket->drain(16);
        check(finished.count() == 1 && !finished.at(0).at(0).toBool(),
              "a cancellation ignores a late drain callback and finishes once");
        check(AetherSDR::DvkWavTransferTestAccess::bytesAccepted(transfer) == 0,
              "cleanup resets the accepted-byte cursor after cancellation");

        auto* reuseSocket = new PartialWriteSocket({32}, &transfer);
        AetherSDR::DvkWavTransferTestAccess::prepareUpload(transfer, reuseSocket, testPayload(32));
        AetherSDR::DvkWavTransferTestAccess::connected(transfer);
        reuseSocket->drain(32);
        check(finished.count() == 2 && finished.at(1).at(0).toBool(),
              "a cancelled transfer can be reused without stale accepted bytes");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    testFullChunkSplitNotifications();
    testPartialDrainDoesNotResendAcceptedBytes();
    testWriteFailuresAndCancellationLeaveNoStaleAcceptedCursor();
    testSynchronousWriteError();
    testInvalidDrainAndEarlyClose();
    return g_failures == 0 ? 0 : 1;
}
