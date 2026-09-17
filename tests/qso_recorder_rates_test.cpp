// RFC #5468 A3: exercise the production recorder through real temporary files.
// No sockets, audio devices or radio/TX transport are opened by this target.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/PcmFrame.h"
#include "core/QsoRecorder.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace AetherSDR {
class QsoRecorderRatesTestAccess {
public:
    static void setBeforeWriteHook(QsoRecorder& recorder, std::function<void()> hook)
    {
        recorder.m_beforePcmWriteForTest = std::move(hook);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void configure(QsoRecorder& recorder, const QString& directory)
{
    recorder.setRecordingDir(directory);
    recorder.setAutoRecordEnabled(false);
    recorder.setIncludeDate(false);
    recorder.setIncludeTime(false);
    recorder.setIncludeFrequency(false);
    recorder.setIncludeMode(false);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "open test-owned recording for independent inspection");
    return file.readAll();
}

void verifyWav(const QByteArray& wav, int rate, qint64 frames)
{
    check(wav.size() == 44 + frames * 4, "physical WAV length equals captured stereo frames");
    if (wav.size() < 44) {
        return;
    }
    check(wav.first(4) == "RIFF" && wav.mid(8, 8) == "WAVEfmt "
              && wav.mid(36, 4) == "data", "recording uses canonical PCM WAV chunk identifiers");
    check(qFromLittleEndian<quint32>(wav.constData() + 4) == frames * 4 + 36
              && qFromLittleEndian<quint32>(wav.constData() + 40) == frames * 4,
          "RIFF and data lengths describe actual accepted bytes");
    check(qFromLittleEndian<quint32>(wav.constData() + 24) == rate
              && qFromLittleEndian<quint32>(wav.constData() + 28) == rate * 4,
          "WAV sample and byte rates match immutable recording rate");
    check(qFromLittleEndian<quint32>(wav.constData() + 16) == 16
              && qFromLittleEndian<quint16>(wav.constData() + 20) == 1
              && qFromLittleEndian<quint16>(wav.constData() + 22) == 2
              && qFromLittleEndian<quint16>(wav.constData() + 32) == 4
              && qFromLittleEndian<quint16>(wav.constData() + 34) == 16,
          "recording is PCM16 little-endian stereo with complete frame alignment");
}

QVector<float> stereo(int frames, float left, float right)
{
    QVector<float> samples(frames * 2);
    for (int frame = 0; frame < frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return samples;
}

QByteArray floats(const QVector<float>& samples)
{
    return QByteArray(reinterpret_cast<const char*>(samples.constData()),
                      samples.size() * static_cast<qsizetype>(sizeof(float)));
}

QByteArray pcm16(int frames, qint16 left, qint16 right)
{
    QVector<qint16> samples(frames * 2);
    for (int frame = 0; frame < frames; ++frame) {
        samples[frame * 2] = left;
        samples[frame * 2 + 1] = right;
    }
    return QByteArray(reinterpret_cast<const char*>(samples.constData()),
                      samples.size() * static_cast<qsizetype>(sizeof(qint16)));
}

PcmFrame produce(PcmProducer& producer, QVector<float> samples,
                 std::optional<quint64> firstSample = std::nullopt, bool discontinuity = false)
{
    const std::optional<PcmFrame> frame = producer.produce(std::move(samples), firstSample, discontinuity);
    check(frame.has_value(), "test fixture produces a valid owning PCM frame");
    return frame.value_or(PcmFrame{});
}

qint16 sampleAt(const QByteArray& wav, int frame, int channel)
{
    const qsizetype offset = 44 + frame * 4 + channel * 2;
    if (offset + 2 > wav.size()) {
        return 0;
    }
    return qFromLittleEndian<qint16>(wav.constData() + offset);
}

void verifyIsolatedSegment(const QByteArray& wav, int firstFrame, int frames,
                           int activeChannel, const char* message)
{
    int peak = 0;
    bool silentOtherChannel = true;
    for (int frame = firstFrame; frame < firstFrame + frames; ++frame) {
        peak = std::max(peak, std::abs(static_cast<int>(sampleAt(wav, frame, activeChannel))));
        silentOtherChannel = silentOtherChannel && sampleAt(wav, frame, 1 - activeChannel) == 0;
    }
    check(peak > 2000 && silentOtherChannel, message);
    check(std::abs(static_cast<int>(sampleAt(wav, firstFrame + frames - 1, activeChannel))) > 1000,
          "normal source transition retains the finite segment's last output frame");
}


void finalizedStopDuration()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer producer;
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}),
          "start 48k metadata source");
    const auto frame = producer.produce({0.25f, -0.25f});
    recorder.feedRxFrame(*frame);
    recorder.startRecording();
    recorder.onMoxChanged(true);
    recorder.feedTxAudio(QByteArray(24000 * 4, '\0'));
    const QString path = recorder.recordingFilePath();
    check(recorder.stopRecording() == 1, "stop result includes the finite 24-to-48 tail");
    verifyWav(readFile(path), 48000, 48000);
    check(recorder.stopRecording() == 0, "stopping an inactive recorder returns zero");
}

