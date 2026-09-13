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

    static int overallTimeoutMs(const FirmwareUploader& uploader, qint64 bytes)
    {
        return uploader.overallTimeoutMsFor(bytes);
    }
    static bool barrierActive(const FirmwareUploader& uploader)
    {
        return uploader.retryBarrierActive();
    }
    static void expireRadioProgressFreshness(FirmwareUploader& uploader)
    {
        uploader.m_radioProgressStaleMs = 0;
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

using Outcome = AetherSDR::FirmwareUploader::Outcome;

struct FinishedEvent {
    Outcome outcome;
    QString message;
};

void checkRetryRequiresFreshConnection()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
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
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
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
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
                     });

    const auto timedOutGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 uploadTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, timedOutGeneration, uploadTimeout, QStringLiteral("upload timeout"));
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, timedOutGeneration, uploadTimeout, QStringLiteral("late timeout"));
    check(finished.size() == 1 && finished.front().outcome == Outcome::Failed,
          "an inactive upload is bounded and its stale timeout cannot emit twice");

    const auto disconnectedGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(1), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, disconnectedGeneration, 1);
    AetherSDR::FirmwareUploaderTestAccess::modelDisconnected(uploader, disconnectedGeneration);
    check(finished.size() == 2 && finished.back().outcome == Outcome::Unconfirmed
              && finished.back().message.contains(QStringLiteral("rebooting")),
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
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
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
    check(finished.size() == 1 && finished.front().outcome == Outcome::Failed
              && finished.front().message.contains(QStringLiteral("operation limit")),
          "radio progress cannot extend the hard client operation deadline");
}

void checkRadioStatusValidationAndFailure()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QVector<QString> progress;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
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
    check(progress.size() == progressBeforeMalformed && finished.isEmpty(),
          "malformed transfer and failed values are ignored without inventing a result");

    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("1")},
         {QStringLiteral("reason"), QStringLiteral("signature validation failed")}});
    check(finished.size() == 1 && finished.front().outcome == Outcome::Failed
              && finished.front().message.contains(QStringLiteral("signature validation failed")),
          "radio failed=1 terminates once and preserves the reported reason");
}

void checkDisconnectTimeoutAndStaleCallbacks()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
                     });

    const auto disconnectedGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    const quint64 disconnectedTimeout = AetherSDR::FirmwareUploaderTestAccess::timeoutToken(uploader);
    AetherSDR::FirmwareUploaderTestAccess::disconnected(uploader, disconnectedGeneration);
    AetherSDR::FirmwareUploaderTestAccess::expire(
        uploader, disconnectedGeneration, disconnectedTimeout, QStringLiteral("late timeout"));
    check(finished.size() == 1 && finished.front().outcome == Outcome::Failed,
          "disconnect before drain is a failure and emits one terminal result");

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
    check(finished.size() == 2 && finished.back().outcome == Outcome::Failed,
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
    check(finished.size() == 3 && finished.back().outcome == Outcome::Unconfirmed,
          "post-drain confirmation wait is bounded and resolves unconfirmed, never successful");
}

void checkRejectedUploadResponse()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
                     });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::begin(uploader, nonPeriodicBytes(4));
    AetherSDR::FirmwareUploaderTestAccess::reject(uploader, generation, 0x15);
    check(finished.size() == 1 && finished.front().outcome == Outcome::Failed,
          "a rejected upload-port response terminates without starting a transfer");
}

void checkProgressReentrancyCannotMutateReplacementAttempt()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    bool restarted = false;
    quint64 replacementGeneration = 0;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
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
              && finished.size() == 1 && finished.front().outcome == Outcome::Failed,
          "replacement attempt remains independent after reentrant cancellation");
}


// #5572 review: failed=0 is the radio's own confirmation, not noise. FlexLib
// treats any parseable `failed` as the terminal word on an update and drops the
// command channel on it (Radio.cs:12612-12631), so dropping failed=0 would
// discard the one signal that can distinguish a real install from a silent one.
// #5572 review: m_radioProgressSeen used to latch forever, so if the radio's
// status stream stalled while TCP kept draining, the bar froze at the last
// reported percentage for the rest of the transfer — and acknowledgeBytes
// re-arms the inactivity timer, so nothing else would have noticed.
void checkStaleRadioProgressFallsBackToLocalBytes()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    int lastProgress = -1;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::progressChanged,
                     [&lastProgress](int percent, const QString&) { lastProgress = percent; });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(100), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"),
        {{QStringLiteral("transfer"), QStringLiteral("0.10")}});
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 40);
    check(lastProgress == 10,
          "fresh radio progress supersedes the local byte counter");

    AetherSDR::FirmwareUploaderTestAccess::expireRadioProgressFreshness(uploader);
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 40);
    check(lastProgress == 80,
          "a stalled radio status stream falls back to local byte progress");
}

void checkRadioConfirmationSucceedsAndReleasesBarrier()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    QVector<FinishedEvent> finished;
    QObject::connect(&uploader, &AetherSDR::FirmwareUploader::finished,
                     [&finished](Outcome outcome, const QString& message) {
                         finished.append({outcome, message});
                     });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        uploader, nonPeriodicBytes(4), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::dispatched(uploader);
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(uploader, generation, 4);
    check(AetherSDR::FirmwareUploaderTestAccess::waiting(uploader) && finished.isEmpty(),
          "a drained socket still waits for the radio rather than claiming success");

    AetherSDR::FirmwareUploaderTestAccess::status(
        uploader, generation, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("0")}});
    check(finished.size() == 1 && finished.front().outcome == Outcome::Succeeded,
          "radio failed=0 confirms the install and is the only path to Succeeded");
    check(!AetherSDR::FirmwareUploaderTestAccess::barrierActive(uploader),
          "a radio-settled outcome leaves nothing pending, so the retry barrier clears");
}

