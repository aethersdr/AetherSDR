#include "core/FirmwareUploader.h"

#include <QCoreApplication>
#include <QVector>

#include <algorithm>
#include <cstdio>

namespace AetherSDR {

// The seam injects only write acceptance and callback delivery. It creates no
// socket, server, RadioModel, or radio connection; production queue and status
// handlers are exercised unchanged.
class FirmwareUploaderTestAccess {
public:
    static bool beginAllowed(FirmwareUploader& uploader, const QByteArray& data)
    {
        return uploader.beginOperation(data, QStringLiteral("test.ssdr"));
    }
    static void dispatched(FirmwareUploader& uploader) { uploader.markUploadDispatched(); }
    static void connectionChanged(FirmwareUploader& uploader, bool connected)
    {
        uploader.handleConnectionStateChanged(connected);
    }
    static FirmwareUploader::Generation begin(FirmwareUploader& uploader,
                                              const QByteArray& data)
    {
        uploader.beginOperation(data, QStringLiteral("test.ssdr"));
        return uploader.m_generation;
    }

    static FirmwareUploader::Generation start(FirmwareUploader& uploader,
                                              const QByteArray& data,
                                              FirmwareUploader::WriteFunction writer)
    {
        const FirmwareUploader::Generation generation = begin(uploader, data);
        uploader.startSending(generation, std::move(writer));
        return generation;
    }

    static void acknowledge(FirmwareUploader& uploader,
                            FirmwareUploader::Generation generation,
                            qint64 bytes)
    {
        uploader.acknowledgeBytes(generation, bytes);
    }

    static void disconnected(FirmwareUploader& uploader,
                             FirmwareUploader::Generation generation)
    {
        uploader.handleDisconnected(generation);
    }

    static void modelDisconnected(FirmwareUploader& uploader,
                                  FirmwareUploader::Generation generation)
    {
        uploader.handleModelDisconnected(generation);
    }

    static void status(FirmwareUploader& uploader,
                       FirmwareUploader::Generation generation,
                       const QString& object,
                       const QMap<QString, QString>& kvs)
    {
        uploader.onRadioStatus(generation, object, kvs);
    }

    static void reject(FirmwareUploader& uploader,
                       FirmwareUploader::Generation generation,
                       int code)
    {
        uploader.onUploadPortReceived(generation, code, {});
    }

    static void expire(FirmwareUploader& uploader,
                       FirmwareUploader::Generation generation,
                       quint64 timeoutToken,
                       const QString& message)
    {
        uploader.onTimeout(generation, timeoutToken, message);
    }

    static void expireOverall(FirmwareUploader& uploader,
                              FirmwareUploader::Generation generation,
                              quint64 timeoutToken)
    {
        uploader.onOverallTimeout(generation, timeoutToken);
    }

    static qint64 pending(const FirmwareUploader& uploader) { return uploader.m_pendingBytes; }
    static bool waiting(const FirmwareUploader& uploader) { return uploader.m_waitingForConfirmation; }
    static quint64 timeoutToken(const FirmwareUploader& uploader) { return uploader.m_timeoutToken; }
    static quint64 overallTimeoutToken(const FirmwareUploader& uploader)
    {
        return uploader.m_overallTimeoutToken;
    }
};

} // namespace AetherSDR

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

QByteArray nonPeriodicBytes(qsizetype size)
{
    QByteArray data;
    data.resize(size);
    quint32 state = 0xC0FFEE11U;
    for (qsizetype i = 0; i < size; ++i) {
        state = state * 1664525U + 1013904223U;
        data[i] = static_cast<char>((state >> 24U) & 0xffU);
    }
    return data;
}

struct FinishedEvent {
    bool success;
    QString message;
};

void checkRetryRequiresFreshConnection()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    const QByteArray data = nonPeriodicBytes(8);
    const auto first = AetherSDR::FirmwareUploaderTestAccess::begin(uploader, data);
    AetherSDR::FirmwareUploaderTestAccess::dispatched(uploader);
    uploader.cancel();
    check(!AetherSDR::FirmwareUploaderTestAccess::beginAllowed(uploader, data)
              && !uploader.isUploading()
              && finished.back().message.contains(QStringLiteral("reconnect")),
          "a dispatched upload cannot be retried on the same command session");
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, first, QStringLiteral("file update"), {{QStringLiteral("failed"), QStringLiteral("1")}});
    AetherSDR::FirmwareUploaderTestAccess::connectionChanged(uploader, true);
    check(!AetherSDR::FirmwareUploaderTestAccess::beginAllowed(uploader, data),
          "a same-session connected notification cannot bypass the retry barrier");
    AetherSDR::FirmwareUploaderTestAccess::connectionChanged(uploader, false);
    check(!AetherSDR::FirmwareUploaderTestAccess::beginAllowed(uploader, data),
          "disconnect alone cannot permit retry before reconnection");
    AetherSDR::FirmwareUploaderTestAccess::connectionChanged(uploader, true);
    check(AetherSDR::FirmwareUploaderTestAccess::beginAllowed(uploader, data),
          "a fresh connection permits an explicitly requested new upload");
    AetherSDR::FirmwareUploaderTestAccess::dispatched(uploader);
    const qsizetype count = finished.size();
    AetherSDR::FirmwareUploaderTestAccess::connectionChanged(uploader, false);
    AetherSDR::FirmwareUploaderTestAccess::connectionChanged(uploader, false);
    check(!uploader.isUploading() && finished.size() == count + 1,
          "command disconnection terminates an active attempt once");
    check(!AetherSDR::FirmwareUploaderTestAccess::beginAllowed(uploader, data),
          "terminal cleanup does not erase the fresh-connection requirement");
}

