// Regression test for #4003 — heap-use-after-free in QsoRecorder::startFile().
//
// QsoRecorder holds the active slice via QPointer<SliceModel>, which auto-nulls
// when the SliceModel is destroyed (slice removal / radio-reconnect prune).
// Before the fix it was a raw SliceModel*, so startFile()'s `if (m_slice)`
// guard passed on a freed pointer and dereferenced garbage frequency/mode.
//
// The meaningful assertion isn't "doesn't crash" (that only fails under ASan) —
// it's that after the slice is destroyed the captured metadata is *cleared*
// (freq 0 / empty mode), so the built filename drops the freq/mode components.
// That is deterministic with the fix and impossible to satisfy with the bug.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecorder.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    const QString a_ = (actual); const QString e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d  expected \"%s\", got \"%s\"\n", \
                     __FILE__, __LINE__, \
                     e_.toUtf8().constData(), a_.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected true: %s\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

// Configure a recorder to emit a deterministic, freq/mode-only filename.
static void configure(QsoRecorder& rec, const QString& dir)
{
    rec.setRecordingDir(dir);
    rec.setIncludeDate(false);
    rec.setIncludeTime(false);
    rec.setIncludeFrequency(true);
    rec.setIncludeMode(true);
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-qso-recorder-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());

    // ── Sanity: a live slice contributes its freq + mode to the filename. This
    // is the behavior the crash path was trying to deliver.
    {
        QsoRecorder rec;
        configure(rec, tmp.path());
        SliceModel slice(0);
        slice.setFrequency(14.250);
        slice.setMode("USB");
        rec.setSlice(&slice);
        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        EXPECT_EQ(QFileInfo(rec.recordingFilePath()).fileName(),
                  QStringLiteral("14.250MHz_USB.wav"));
        rec.stopRecording();
    }

    // ── #4003: a slice destroyed before startRecording() must not be
    // dereferenced. QPointer nulls synchronously on delete, so startFile() takes
    // the else branch → freq 0 / empty mode → filename collapses to "QSO.wav".
    // With the old raw pointer this either aborted (ASan) or wrote a garbage
    // frequency into the name.
    {
        QsoRecorder rec;
        configure(rec, tmp.path());
        auto* slice = new SliceModel(0);
        slice->setFrequency(14.250);
        slice->setMode("USB");
        rec.setSlice(slice);
        delete slice;  // simulates reconnect prune freeing the slice
        rec.startRecording();
        EXPECT_TRUE(rec.isRecording());
        EXPECT_EQ(QFileInfo(rec.recordingFilePath()).fileName(),
                  QStringLiteral("QSO.wav"));
        rec.stopRecording();
    }

    if (g_failures == 0) {
        std::printf("qso_recorder_slice_lifetime_test: all checks passed\n");
        return 0;
    }
    std::printf("qso_recorder_slice_lifetime_test: %d failure(s)\n", g_failures);
    return 1;
}
