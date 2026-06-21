#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (a_ != e_) { \
        const QString a_str = QString("%1").arg(a_); \
        const QString e_str = QString("%1").arg(e_); \
        std::fprintf(stderr, "FAIL %s:%d  expected %s, got %s\n", \
                     __FILE__, __LINE__, \
                     e_str.toUtf8().constData(), \
                     a_str.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

static QMap<QString, QString> kv(std::initializer_list<std::pair<QString, QString>> pairs)
{
    QMap<QString, QString> m;
    for (const auto& p : pairs) m.insert(p.first, p.second);
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── Fallback: letter() returns 'A' + sliceId when index_letter hasn't
    // arrived yet (older firmware, early in status sequence).
    {
        SliceModel s(2);
        EXPECT_EQ(s.letter(), QString("C"));  // 'A' + 2
    }

    // ── applyStatus("index_letter=A") populates the radio-given letter
    // and emits letterChanged with the resolved value.
    {
        SliceModel s(3);
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyStatus(kv({{"index_letter", "A"}}));
        EXPECT_EQ(s.letter(), QString("A"));
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("A"));
    }

    // ── Re-applying the same letter does NOT re-emit letterChanged.
    {
        SliceModel s(1);
        s.applyStatus(kv({{"index_letter", "B"}}));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyStatus(kv({{"index_letter", "B"}}));
        EXPECT_EQ(spy.count(), 0);
    }

    // ── Letter change emits exactly once and the resolved value follows.
    {
        SliceModel s(2);
        s.applyStatus(kv({{"index_letter", "A"}}));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyStatus(kv({{"index_letter", "C"}}));
        EXPECT_EQ(s.letter(), QString("C"));
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("C"));
    }

    // ── emitLetterRefresh() forces a re-emission with the current value
    // (used when a display preference changes).
    {
        SliceModel s(0);
        s.applyStatus(kv({{"index_letter", "A"}}));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.emitLetterRefresh();
        EXPECT_EQ(spy.count(), 1);
        EXPECT_EQ(spy.takeFirst().at(0).toString(), QString("A"));
    }

    // ── Status messages without index_letter don't disturb the stored
    // letter or emit on letterChanged.
    {
        SliceModel s(2);
        s.applyStatus(kv({{"index_letter", "A"}}));
        QSignalSpy spy(&s, &SliceModel::letterChanged);
        s.applyStatus(kv({{"in_use", "1"}, {"RF_frequency", "14.250"}}));
        EXPECT_EQ(s.letter(), QString("A"));
        EXPECT_EQ(spy.count(), 0);
    }

    // ── External receive replacement (KiwiSDR virtual antenna) owns visible
    // audio pan locally. It must not send audio_pan commands to the Flex radio
    // while Flex audio is silently muted for the same slice.
    {
        SliceModel s(4);
        QStringList commands;
        QObject::connect(&s, &SliceModel::commandReady,
                         [&commands](const QString& cmd) { commands.append(cmd); });
        s.applyStatus(kv({{"audio_pan", "25"}}));
        EXPECT_EQ(s.audioPan(), 25);
        EXPECT_EQ(s.flexAudioPan(), 25);

        QSignalSpy panSpy(&s, &SliceModel::audioPanChanged);
        s.setExternalReceiveAudioReplacementMute(true);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_mute=1"));
        EXPECT_EQ(s.audioPan(), 25);
        EXPECT_EQ(s.flexAudioPan(), 25);
        EXPECT_EQ(panSpy.count(), 0);
        commands.clear();

        s.setAudioPan(80);
        EXPECT_EQ(s.audioPan(), 80);
        EXPECT_EQ(s.flexAudioPan(), 25);
        EXPECT_EQ(commands.join(QStringLiteral("|")), QString());
        EXPECT_EQ(panSpy.count(), 1);
        EXPECT_EQ(panSpy.takeFirst().at(0).toInt(), 80);

        s.applyStatus(kv({{"audio_pan", "10"}}));
        EXPECT_EQ(s.audioPan(), 80);
        EXPECT_EQ(s.flexAudioPan(), 10);
        EXPECT_EQ(panSpy.count(), 0);

        s.setExternalReceiveAudioReplacementMute(false, false);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_mute=0"));
        EXPECT_EQ(s.audioPan(), 10);
        EXPECT_EQ(s.flexAudioPan(), 10);
        EXPECT_EQ(panSpy.count(), 1);
        EXPECT_EQ(panSpy.takeFirst().at(0).toInt(), 10);
        commands.clear();

        s.setAudioPan(60);
        EXPECT_EQ(s.audioPan(), 60);
        EXPECT_EQ(s.flexAudioPan(), 60);
        EXPECT_EQ(commands.join(QStringLiteral("|")),
                  QStringLiteral("slice set 4 audio_pan=60"));
    }

    if (g_failures == 0) {
        std::printf("slice_model_letter_test: all checks passed\n");
        return 0;
    }
    std::printf("slice_model_letter_test: %d failure(s)\n", g_failures);
    return 1;
}