void knownProducerRateAndDuration()
{
    // Replacing typed ingress with legacyStereo24(), hardcoding the header rate,
    // or deriving duration from wall time must each fail this production test.
    for (int rate : {24000, 48000}) {
        QTemporaryDir directory;
        check(directory.isValid(), "create recording directory");
        QsoRecorder recorder;
        configure(recorder, directory.path());
        PcmProducer producer;
        check(producer.start(PcmPurpose::Speaker, -1, {rate, PcmLayout::Stereo}),
              "start normalized typed source");
        const std::optional<PcmFrame> observed = producer.produce({0.25f, -0.5f});
        check(observed.has_value(), "produce pre-start metadata observation");
        if (!observed) {
            continue;
        }
        recorder.feedRxFrame(*observed);
        check(!recorder.isRecording() && recorder.recordingFilePath().isEmpty(),
              "observing source metadata does not start recording");
        int stoppedDuration = -1;
        QObject::connect(&recorder, &QsoRecorder::recordingStopped, &recorder,
                         [&stoppedDuration](const QString&, int duration) { stoppedDuration = duration; });
        recorder.startRecording();
        check(recorder.isRecording(), "manual recording starts");
        const QString path = recorder.recordingFilePath();
        const QByteArray initial = readFile(path);
        check(initial.size() == 44 && qFromLittleEndian<quint32>(initial.constData() + 24) == rate,
              "initial header fixes observed producer rate before accepting samples");
        recorder.feedRxFrame(*observed);
        const std::optional<PcmFrame> frame = producer.produce(QVector<float>(rate * 2, 0.25f));
        check(frame.has_value(), "produce one second of typed audio");
        if (frame) {
            recorder.feedRxFrame(*frame);
        }
        check(recorder.recordingDurationSecs() == 1,
              "live duration follows accepted sample frames rather than elapsed wall time");
        recorder.stopRecording();
        check(stoppedDuration == 1, "stopped duration represents one second of captured audio");
        verifyWav(readFile(path), rate, rate);
    }
}

void legacyAndMonoQuantization()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    recorder.startRecording();
    const QString legacyPath = recorder.recordingFilePath();
    recorder.feedRxAudio(floats({-2.0f, 2.0f, -0.5f, 0.5f, 0.0f, 1.0f, -1.0f, 0.0f}));
    recorder.stopRecording();
    const QByteArray legacy = readFile(legacyPath);
    verifyWav(legacy, 24000, 4);
    const qint16 expected[] = {-32767, 32767, -16383, 16383, 0, 32767, -32767, 0};
    for (int sample = 0; sample < 8; ++sample) {
        check(sampleAt(legacy, sample / 2, sample % 2) == expected[sample],
              "legacy stereo float input retains clipping and independent L/R quantization");
    }

    for (int rate : {24000, 48000}) {
        PcmProducer producer;
        check(producer.start(PcmPurpose::Speaker, -1, {rate, PcmLayout::Mono}), "start mono producer");
        recorder.feedRxFrame(produce(producer, {0.0f}));
        recorder.startRecording();
        const QString path = recorder.recordingFilePath();
        recorder.feedRxFrame(produce(producer, {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f}));
        recorder.stopRecording();
        const QByteArray wav = readFile(path);
        verifyWav(wav, rate, 5);
        const qint16 monoExpected[] = {-32767, -16383, 0, 16383, 32767};
        for (int frame = 0; frame < 5; ++frame) {
            check(sampleAt(wav, frame, 0) == monoExpected[frame]
                      && sampleAt(wav, frame, 1) == monoExpected[frame],
                  "typed mono clips and duplicates into both WAV channels");
        }
    }
}

