// RFC #5468 A3: exercise production WAV preparation and playback lifecycle
// after format negotiation. Sink start/disposal are injected; this executable
// never enumerates an audio device, opens QAudioSink, or accesses a radio.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecorder.h"
#include "core/QsoWavPlayback.h"

#include <QAudioFormat>
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

using namespace AetherSDR;

namespace AetherSDR {

class QsoRecorderPlaybackTestAccess {
public:
    static void setPath(QsoRecorder& recorder, const QString& path)
    {
        recorder.m_lastRecordingPath = path;
    }

    static void installSink(
        QsoRecorder& recorder,
        std::function<QAudio::Error(QIODevice&, const QAudioFormat&)> start,
        std::function<void(bool)> release)
    {
        recorder.m_startPlaybackSinkForTest = std::move(start);
        recorder.m_releasePlaybackSinkForTest = std::move(release);
    }

    static void start(QsoRecorder& recorder, const QAudioFormat& format)
    {
        recorder.startPlaybackWithFormat(QAudioDevice{}, format);
    }

    static void sinkState(QsoRecorder& recorder, QAudio::State state)
    {
        recorder.onPlaybackSinkState(state);
    }

    static bool bufferIsOpen(const QsoRecorder& recorder)
    {
        return recorder.m_playBuffer.isOpen();
    }

    static void failNextFileWrite(QsoRecorder& recorder)
    {
        recorder.m_writeForTest = [](QFile&, const char*, qint64) {
            return qint64{-1};
        };
    }
};

} // namespace AetherSDR

namespace {

int g_failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++g_failures; \
    } \
} while (0)

QAudioFormat sinkFormat(int rate, int channels, QAudioFormat::SampleFormat encoding)
{
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(channels);
    format.setSampleFormat(encoding);
    return format;
}

void appendChunk(QByteArray& file, const char* id, const QByteArray& body)
{
    file.append(id, 4);
    char size[4];
    qToLittleEndian<quint32>(static_cast<quint32>(body.size()), size);
    file.append(size, 4);
    file.append(body);
    if ((body.size() & 1) != 0) {
        file.append('\0');
    }
}

// A standard PCM16 RIFF file with odd-sized unknown chunks both before and
// after its PCM. This ensures production playback must parse data boundaries.
QString writeFixture(const QString& directory, int rate = 48000, int channels = 2)
{
    QByteArray format(16, '\0');
    qToLittleEndian<quint16>(1, format.data());
    qToLittleEndian<quint16>(channels, format.data() + 2);
    qToLittleEndian<quint32>(rate, format.data() + 4);
    qToLittleEndian<quint32>(rate * channels * 2, format.data() + 8);
    qToLittleEndian<quint16>(channels * 2, format.data() + 12);
    qToLittleEndian<quint16>(16, format.data() + 14);

    const int frames = rate / 10;
    QByteArray pcm(frames * channels * 2, Qt::Uninitialized);
    for (int frame = 0; frame < frames; ++frame) {
        qToLittleEndian<qint16>(8192, pcm.data() + frame * channels * 2);
        if (channels == 2) {
            qToLittleEndian<qint16>(-16384, pcm.data() + frame * channels * 2 + 2);
        }
    }
    QByteArray bytes("RIFF\0\0\0\0WAVE", 12);
    appendChunk(bytes, "JUNK", QByteArray("abc", 3));
    appendChunk(bytes, "fmt ", format);
    appendChunk(bytes, "data", pcm);
    appendChunk(bytes, "LIST", QByteArray("metadata!", 9));
    qToLittleEndian<quint32>(static_cast<quint32>(bytes.size() - 8), bytes.data() + 4);
    bytes.append("outside RIFF");

    const QString path = directory + QStringLiteral("/playback.wav");
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
    file.close();
    return path;
}

struct SinkObservation {
    QStringList events;
    QByteArray payload;
    QAudioFormat format;
    int starts{0};
    int releases{0};
    QAudio::Error startError{QAudio::NoError};
    bool signalFailureDuringStart{false};
};