void checkByteIdentityAndPostDrainState()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    int lastProgress = -1;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::progressChanged,
                     [&lastProgress](int percent, const QString&) { lastProgress = percent; });

    const QByteArray source = nonPeriodicBytes(2 * 65536 + 913);
    QByteArray written;
    QVector<qint64> acceptedSizes{32769, 7, 4099, 31, 65535, 2, 8191};
    qsizetype writeIndex = 0;
    const auto writer = [&written, &acceptedSizes, &writeIndex](const char* data, qint64 requested) {
        const qint64 accepted = qMin(requested, acceptedSizes.at(writeIndex % acceptedSizes.size()));
        ++writeIndex;
        written.append(data, accepted);
        return accepted;
    };
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, source, writer);
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"), {{QStringLiteral("transfer"), QStringLiteral("0.50")}});

    const qint64 firstPending = AetherSDR::FirmwareUploaderTestAccess::pending(uploader);
    const qsizetype writesBeforePartialDrain = writeIndex;
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 1024);
    check(writeIndex == writesBeforePartialDrain,
          "a partial bytesWritten drain does not queue a duplicate chunk");
    check(AetherSDR::FirmwareUploaderTestAccess::pending(uploader) == firstPending - 1024,
          "partial drain retains the accepted remainder as the only in-flight chunk");

    while (uploader.isUploading() && !AetherSDR::FirmwareUploaderTestAccess::waiting(uploader)) {
        const qint64 pending = AetherSDR::FirmwareUploaderTestAccess::pending(uploader);
        check(pending > 0, "each active transfer phase has one accepted chunk to drain");
        if (pending <= 0) {
            break;
        }
        AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, pending);
    }

    check(written == source,
          "partial drains and short write acceptance preserve exact nonperiodic firmware bytes once");
    check(uploader.isUploading() && AetherSDR::FirmwareUploaderTestAccess::waiting(uploader),
          "draining every local byte enters radio-confirmation wait");
    check(lastProgress == 50,
          "local byte drain preserves the last radio-reported transfer percentage");
    check(finished.isEmpty(),
          "a drained upload socket never claims firmware installation success");
}

void checkUploadTimeoutAndModelDisconnect()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });

    const auto timedOutGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 uploadTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, timedOutGeneration, uploadTimeout, QStringLiteral("upload timeout"));
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, timedOutGeneration, uploadTimeout, QStringLiteral("late timeout"));
    check(finished.size() == 1 && !finished.front().success,
          "an inactive upload is bounded and its stale timeout cannot emit twice");

    const auto disconnectedGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(1), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, disconnectedGeneration, 1);
    AetherSDR::FirmwareUploaderTestAccess::modelDisconnected(uploader, disconnectedGeneration);
    check(finished.size() == 2 && !finished.back().success
              && finished.back().message.contains(QStringLiteral("unconfirmed")),
          "radio command-channel disconnect after drain reports an unconfirmed outcome once");
}

void checkReportedCompletePercentageIsNotOverwritten()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    int lastProgress = -1;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::progressChanged,
                     [&lastProgress](int percent, const QString&) { lastProgress = percent; });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(1), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"), {{QStringLiteral("transfer"), QStringLiteral("1.00")}});
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 1);
    check(lastProgress == 100 && uploader.isUploading()
              && AetherSDR::FirmwareUploaderTestAccess::waiting(uploader),
          "reported transfer=1.00 remains visible while installation is still unconfirmed");
}

void checkOverallDeadlineCannotBeExtendedByProgress()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 overallTimeout = AetherSDR::FirmwareUploaderTestAccess::overallTimeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"), {{QStringLiteral("transfer"), QStringLiteral("0.25")}});
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"), {{QStringLiteral("transfer"), QStringLiteral("0.26")}});
    AetherSDR::FirmwareUploaderTestAccess::expireOverall(uploader, generation, overallTimeout);
    AetherSDR::FirmwareUploaderTestAccess::expireOverall(uploader, generation, overallTimeout);
    check(finished.size() == 1 && !finished.front().success
              && finished.front().message.contains(QStringLiteral("10-minute")),
          "radio progress cannot extend the hard client operation deadline");
}