void earlyAndTxFirstRateSelection()
{
    for (bool knownBeforeStart : {false, true}) {
        QTemporaryDir directory;
        QsoRecorder recorder;
        configure(recorder, directory.path());
        PcmProducer producer;
        check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start TX-first RX producer");
        const PcmFrame observed = produce(producer, stereo(1, 0.75f, -0.75f));
        if (knownBeforeStart) {
            recorder.feedRxFrame(observed);
        }
        recorder.onMoxChanged(true);
        recorder.startRecording();
        const QString firstPath = recorder.recordingFilePath();
        recorder.feedTxAudio(pcm16(31, 8192, -16384));
        // A TX-gated block updates metadata for the next run, without entering
        // this run or allowing the same queued block to enter a later one.
        recorder.feedRxFrame(observed);
        recorder.stopRecording();
        const QByteArray first = readFile(firstPath);
        verifyWav(first, knownBeforeStart ? 48000 : 24000, knownBeforeStart ? 62 : 31);
        recorder.onMoxChanged(false);
        recorder.startRecording();
        const QString secondPath = recorder.recordingFilePath();
        recorder.feedRxFrame(observed);
        recorder.feedRxFrame(produce(producer, stereo(8, 0.25f, -0.5f)));
        recorder.stopRecording();
        verifyWav(readFile(secondPath), 48000, 8);
        check(firstPath != secondPath && readFile(firstPath) == first,
              "later rate selection allocates a new file and preserves the earlier recording");
    }

    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer ignoredSlice;
    PcmProducer ignoredAuxiliary;
    check(ignoredSlice.start(PcmPurpose::Slice, 0, {48000, PcmLayout::Stereo}), "start slice-only source");
    check(ignoredAuxiliary.start(PcmPurpose::Auxiliary, -1, {48000, PcmLayout::Stereo}), "start auxiliary source");
    recorder.feedRxFrame(produce(ignoredSlice, stereo(1, 0.5f, 0.5f)));
    recorder.feedRxFrame(produce(ignoredAuxiliary, stereo(1, 0.5f, 0.5f)));
    recorder.startRecording();
    const QString path = recorder.recordingFilePath();
    PcmProducer lateSpeaker;
    check(lateSpeaker.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start late speaker source");
    recorder.feedRxFrame(produce(lateSpeaker, stereo(17, 0.25f, -0.5f)));
    recorder.stopRecording();
    verifyWav(readFile(path), 24000, 9);
}

void replacementReconnectAndReplay()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer firstProducer;
    PcmProducer replacement;
    check(firstProducer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start first source");
    check(replacement.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start competing source");
    recorder.feedRxFrame(produce(firstProducer, stereo(1, 0.0f, 0.0f)));
    recorder.startRecording();
    const QString firstPath = recorder.recordingFilePath();
    const PcmFrame first = produce(firstProducer, stereo(8, 0.25f, 0.0f));
    const PcmFrame competing = produce(replacement, stereo(11, 0.0f, -0.5f));
    recorder.feedRxFrame(first);
    recorder.feedRxFrame(first); // duplicate while current
    recorder.feedRxFrame(competing); // rejected without consuming its cursor
    firstProducer.invalidate();
    recorder.feedRxFrame(first); // revoked, not merely a duplicate
    recorder.feedRxFrame(competing);
    recorder.feedRxFrame(competing);
    recorder.stopRecording();
    const QByteArray firstWav = readFile(firstPath);
    verifyWav(firstWav, 48000, 19);
    verifyIsolatedSegment(firstWav, 0, 8, 0, "first source retains its accepted channel history");
    verifyIsolatedSegment(firstWav, 8, 11, 1, "replacement is accepted after revocation without competing-source cursor poisoning");

    recorder.startRecording();
    const QString secondPath = recorder.recordingFilePath();
    recorder.feedRxFrame(competing); // replay cursor persists across files
    recorder.feedRxFrame(produce(replacement, stereo(5, 0.0f, 0.25f)));
    replacement.invalidate();
    check(replacement.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "reconnect producer with fresh session");
    recorder.feedRxFrame(competing);
    recorder.feedRxFrame(produce(replacement, stereo(7, -0.25f, 0.0f)));
    recorder.stopRecording();
    verifyWav(readFile(secondPath), 48000, 12);
    check(readFile(firstPath) == firstWav, "reconnect and stop/start do not rewrite accepted historical bytes");

    replacement.invalidate();
    recorder.startRecording();
    const QString thirdPath = recorder.recordingFilePath();
    recorder.feedRxAudio(floats(stereo(3, 0.25f, 0.25f)));
    recorder.stopRecording();
    verifyWav(readFile(thirdPath), 24000, 3);
}

void rateEpochChangesAndForwardGap()
{
    for (int fileRate : {24000, 48000}) {
        QTemporaryDir directory;
        QsoRecorder recorder;
        configure(recorder, directory.path());
        PcmProducer producer;
        check(producer.start(PcmPurpose::Speaker, -1, {fileRate, PcmLayout::Stereo}),
              "start observed source at the selected file rate");
        recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
        recorder.startRecording();
        const QString path = recorder.recordingFilePath();
        check(producer.setFormat({24000, PcmLayout::Stereo}), "begin epoch cycle with 24 kHz source");
        recorder.feedRxFrame(produce(producer, stereo(17, 0.25f, 0.0f)));
        recorder.onMoxChanged(true); // drain while the preceding epoch is current
        check(producer.setFormat({48000, PcmLayout::Stereo}), "change epoch from 24 to 48 kHz");
        recorder.onMoxChanged(false);
        const PcmFrame middle = produce(producer, stereo(9, 0.0f, -0.5f));
        recorder.feedRxFrame(middle);
        recorder.onMoxChanged(true);
        check(producer.setFormat({24000, PcmLayout::Stereo}), "change epoch from 48 back to 24 kHz");
        recorder.onMoxChanged(false);
        recorder.feedRxFrame(middle);
        recorder.feedRxFrame(produce(producer, stereo(13, -0.25f, 0.0f)));
        recorder.stopRecording();
        const QByteArray wav = readFile(path);
        const int firstFrames = fileRate == 24000 ? 17 : 34;
        const int middleFrames = fileRate == 24000 ? 5 : 9;
        const int lastFrames = fileRate == 24000 ? 13 : 26;
        verifyWav(wav, fileRate, firstFrames + middleFrames + lastFrames);
        verifyIsolatedSegment(wav, 0, firstFrames, 0, "24 kHz segment has independent channel history");
        verifyIsolatedSegment(wav, firstFrames, middleFrames, 1,
                              "48 kHz epoch uses file-rate conversion and fresh L/R history");
        verifyIsolatedSegment(wav, firstFrames + middleFrames, lastFrames, 0,
                              "return to 24 kHz cannot revive the prior epoch's channel history");
    }

    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer producer;
    recorder.startRecording(); // no live observation: immutable legacy 24 kHz
    const QString gapPath = recorder.recordingFilePath();
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start forward-gap source");
    recorder.feedRxFrame(produce(producer, stereo(7, 0.5f, 0.0f)));
    recorder.feedRxFrame(produce(producer, stereo(11, 0.0f, -0.5f), 1000, true));
    recorder.stopRecording();
    const QByteArray gapWav = readFile(gapPath);
    verifyWav(gapWav, 24000, 4 + 6);
    verifyIsolatedSegment(gapWav, 0, 4, 0, "forward gap drains accepted earlier samples without fabricating silence");
    verifyIsolatedSegment(gapWav, 4, 6, 1, "forward gap restarts conversion history at the next received block");
}

void independentRxVoiceAndCwTails()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer producer;
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "select 48 kHz file before over changes");
    recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
    recorder.startRecording();
    const QString path = recorder.recordingFilePath();
    check(producer.setFormat({24000, PcmLayout::Stereo}), "use 24 kHz RX with immutable 48 kHz file");
    recorder.feedTxAudio(pcm16(99, 32767, 32767));
    recorder.feedCwAudio(pcm16(99, 32767, 32767));
    recorder.feedRxFrame(produce(producer, stereo(127, 0.4f, 0.0f)));
    recorder.onMoxChanged(true);
    recorder.feedTxAudio(pcm16(31, 0, 16384));
    recorder.setCwOverActive(true);
    recorder.feedCwAudio(pcm16(17, -16384, 0));
    recorder.onMoxChanged(false); // a break-in element edge cannot end the CW segment
    const PcmFrame gatedRx = produce(producer, stereo(100, 1.0f, 1.0f));
    recorder.feedRxFrame(gatedRx);
    recorder.feedCwAudio(pcm16(15, -16384, 0));
    recorder.setCwOverActive(false);
    recorder.feedRxFrame(gatedRx); // TX observation already consumed it
    recorder.feedRxFrame(produce(producer, stereo(32, 0.0f, -0.25f)));
    recorder.stopRecording();
    const QByteArray wav = readFile(path);
    verifyWav(wav, 48000, 254 + 62 + 64 + 64);
    verifyIsolatedSegment(wav, 0, 254, 0, "RX converter tail stays in its own channel at the voice boundary");
    verifyIsolatedSegment(wav, 254, 62, 1, "voice tail drains before CW without retaining RX history");
    verifyIsolatedSegment(wav, 316, 64, 0, "CW tail survives MOX element gaps and drains before RX");
    verifyIsolatedSegment(wav, 380, 64, 1, "RX resumes after the CW over with fresh conversion history");

    // Both TX ingress methods keep the existing MOX-or-CW-over admission.
    // MainWindow owns microphone selection; recorder does not invent authority.
    recorder.onMoxChanged(true);
    recorder.startRecording();
    const QString unionPath = recorder.recordingFilePath();
    recorder.feedCwAudio(pcm16(3, 8192, 0));
    recorder.setCwOverActive(true);
    recorder.onMoxChanged(false);
    recorder.feedTxAudio(pcm16(5, 0, 8192));
    recorder.stopRecording();
    recorder.setCwOverActive(false);
    verifyWav(readFile(unionPath), 24000, 8);
}