void observePlayback(QsoRecorder& recorder, SinkObservation& observation)
{
    QsoRecorderPlaybackTestAccess::installSink(
        recorder,
        [&recorder, &observation](QIODevice& source, const QAudioFormat& format) {
            CHECK(!recorder.isPlaying());
            CHECK(source.isOpen());
            CHECK(source.pos() == 0);
            ++observation.starts;
            observation.events.append(QStringLiteral("sink.start"));
            observation.payload = source.readAll();
            observation.format = format;
            if (observation.signalFailureDuringStart) {
                QsoRecorderPlaybackTestAccess::sinkState(recorder, QAudio::StoppedState);
            }
            return observation.startError;
        },
        [&recorder, &observation](bool stop) {
            CHECK(!recorder.isPlaying());
            CHECK(QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder));
            ++observation.releases;
            observation.events.append(stop ? QStringLiteral("sink.stop")
                                           : QStringLiteral("sink.release"));
            // Native QAudioSink::stop may synchronously emit StoppedState.
            // Its callback must not recurse, unmute twice, or stop a replay.
            QsoRecorderPlaybackTestAccess::sinkState(recorder, QAudio::StoppedState);
        });
    QObject::connect(&recorder, &QsoRecorder::muteRxRequested, &recorder,
                     [&recorder, &observation](bool mute) {
                         CHECK(recorder.isPlaying() == mute);
                         CHECK(QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder) == mute);
                         observation.events.append(mute ? QStringLiteral("mute.on")
                                                        : QStringLiteral("mute.off"));
                     });
    QObject::connect(&recorder, &QsoRecorder::playbackStarted, &recorder,
                     [&observation]() { observation.events.append(QStringLiteral("started")); });
    QObject::connect(&recorder, &QsoRecorder::playbackStopped, &recorder,
                     [&observation]() { observation.events.append(QStringLiteral("stopped")); });
}

double nativeSample(const QByteArray& pcm, qsizetype index, QAudioFormat::SampleFormat format)
{
    if (format == QAudioFormat::Float) {
        float sample = 0.0f;
        std::memcpy(&sample, pcm.constData() + index * sizeof(float), sizeof(sample));
        return sample;
    }
    qint16 sample = 0;
    std::memcpy(&sample, pcm.constData() + index * sizeof(qint16), sizeof(sample));
    return sample / 32768.0;
}

void testFileAndSinkFormatMatrix()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    for (const int sourceRate : {24000, 44100, 48000}) {
        for (const int sourceChannels : {1, 2}) {
            const QString path = writeFixture(directory.path(), sourceRate, sourceChannels);
            for (const int outputRate : {24000, 44100, 48000}) {
                for (const int outputChannels : {1, 2}) {
                    for (const QAudioFormat::SampleFormat encoding :
                         {QAudioFormat::Int16, QAudioFormat::Float}) {
                        SinkObservation observation;
                        QsoRecorder recorder;
                        observePlayback(recorder, observation);
                        QsoRecorderPlaybackTestAccess::setPath(recorder, path);
                        const QAudioFormat format = sinkFormat(outputRate, outputChannels, encoding);
                        QsoRecorderPlaybackTestAccess::start(recorder, format);
                        CHECK(recorder.isPlaying());
                        CHECK(observation.format == format);
                        CHECK(observation.events == QStringList({"sink.start", "mute.on", "started"}));
                        const int sampleBytes = encoding == QAudioFormat::Float ? 4 : 2;
                        const qsizetype frames = outputRate / 10;
                        const qsizetype expectedBytes = frames * outputChannels * sampleBytes;
                        CHECK(observation.payload.size() == expectedBytes);
                        if (observation.payload.size() == expectedBytes) {
                            for (int channel = 0; channel < outputChannels; ++channel) {
                                const double expected = sourceChannels == 1 ? 0.25
                                    : outputChannels == 1 ? -0.125
                                    : channel == 0 ? 0.25 : -0.5;
                                CHECK(std::abs(nativeSample(observation.payload,
                                    (frames / 2) * outputChannels + channel, encoding) - expected) < 0.001);
                            }
                        }
                        recorder.stopPlayback();
                        CHECK(!recorder.isPlaying());
                        CHECK(observation.releases == 1);
                    }
                }
            }
        }
    }
}

