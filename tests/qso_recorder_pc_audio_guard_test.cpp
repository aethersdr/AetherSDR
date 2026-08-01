// Regression test for #4629 — Client-Side recording with PC Audio disabled
// produced a 44-byte, header-only WAV and reported success.
//
// Root cause: QsoRecorder's only RX producer is RadioModel::rxDemodAudioReady,
// fed on a Flex by the `remote_audio_rx` VITA-49 stream — and that stream is
// created only when PC Audio is enabled. With it off, feedRxAudio() is never
// called. startRecording() knew none of this: it opened the file, wrote the
// 44-byte header, and waited for samples that could not arrive.
//
// The decisive assertion here is NO FILE IS CREATED. Asserting only
// !isRecording() would still pass against a fix that opened the file and then
// bailed, which would leave the same confusing artifact on disk that the
// reporter filed the issue about.
//
// The second-most important assertion is that RADIO-SIDE RECORDING STILL
// STARTS with PC Audio off. It records on the radio via `slice set <n>
// record=1` and never touches the client audio path, so it is the standing
// workaround for an operator monitoring on the radio's own speaker. Blocking it
// would be a worse regression than the bug.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecordStartPolicy.h"
#include "core/QsoRecorder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected true: %s\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_EQ_INT(actual, expected) do { \
    const int a_ = int(actual); const int e_ = int(expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d  expected %d, got %d\n", \
                     __FILE__, __LINE__, e_, a_); \
        ++g_failures; \
    } \
} while (0)

static void setMode(const char* recordingMode, const char* pcAudio)
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("RecordingMode"), QString::fromLatin1(recordingMode));
    s.setValue(QStringLiteral("PcAudioEnabled"), QString::fromLatin1(pcAudio));
    s.save();
}

// Count files the recorder may have created, so "no file" is checked against
// the filesystem rather than against the recorder's own account of itself.
static int fileCount(const QString& dir)
{
    return int(QDir(dir).entryList(QDir::Files | QDir::NoDotAndDotDot).size());
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-qso-pc-audio-guard"));
    if (!settingsProfile.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ── The bug: Client-Side + PC Audio off ─────────────────────────────────
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Client", "False");

        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());

        int blocked = 0;
        RecordStartDecision reason = RecordStartDecision::Allow;
        QObject::connect(&rec, &QsoRecorder::recordingBlocked,
                         &rec, [&](RecordStartDecision r) { ++blocked; reason = r; });
        int started = 0;
        QObject::connect(&rec, &QsoRecorder::recordingStarted,
                         &rec, [&](const QString&) { ++started; });

        rec.startRecording();

        EXPECT_TRUE(!rec.isRecording());
        // The assertion that distinguishes this fix from a half-fix: nothing
        // was written to disk at all — not even the 44-byte header.
        EXPECT_EQ_INT(fileCount(tmp.path()), 0);
        EXPECT_TRUE(rec.recordingFilePath().isEmpty());
        EXPECT_EQ_INT(blocked, 1);
        EXPECT_TRUE(reason == RecordStartDecision::BlockedPcAudioDisabled);
        EXPECT_EQ_INT(started, 0);

        // The policy agrees with what the recorder actually did, so the
        // automation bridge's `reason` field cannot drift from the behaviour.
        EXPECT_TRUE(rec.evaluateStart()
                    == RecordStartDecision::BlockedPcAudioDisabled);
    }

    // ── Client-Side + PC Audio on: unchanged, still records ─────────────────
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Client", "True");

        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());

        int blocked = 0;
        QObject::connect(&rec, &QsoRecorder::recordingBlocked,
                         &rec, [&](RecordStartDecision) { ++blocked; });

        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        EXPECT_EQ_INT(blocked, 0);
        EXPECT_EQ_INT(fileCount(tmp.path()), 1);
        rec.stopRecording();
    }

    // ── SEAM-NATIVE BACKEND + PC Audio off MUST STILL START ─────────────────
    // SimBackend owns its RX audio but does NOT lock PC Audio on (it neither
    // host-modulates nor transmits), so this configuration is reachable in demo
    // mode and its audio reaches the recorder over the seam. The provider is a
    // live callback, so this is the same code path a real backend swap takes.
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Client", "False");

        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());
        rec.setBackendOwnsRxAudioProvider([]() { return true; });

        int blocked = 0;
        QObject::connect(&rec, &QsoRecorder::recordingBlocked,
                         &rec, [&](RecordStartDecision) { ++blocked; });

        EXPECT_TRUE(rec.evaluateStart() == RecordStartDecision::Allow);
        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        EXPECT_EQ_INT(blocked, 0);
        rec.stopRecording();
    }

    // ── A provider that flips is honored live, not cached ───────────────────
    // Guards the staleness this design exists to avoid: swapping Flex → sim
    // must change the answer without anything being re-pushed.
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Client", "False");

        bool seamNative = false;
        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());
        rec.setBackendOwnsRxAudioProvider([&]() { return seamNative; });

        EXPECT_TRUE(rec.evaluateStart()
                    == RecordStartDecision::BlockedPcAudioDisabled);
        seamNative = true;   // backend swap, nothing else touched
        EXPECT_TRUE(rec.evaluateStart() == RecordStartDecision::Allow);
    }

    // ── RADIO-SIDE + PC Audio off MUST STILL START ──────────────────────────
    // Radio-side recording is handled by the radio and has no dependency on the
    // client's audio path. This is the case the guard must never catch.
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Radio", "False");

        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());

        int blocked = 0;
        QObject::connect(&rec, &QsoRecorder::recordingBlocked,
                         &rec, [&](RecordStartDecision) { ++blocked; });

        EXPECT_TRUE(rec.evaluateStart() == RecordStartDecision::Allow);
        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        EXPECT_EQ_INT(blocked, 0);
        rec.stopRecording();
    }

    // ── Zero-capture is reported, not passed off as success ─────────────────
    // The safety net: if anything else ever strands the audio feed mid-session,
    // stopping must not hand back a header-only file with no explanation.
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        setMode("Client", "True");

        QsoRecorder rec;
        rec.setRecordingDir(tmp.path());

        int errors = 0;
        QObject::connect(&rec, &QsoRecorder::recordingError,
                         &rec, [&](const QString&) { ++errors; });

        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        rec.stopRecording();  // no audio was ever fed

        EXPECT_EQ_INT(errors, 1);
        // The file is still finalized and left on disk — a valid empty WAV is
        // more useful to the operator than a deleted one, and deleting a file
        // the operator asked for would be its own surprise.
        EXPECT_TRUE(QFileInfo(rec.recordingFilePath()).size() == 44);
    }

    if (g_failures == 0) {
        std::printf("qso_recorder_pc_audio_guard_test: all checks passed\n");
        return 0;
    }
    std::printf("qso_recorder_pc_audio_guard_test: %d failure(s)\n", g_failures);
    return 1;
}