void voiceHistorySurvivesUnrelatedCwEdges()
{
    for (const int rate : {24000, 48000}) {
        const auto capture = [rate](bool initiallyCw, bool changeCw) {
            QTemporaryDir directory;
            QsoRecorder recorder;
            configure(recorder, directory.path());
            PcmProducer producer;
            check(producer.start(PcmPurpose::Speaker, -1, {rate, PcmLayout::Stereo}),
                  "select file rate for continuous voice history");
            recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
            recorder.setCwOverActive(initiallyCw);
            recorder.onMoxChanged(true);
            recorder.startRecording();
            const QString path = recorder.recordingFilePath();
            recorder.feedTxAudio(pcm16(1275, 16384, -16384));
            if (changeCw) {
                recorder.setCwOverActive(!initiallyCw);
            }
            // MOX keeps the same voice source admitted across an unrelated
            // delayed CW event. No CW PCM has taken ownership of this segment.
            recorder.feedTxAudio(pcm16(2731, 16384, -16384));
            recorder.stopRecording();
            const QByteArray wav = readFile(path);
            verifyWav(wav, rate, 4006 * rate / 24000);
            return wav.mid(44);
        };
        const QByteArray continuous = capture(false, false);
        check(capture(true, true) == continuous,
              "late CW close cannot reset a MOX-admitted voice resampler history");
        check(capture(false, true) == continuous,
              "CW opening cannot reset voice history before any CW PCM arrives");
    }
}

