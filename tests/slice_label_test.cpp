#include "gui/SliceLabel.h"
#include "core/AppSettings.h"

#include <QChar>
#include <QString>
#include <cstdio>

using namespace MasterSDR;

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

static void setMode(const char* mode)
{
    AppSettings::instance().setValue("SliceLetterDisplay", mode);
}

int main()
{
    // ── Global mode (default) ─────────────────────────────────────
    setMode("Global");

    // richText returns plain global letter, no subscript, regardless of
    // whether a radio letter is supplied.
    EXPECT_EQ(SliceLabel::richText(0),           QString("A"));
    EXPECT_EQ(SliceLabel::richText(2),           QString("C"));
    EXPECT_EQ(SliceLabel::richText(2, "A"),      QString("C"));
    EXPECT_EQ(SliceLabel::richText(7, "B"),      QString("H"));

    EXPECT_EQ(SliceLabel::unicodeForm(0),        QString("A"));
    EXPECT_EQ(SliceLabel::unicodeForm(2, "A"),   QString("C"));

    // Out-of-range global slice id falls back to '?' in global mode.
    EXPECT_EQ(SliceLabel::richText(-1),          QString("?"));

    // ── RadioIndexed mode ─────────────────────────────────────────
    setMode("RadioIndexed");

    // Prefer the radio letter; subscript is 1-based global slot number.
    EXPECT_EQ(SliceLabel::richText(0, "A"),      QString("A<sub>1</sub>"));
    EXPECT_EQ(SliceLabel::richText(1, "A"),      QString("A<sub>2</sub>"));
    EXPECT_EQ(SliceLabel::richText(2, "B"),      QString("B<sub>3</sub>"));

    // Missing radio letter falls back to the global letter, still with
    // subscript so slot info isn't lost.
    EXPECT_EQ(SliceLabel::richText(2),           QString("C<sub>3</sub>"));

    EXPECT_EQ(SliceLabel::unicodeForm(1, "A"),
              QString("A") + QChar(0x2082));
    EXPECT_EQ(SliceLabel::unicodeForm(7, "B"),
              QString("B") + QChar(0x2088));

    // displayColorIndex — Global mode returns sliceId; RadioIndexed
    // returns letter index, falling back to sliceId when letter is
    // empty.
    setMode("Global");
    EXPECT_EQ(SliceLabel::displayColorIndex(2, "A"), 2);
    setMode("RadioIndexed");
    EXPECT_EQ(SliceLabel::displayColorIndex(2, "A"), 0);
    EXPECT_EQ(SliceLabel::displayColorIndex(3, "B"), 1);
    EXPECT_EQ(SliceLabel::displayColorIndex(3, ""),  3);  // fallback

    // Restore default for any subsequent tests.
    setMode("Global");

    if (g_failures == 0) {
        std::printf("slice_label_test: all checks passed\n");
        return 0;
    }
    std::printf("slice_label_test: %d failure(s)\n", g_failures);
    return 1;
}