void testCompletionCancellationAndReplay()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SinkObservation observation;
    QsoRecorder recorder;
    observePlayback(recorder, observation);
    QsoRecorderPlaybackTestAccess::setPath(recorder, writeFixture(directory.path()));
    const QAudioFormat format = sinkFormat(44100, 2, QAudioFormat::Float);
    for (const QAudio::State finish : {QAudio::IdleState, QAudio::StoppedState, QAudio::ActiveState}) {
        observation.events.clear();
        const int previousStarts = observation.starts;
        QsoRecorderPlaybackTestAccess::start(recorder, format);
        QsoRecorderPlaybackTestAccess::start(recorder, format);
        CHECK(observation.starts == previousStarts + 1);
        QsoRecorderPlaybackTestAccess::sinkState(recorder, QAudio::ActiveState);
        QsoRecorderPlaybackTestAccess::sinkState(recorder, QAudio::SuspendedState);
        CHECK(recorder.isPlaying());
        if (finish == QAudio::ActiveState) {
            recorder.stopPlayback();
        } else {
            QsoRecorderPlaybackTestAccess::sinkState(recorder, finish);
        }
        recorder.stopPlayback();
        QsoRecorderPlaybackTestAccess::sinkState(recorder, QAudio::StoppedState);
        CHECK(!recorder.isPlaying());
        CHECK(!QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder));
        CHECK(observation.events == QStringList({"sink.start", "mute.on", "started",
                                                 "sink.stop", "mute.off", "stopped"}));
    }
    CHECK(observation.starts == 3);
    CHECK(observation.releases == 3);
}

void testStartFailureLeavesRxLiveAndAllowsRetry()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SinkObservation observation;
    QsoRecorder recorder;
    observePlayback(recorder, observation);
    QsoRecorderPlaybackTestAccess::setPath(recorder, writeFixture(directory.path()));
    const QAudioFormat format = sinkFormat(48000, 1, QAudioFormat::Float);
    observation.startError = QAudio::OpenError;
    observation.signalFailureDuringStart = true;
    QsoRecorderPlaybackTestAccess::start(recorder, format);
    CHECK(!recorder.isPlaying());
    CHECK(!QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder));
    CHECK(observation.events == QStringList({"sink.start", "sink.release"}));
    recorder.stopPlayback();
    CHECK(observation.releases == 1);
    observation.startError = QAudio::NoError;
    observation.signalFailureDuringStart = false;
    QsoRecorderPlaybackTestAccess::start(recorder, format);
    CHECK(recorder.isPlaying());
    CHECK(observation.starts == 2);
    recorder.stopPlayback();
    CHECK(observation.releases == 2);
}

void testRejectedReplayCannotReuseOldPayload()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SinkObservation observation;
    QsoRecorder recorder;
    observePlayback(recorder, observation);
    const QString path = writeFixture(directory.path());
    QsoRecorderPlaybackTestAccess::setPath(recorder, path);
    const QAudioFormat format = sinkFormat(24000, 2, QAudioFormat::Int16);
    QsoRecorderPlaybackTestAccess::start(recorder, format);
    CHECK(recorder.isPlaying());
    recorder.stopPlayback();
    observation.events.clear();
    QsoRecorderPlaybackTestAccess::start(recorder, sinkFormat(48000, 4, QAudioFormat::Float));
    QsoRecorderPlaybackTestAccess::start(recorder, sinkFormat(48000, 2, QAudioFormat::UInt8));
    CHECK(!recorder.isPlaying());
    CHECK(observation.events.isEmpty());
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write("broken", 6) == 6);
    file.close();
    QsoRecorderPlaybackTestAccess::start(recorder, format);
    CHECK(file.remove());
    QsoRecorderPlaybackTestAccess::start(recorder, format);
    CHECK(!recorder.isPlaying());
    CHECK(!QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder));
    CHECK(observation.events.isEmpty());
    CHECK(observation.starts == 1);
}

void testFailedRecordingRetiresPlayback()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SinkObservation observation;
    QsoRecorder recorder;
    observePlayback(recorder, observation);
    recorder.setRecordingDir(directory.path());
    recorder.setIncludeDate(false);
    recorder.setIncludeTime(false);
    recorder.setIncludeFrequency(false);
    recorder.setIncludeMode(false);
    recorder.startRecording();
    CHECK(recorder.isRecording());
    const float samples[2] = {0.25f, -0.5f};
    const QByteArray pcm(reinterpret_cast<const char*>(samples), sizeof(samples));
    recorder.feedRxAudio(pcm);
    recorder.stopRecording();
    CHECK(recorder.hasLastRecording());

    recorder.startRecording();
    CHECK(recorder.isRecording());
    QsoRecorderPlaybackTestAccess::failNextFileWrite(recorder);
    recorder.feedRxAudio(pcm);
    recorder.stopRecording();
    CHECK(!recorder.hasLastRecording());
    CHECK(recorder.recordingFilePath().isEmpty());
    QsoRecorderPlaybackTestAccess::start(recorder, sinkFormat(48000, 2, QAudioFormat::Float));
    recorder.startPlayback(); // Empty-path guard must return before device enumeration.
    CHECK(!recorder.isPlaying());
    CHECK(observation.starts == 0);
    CHECK(observation.events.isEmpty());
}