// This barrier pauses a real feed after conversion while it holds the writer
// mutex. Bounded waits also let a missing hook fail instead of hanging CTest.
class PendingWrite {
public:
    void pause()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_entered) {
            return;
        }
        m_entered = true;
        m_changed.notify_all();
        if (!m_changed.wait_for(lock, std::chrono::seconds(5), [this] { return m_release; })) {
            m_timedOut = true;
        }
    }

    bool awaitEntry()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, std::chrono::seconds(5), [this] { return m_entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_release = true;
        m_changed.notify_all();
    }

    bool timedOut()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_timedOut;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_entered = false;
    bool m_release = false;
    bool m_timedOut = false;
};

void revocationBeforeWriteAdmission()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer producer;
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start concurrent revoke source");
    recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
    recorder.startRecording();
    const QString path = recorder.recordingFilePath();
    recorder.feedRxFrame(produce(producer, stereo(8, 0.25f, -0.5f)));
    const PcmFrame pending = produce(producer, stereo(1024, 1.0f, 1.0f));
    PendingWrite barrier;
    QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, [&barrier] { barrier.pause(); });
    std::thread feeder([&recorder, &pending] { recorder.feedRxFrame(pending); });
    check(barrier.awaitEntry(), "feed reaches the post-conversion pre-write barrier");
    producer.invalidate();
    barrier.release();
    feeder.join();
    recorder.stopRecording();
    check(!barrier.timedOut(), "revocation ordering completes without a timeout");
    const QByteArray wav = readFile(path);
    verifyWav(wav, 48000, 8);
    for (int frame = 0; frame < 8; ++frame) {
        check(sampleAt(wav, frame, 0) == 8191 && sampleAt(wav, frame, 1) == -16383,
              "revocation drops uncommitted converted output and preserves every previously accepted sample");
    }
    QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, {});

    recorder.startRecording(); // revoked observation cannot choose 48 kHz
    const QString tailPath = recorder.recordingFilePath();
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "reconnect for delayed-tail revocation");
    recorder.feedRxFrame(produce(producer, stereo(7, 0.5f, -0.5f)));
    producer.invalidate();
    recorder.stopRecording();
    verifyWav(readFile(tailPath), 24000, 0);
}