void checkRadioStatusValidationAndFailure()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QVector<QString> progress;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::progressChanged,
                     [&progress](int, const QString& message) { progress.append(message); });

    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(3), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 3);
    const qsizetype progressBeforeMalformed = progress.size();
    for (const QString& transfer : {QStringLiteral("nan"), QStringLiteral("-0.01"),
                                    QStringLiteral("1.01"), QStringLiteral("not-a-number")}) {
        AetherSDR::FirmwareUploaderTestAccess::status(
            uploader, generation, QStringLiteral("file update"), {{QStringLiteral("transfer"), transfer}});
    }
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"), {{QStringLiteral("failed"), QStringLiteral("2")}});
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("0")}, {QStringLiteral("reason"), QStringLiteral("none")}});
    check(progress.size() == progressBeforeMalformed && finished.isEmpty(),
          "malformed transfer and failed values are ignored without inventing a result");

    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("1")},
         {QStringLiteral("reason"), QStringLiteral("signature validation failed")}});
    check(finished.size() == 1 && !finished.front().success
              && finished.front().message.contains(QStringLiteral("signature validation failed")),
          "radio failed=1 terminates once and preserves the reported reason");
}

void checkDisconnectTimeoutAndStaleCallbacks()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });

    const auto disconnectedGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 disconnectedTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::disconnected(uploader, disconnectedGeneration);
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, disconnectedGeneration, disconnectedTimeout, QStringLiteral("late timeout"));
    check(finished.size() == 1 && !finished.front().success,
          "disconnect before drain is an unconfirmed failure and emits one terminal result");

    const auto cancelledGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 cancelledTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    uploader.cancel();
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, cancelledGeneration, 8);
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, cancelledGeneration, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("1")}, {QStringLiteral("reason"), QStringLiteral("late")}});
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, cancelledGeneration, cancelledTimeout, QStringLiteral("late timeout"));
    check(finished.size() == 2 && !finished.back().success,
          "cancel invalidates late byte, status, and timeout callbacks exactly once");

    const auto waitingGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(1), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, waitingGeneration, 1);
    const quint64 confirmationTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::disconnected(uploader, waitingGeneration);
    check(finished.size() == 2 && uploader.isUploading(),
          "disconnect after drain remains unconfirmed rather than reporting install success");
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, waitingGeneration, confirmationTimeout, QStringLiteral("confirmation timed out"));
    check(finished.size() == 3 && !finished.back().success,
          "post-drain confirmation wait is bounded and never succeeds speculatively");
}

void checkRejectedUploadResponse()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::begin(uploader, nonPeriodicBytes(4));
    AetherSDR::FirmwareUploaderTestAccess::reject(uploader, generation, 0x15);
    check(finished.size() == 1 && !finished.front().success,
          "a rejected upload-port response terminates without starting a transfer");
}

void checkProgressReentrancyCannotMutateReplacementAttempt()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    bool restarted = false;
    quint64 replacementGeneration = 0;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](bool success, const QString& message) {
                         finished.append({success, message});
                     });
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::progressChanged,
                     [&uploader, &restarted, &replacementGeneration](int, const QString& message) {
                         if (restarted || !message.startsWith(QStringLiteral("Uploading..."))) {
                             return;
                         }
                         restarted = true;
                         uploader.cancel();
                         replacementGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
                             uploader, nonPeriodicBytes(5),
                             [](const char*, qint64 requested) { return requested; });
                     });

    const auto oldGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, oldGeneration, 8);
    check(restarted && uploader.isUploading()
              && !AetherSDR::FirmwareUploaderTestAccess::waiting(uploader)
              && AetherSDR::FirmwareUploaderTestAccess::pending(uploader) == 5,
          "progress callback cancellation cannot let an old acknowledgement mutate its replacement");
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, replacementGeneration, 5);
    check(AetherSDR::FirmwareUploaderTestAccess::waiting(uploader)
              && finished.size() == 1 && !finished.front().success,
          "replacement attempt remains independent after reentrant cancellation");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    checkRetryRequiresFreshConnection();
    checkByteIdentityAndPostDrainState();
    checkRadioStatusValidationAndFailure();
    checkUploadTimeoutAndModelDisconnect();
    checkReportedCompletePercentageIsNotOverwritten();
    checkOverallDeadlineCannotBeExtendedByProgress();
    checkDisconnectTimeoutAndStaleCallbacks();
    checkRejectedUploadResponse();
    checkProgressReentrancyCannotMutateReplacementAttempt();
    return g_failures == 0 ? 0 : 1;
}