void testDestructionStopsBeforeBufferTeardown()
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SinkObservation observation;
    auto recorder = std::make_unique<QsoRecorder>();
    observePlayback(*recorder, observation);
    QsoRecorderPlaybackTestAccess::setPath(*recorder, writeFixture(directory.path()));
    QsoRecorderPlaybackTestAccess::start(*recorder, sinkFormat(48000, 2, QAudioFormat::Float));
    CHECK(recorder->isPlaying());
    recorder.reset();
    CHECK(observation.events == QStringList({"sink.start", "mute.on", "started",
                                             "sink.stop", "mute.off", "stopped"}));
    CHECK(observation.releases == 1);
}


// The output budget must reject before sink creation, but never silently.
QString writeOverBudgetFixture(const QString& directory, const QAudioFormat& format)
{
    const qint64 sourceFrames = kQsoPlaybackMaxFrames
        * 24000 / format.sampleRate() + 1;
    const quint32 bytes = static_cast<quint32>(sourceFrames * 4);
    const auto header = QsoRecordingFormat{}.wavHeader(bytes);
    CHECK(header.has_value());
    const QString path = directory + QStringLiteral("/over-budget.wav");
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(*header) == header->size());
    CHECK(file.resize(44 + bytes)); // Sparse: no large allocation or PCM write.
    return path;
}

void testPreparationRefusalReportsAndAllowsRetry()
{
    QTemporaryDir directory;
    for (const QAudioFormat& format : {sinkFormat(24000, 2, QAudioFormat::Int16),
                                      sinkFormat(48000, 2, QAudioFormat::Int16),
                                      sinkFormat(48000, 2, QAudioFormat::Float)}) {
        SinkObservation observation;
        QsoRecorder recorder;
        observePlayback(recorder, observation);
        const QString path = writeOverBudgetFixture(directory.path(), format);
        QsoRecorderPlaybackTestAccess::setPath(recorder, path);
        QStringList errors;
        QObject::connect(&recorder, &QsoRecorder::recordingError, &recorder,
                         [&](const QString& error) {
            CHECK(!recorder.isPlaying());
            CHECK(!QsoRecorderPlaybackTestAccess::bufferIsOpen(recorder));
            CHECK(recorder.hasLastRecording());
            errors.append(error);
            recorder.stopPlayback(); // Refusal notification is reentrant.
        });
        QsoRecorderPlaybackTestAccess::start(recorder, format);
        CHECK(errors.size() == 1);
        CHECK(!errors.isEmpty() && errors.first().contains(QStringLiteral("limit")));
        CHECK(observation.events.isEmpty());
        CHECK(observation.starts == 0);
        QsoRecorderPlaybackTestAccess::setPath(recorder, writeFixture(directory.path()));
        QsoRecorderPlaybackTestAccess::start(recorder, format);
        CHECK(recorder.isPlaying());
        recorder.stopPlayback();
    }
}

void testErrorCallbackCanRetryOrDestroy()
{
    QTemporaryDir directory;
    const QAudioFormat format = sinkFormat(48000, 2, QAudioFormat::Float);
    const QString valid = writeFixture(directory.path());
    const QString invalid = writeOverBudgetFixture(directory.path(), format);
    for (const bool destroy : {false, true}) {
        SinkObservation observation;
        auto recorder = std::make_unique<QsoRecorder>();
        observePlayback(*recorder, observation);
        QsoRecorderPlaybackTestAccess::setPath(*recorder, invalid);
        int errors = 0;
        QObject context;
        QObject::connect(recorder.get(), &QsoRecorder::recordingError, &context,
                         [&](const QString&) {
            ++errors;
            if (destroy) {
                recorder.reset();
            } else {
                QsoRecorderPlaybackTestAccess::setPath(*recorder, valid);
                QsoRecorderPlaybackTestAccess::start(*recorder, format);
            }
        });
        QsoRecorderPlaybackTestAccess::start(*recorder, format);
        CHECK(errors == 1);
        if (destroy) {
            CHECK(!recorder);
            CHECK(observation.events.isEmpty());
        } else {
            CHECK(recorder->isPlaying());
            CHECK(observation.events == QStringList({"sink.start", "mute.on", "started"}));
            recorder->stopPlayback();
        }
    }
}