// The barrier exists for attempts whose outcome the radio never reported. An
// outcome the radio DID report must not keep it armed, and an ambiguous one must.
void checkBarrierReleasedOnlyByRadioSettledOutcomes()
{
    AetherSDR::FirmwareUploader rejected(nullptr);
    QVector<FinishedEvent> rejectedFinished;
    QObject::connect(&rejected, &AetherSDR::FirmwareUploader::finished,
                     [&rejectedFinished](Outcome outcome, const QString& message) {
                         rejectedFinished.append({outcome, message});
                     });
    const auto generation = AetherSDR::FirmwareUploaderTestAccess::start(
        rejected, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::dispatched(rejected);
    AetherSDR::FirmwareUploaderTestAccess::status(
        rejected, generation, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("1")},
         {QStringLiteral("reason"), QStringLiteral("bad-signature")}});
    check(rejectedFinished.size() == 1 && rejectedFinished.front().outcome == Outcome::Failed
              && !AetherSDR::FirmwareUploaderTestAccess::barrierActive(rejected),
          "a radio-reported rejection is unambiguous and releases the barrier");

    // A local drain does not establish installation. Closing the dialog at
    // this phase must preserve the unknown outcome and refuse same-session retry.
    AetherSDR::FirmwareUploader cancelled(nullptr);
    QVector<FinishedEvent> cancelledFinished;
    QObject::connect(&cancelled, &AetherSDR::FirmwareUploader::finished,
                     [&cancelledFinished](Outcome outcome, const QString& message) {
                         cancelledFinished.append({outcome, message});
                     });
    const auto cancelledGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        cancelled, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::dispatched(cancelled);
    AetherSDR::FirmwareUploaderTestAccess::acknowledge(cancelled, cancelledGeneration, 8);
    check(cancelled.phase() == AetherSDR::FirmwareUploader::Phase::AwaitingConfirmation,
          "the cancellation fixture reaches the post-drain confirmation phase");
    cancelled.cancel();
    cancelled.cancel();
    AetherSDR::FirmwareUploaderTestAccess::status(
        cancelled, cancelledGeneration, QStringLiteral("file update"),
        {{QStringLiteral("failed"), QStringLiteral("0")}});
    check(cancelledFinished.size() == 1
              && cancelledFinished.front().outcome == Outcome::Unconfirmed
              && cancelledFinished.front().message.contains(QStringLiteral("unconfirmed"))
              && cancelled.phase() == AetherSDR::FirmwareUploader::Phase::Idle,
          "post-drain cancellation emits one unconfirmed result, even with late callbacks");
    check(AetherSDR::FirmwareUploaderTestAccess::barrierActive(cancelled),
          "post-drain cancellation keeps the retry barrier armed");

    AetherSDR::FirmwareUploader stalled(nullptr);
    const auto stalledGeneration = AetherSDR::FirmwareUploaderTestAccess::start(
        stalled, nonPeriodicBytes(8), [](const char*, qint64 requested) { return requested; });
    AetherSDR::FirmwareUploaderTestAccess::dispatched(stalled);
    AetherSDR::FirmwareUploaderTestAccess::expire(
        stalled, stalledGeneration,
        AetherSDR::FirmwareUploaderTestAccess::timeoutToken(stalled),
        QStringLiteral("upload timeout"));
    check(AetherSDR::FirmwareUploaderTestAccess::barrierActive(stalled),
          "a timeout after dispatch leaves the outcome unobservable, so the barrier holds");
}

// #5572 review: a fixed ten-minute ceiling needed ~644 KB/s to pass the
// reporter's own 386 MB image, which a SmartLink uplink does not owe us.
void checkOverallDeadlineScalesWithImageSize()
{
    AetherSDR::FirmwareUploader uploader(nullptr);
    const int small = AetherSDR::FirmwareUploaderTestAccess::overallTimeoutMs(uploader, 1024);
    check(small == 10 * 60 * 1000,
          "a small image keeps the ten-minute floor");

    const qint64 reportedImage = 386282416;
    const int large =
        AetherSDR::FirmwareUploaderTestAccess::overallTimeoutMs(uploader, reportedImage);
    check(large > small, "a large image is given more than the floor");
    // It must survive a genuinely slow uplink: the budget has to exceed the time
    // the image takes at a rate the floor would have failed.
    check(static_cast<qint64>(large) > (reportedImage * 1000LL) / (128LL * 1024LL),
          "#5572's 386MB image survives a 128 KB/s uplink within the deadline");
    check(AetherSDR::FirmwareUploaderTestAccess::overallTimeoutMs(
              uploader, 500LL * 1024LL * 1024LL) <= 2 * 60 * 60 * 1000,
          "the deadline is still capped so a wedged transfer cannot run forever");
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
    checkStaleRadioProgressFallsBackToLocalBytes();
    checkRadioConfirmationSucceedsAndReleasesBarrier();
    checkBarrierReleasedOnlyByRadioSettledOutcomes();
    checkOverallDeadlineScalesWithImageSize();
    return g_failures == 0 ? 0 : 1;
}