void concurrentFeedAndStop()
{
    for (bool revoke : {false, true}) {
        QTemporaryDir directory;
        QsoRecorder recorder;
        configure(recorder, directory.path());
        PcmProducer producer;
        check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start feed/stop source");
        recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
        recorder.startRecording();
        const QString path = recorder.recordingFilePath();
        recorder.feedRxFrame(produce(producer, stereo(9, 0.25f, -0.5f)));
        const PcmFrame pending = produce(producer, stereo(127, 0.25f, -0.5f));
        PendingWrite barrier;
        QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, [&barrier] { barrier.pause(); });
        std::thread feeder([&recorder, &pending] { recorder.feedRxFrame(pending); });
        check(barrier.awaitEntry(), "concurrent stop begins with a feed paused under the write mutex");
        std::mutex stopMutex;
        std::condition_variable stopChanged;
        bool stopRequested = false;
        bool releaseTimedOut = false;
        std::thread releaseFeed([&] {
            std::unique_lock<std::mutex> lock(stopMutex);
            if (!stopChanged.wait_for(lock, std::chrono::seconds(5), [&] { return stopRequested; })) {
                releaseTimedOut = true;
            }
            if (revoke) {
                producer.invalidate();
            }
            barrier.release();
        });
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopRequested = true;
            stopChanged.notify_one();
        }
        // Owner APIs stay on the QCoreApplication thread. The pending feed
        // and optional epoch revocation execute on separate joined threads.
        recorder.stopRecording();
        releaseFeed.join();
        feeder.join();
        check(!releaseTimedOut && !barrier.timedOut(), "feed/stop/revoke synchronization completes within its bound");
        const QByteArray wav = readFile(path);
        verifyWav(wav, 48000, revoke ? 9 : 136);
        recorder.feedRxFrame(pending);
        check(readFile(path) == wav, "late feed after finalization cannot append or replace file bytes");
        QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, {});
    }
}

void concurrentDuplicateDelivery()
{
    QTemporaryDir directory;
    QsoRecorder recorder;
    configure(recorder, directory.path());
    PcmProducer producer;
    check(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}), "start simultaneous delivery source");
    recorder.feedRxFrame(produce(producer, stereo(1, 0.0f, 0.0f)));
    recorder.startRecording();
    const QString path = recorder.recordingFilePath();
    const PcmFrame shared = produce(producer, stereo(1024, 0.25f, -0.5f));
    PendingWrite barrier;
    QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, [&barrier] { barrier.pause(); });
    std::thread firstFeed([&recorder, &shared] { recorder.feedRxFrame(shared); });
    check(barrier.awaitEntry(), "first concurrent duplicate pauses inside serialized write admission");
    std::mutex secondMutex;
    std::condition_variable secondChanged;
    bool secondStarted = false;
    std::thread secondFeed([&] {
        {
            std::lock_guard<std::mutex> lock(secondMutex);
            secondStarted = true;
            secondChanged.notify_one();
        }
        recorder.feedRxFrame(shared);
    });
    {
        std::unique_lock<std::mutex> lock(secondMutex);
        check(secondChanged.wait_for(lock, std::chrono::seconds(5), [&] { return secondStarted; }),
              "second delivery is requested while first delivery holds the write mutex");
    }
    barrier.release();
    firstFeed.join();
    secondFeed.join();
    check(!barrier.timedOut(),
          "both feed threads complete with one shared replay cursor");
    QsoRecorderRatesTestAccess::setBeforeWriteHook(recorder, {});
    recorder.stopRecording();
    verifyWav(readFile(path), 48000, 1024);
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-qso-recorder-rates"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("RecordingMode"), QStringLiteral("Client"));
    AppSettings::instance().setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));
    AppSettings::instance().save();
    finalizedStopDuration();
    knownProducerRateAndDuration();
    legacyAndMonoQuantization();
    earlyAndTxFirstRateSelection();
    replacementReconnectAndReplay();
    rateEpochChangesAndForwardGap();
    independentRxVoiceAndCwTails();
    voiceHistorySurvivesUnrelatedCwEdges();
    revocationBeforeWriteAdmission();
    concurrentFeedAndStop();
    concurrentDuplicateDelivery();
    std::printf("qso_recorder_rates_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