// A direct mute observer may cancel/replace playback before the next signal.
// The retired operation must not announce started/stopped for the replacement.
void testMuteCallbackCanCancelOrReplace()
{
    QTemporaryDir directory;
    const QString path = writeFixture(directory.path());
    const QAudioFormat format = sinkFormat(24000, 2, QAudioFormat::Int16);
    for (const bool replace : {false, true}) {
        SinkObservation observation;
        QsoRecorder recorder;
        observePlayback(recorder, observation);
        QsoRecorderPlaybackTestAccess::setPath(recorder, path);
        bool acted = false;
        QObject::connect(&recorder, &QsoRecorder::muteRxRequested, &recorder,
                         [&](bool mute) {
            if (!acted && mute != replace) {
                acted = true;
                if (replace) {
                    QsoRecorderPlaybackTestAccess::start(recorder, format);
                } else {
                    recorder.stopPlayback();
                }
            }
        });
        QsoRecorderPlaybackTestAccess::start(recorder, format);
        if (replace) {
            observation.events.clear();
            recorder.stopPlayback();
            CHECK(recorder.isPlaying());
            CHECK(observation.events == QStringList({"sink.stop", "mute.off",
                                                     "sink.start", "mute.on", "started"}));
            recorder.stopPlayback();
        } else {
            CHECK(!recorder.isPlaying());
            CHECK(observation.events == QStringList({"sink.start", "mute.on",
                                                     "sink.stop", "mute.off", "stopped"}));
        }
    }
}

void testMuteCallbackCanReturnToSameState()
{
    QTemporaryDir directory;
    const QString path = writeFixture(directory.path());
    const QAudioFormat format = sinkFormat(24000, 2, QAudioFormat::Int16);
    for (const bool duringStart : {false, true}) {
        SinkObservation observation;
        QsoRecorder recorder;
        observePlayback(recorder, observation);
        QsoRecorderPlaybackTestAccess::setPath(recorder, path);
        if (!duringStart) {
            QsoRecorderPlaybackTestAccess::start(recorder, format);
            observation.events.clear();
        }
        bool acted = false;
        QObject::connect(&recorder, &QsoRecorder::muteRxRequested, &recorder,
                         [&](bool mute) {
            if (acted || mute != duringStart) {
                return;
            }
            acted = true;
            if (duringStart) {
                recorder.stopPlayback();
                QsoRecorderPlaybackTestAccess::start(recorder, format);
            } else {
                QsoRecorderPlaybackTestAccess::start(recorder, format);
                recorder.stopPlayback();
            }
        });
        if (duringStart) {
            QsoRecorderPlaybackTestAccess::start(recorder, format);
            CHECK(recorder.isPlaying());
            CHECK(observation.events == QStringList({"sink.start", "mute.on", "sink.stop",
                "mute.off", "stopped", "sink.start", "mute.on", "started"}));
            recorder.stopPlayback();
        } else {
            recorder.stopPlayback();
            CHECK(!recorder.isPlaying());
            CHECK(observation.events == QStringList({"sink.stop", "mute.off", "sink.start",
                "mute.on", "started", "sink.stop", "mute.off", "stopped"}));
        }
    }
}

void testMuteCallbackCanDestroy()
{
    QTemporaryDir directory;
    const QString path = writeFixture(directory.path());
    const QAudioFormat format = sinkFormat(24000, 2, QAudioFormat::Int16);
    for (const bool duringStart : {false, true}) {
        SinkObservation observation;
        auto recorder = std::make_unique<QsoRecorder>();
        observePlayback(*recorder, observation);
        QsoRecorderPlaybackTestAccess::setPath(*recorder, path);
        QObject context;
        QObject::connect(recorder.get(), &QsoRecorder::muteRxRequested, &context,
                         [&](bool mute) {
            if (recorder && mute == duringStart) {
                recorder.reset();
            }
        });
        QsoRecorderPlaybackTestAccess::start(*recorder, format);
        if (!duringStart) {
            recorder->stopPlayback();
        }
        CHECK(!recorder);
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-qso-playback-lifecycle"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("RecordingMode"), QStringLiteral("Client"));
    AppSettings::instance().setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));
    testPreparationRefusalReportsAndAllowsRetry();
    testErrorCallbackCanRetryOrDestroy();
    testMuteCallbackCanCancelOrReplace();
    testMuteCallbackCanReturnToSameState();
    testFileAndSinkFormatMatrix();
    testCompletionCancellationAndReplay();
    testStartFailureLeavesRxLiveAndAllowsRetry();
    testRejectedReplayCannotReuseOldPayload();
    testFailedRecordingRetiresPlayback();
    testDestructionStopsBeforeBufferTeardown();
    testMuteCallbackCanDestroy();
    std::printf("qso_recorder_playback_lifecycle_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
