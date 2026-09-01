#include "core/CtcssTones.h"
#include "gui/CtcssToneLabel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QString>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <utility>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

const AetherSDR::CtcssTone* toneFor(double frequency)
{
    const auto it = std::ranges::find_if(
        AetherSDR::kCtcssTones,
        [frequency](const AetherSDR::CtcssTone& tone) {
            return std::abs(tone.frequency - frequency) < 0.05;
        });
    return it == std::end(AetherSDR::kCtcssTones) ? nullptr : &*it;
}

QImage renderRow(QComboBox& combo, int row, QStyle::State state)
{
    QImage image(180, 28, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    QStyleOptionViewItem option;
    option.rect = image.rect();
    option.font = combo.font();
    option.fontMetrics = QFontMetrics(combo.font());
    option.palette = combo.palette();
    option.state = state;
    option.widget = combo.view();
    combo.itemDelegate()->paint(&painter, option, combo.model()->index(row, 0));
    return image;
}

enum class DominantChannel { Red, Green, Blue };

int dominantPixels(const QImage& image, const QRect& rect, DominantChannel channel)
{
    int count = 0;
    const QRect bounded = rect.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int selected = channel == DominantChannel::Red ? color.red()
                : channel == DominantChannel::Green ? color.green() : color.blue();
            const int otherA = channel == DominantChannel::Red ? color.green() : color.red();
            const int otherB = channel == DominantChannel::Blue ? color.green() : color.blue();
            if (selected > 32 && selected > otherA * 2 && selected > otherB * 2) {
                ++count;
            }
        }
    }
    return count;
}

int rightmostDominantPixel(const QImage& image, DominantChannel channel)
{
    for (int x = image.width() - 1; x >= 0; --x) {
        if (dominantPixels(image, QRect(x, 0, 1, image.height()), channel) > 0) {
            return x;
        }
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    bool ok = true;

    const AetherSDR::CtcssTone* coded = toneFor(123.0);
    ok &= expect(coded && coded->code == 18
                     && QString::fromLatin1(coded->designation) == QStringLiteral("3Z"),
                 "123.0 Hz carries the authoritative 3Z PL designation");
    ok &= expect(coded && AetherSDR::ctcssToneLabel(*coded) == QStringLiteral("123.0 3Z"),
                 "coded tones render as frequency followed by PL designation");

    const AetherSDR::CtcssTone* wz = toneFor(69.3);
    ok &= expect(wz && wz->code == 0
                     && QString::fromLatin1(wz->designation) == QStringLiteral("WZ")
                     && AetherSDR::ctcssToneLabel(*wz) == QStringLiteral("69.3 WZ"),
                 "69.3 Hz retains its documented WZ designation");

    const AetherSDR::CtcssTone* interstitial = toneFor(159.8);
    ok &= expect(interstitial && interstitial->code == 0
                     && interstitial->designation[0] == '\0'
                     && AetherSDR::ctcssToneLabel(*interstitial) == QStringLiteral("159.8"),
                 "an interstitial without a PL designation renders as frequency only");

    QComboBox combo;
    QFont stressFont = combo.font();
    stressFont.setPointSize(std::max(14, stressFont.pointSize() + 4));
    combo.setFont(stressFont);
    AetherSDR::populateCtcssToneCombo(&combo);
    ok &= expect(combo.count() == static_cast<int>(AetherSDR::kCtcssToneCount),
                 "the shared population helper exposes every legal tone");
    ok &= expect(combo.maxVisibleItems() == AetherSDR::kCtcssTonePopupRows,
                 "the shared popup is capped at the reviewed visible-row count");
    ok &= expect(combo.view()->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn,
                 "the shared popup always exposes a visible vertical scrollbar");
    const int codedRow = combo.findData(QStringLiteral("123.0"));
    ok &= expect(codedRow >= 0
                     && combo.itemText(codedRow) == QStringLiteral("123.0 3Z")
                     && combo.itemData(codedRow, Qt::UserRole) == QStringLiteral("123.0")
                     && combo.itemData(codedRow, AetherSDR::kCtcssToneDesignationRole)
                            == QStringLiteral("3Z"),
                 "the shared combo retains separate frequency and designation columns");
    const int interstitialRow = combo.findData(QStringLiteral("159.8"));
    ok &= expect(interstitialRow >= 0
                     && combo.itemText(interstitialRow) == QStringLiteral("159.8")
                     && combo.itemData(interstitialRow,
                                       AetherSDR::kCtcssToneDesignationRole).toString().isEmpty(),
                 "the shared combo leaves an uncoded designation column blank");

    const QStyle::State selectedState = QStyle::State_Enabled
        | QStyle::State_Active | QStyle::State_Selected;
    QPalette palette = combo.palette();
    for (QPalette::ColorGroup group : {
             QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        palette.setColor(group, QPalette::Base, Qt::black);
        palette.setColor(group, QPalette::Highlight, Qt::black);
    }
    palette.setColor(QPalette::Active, QPalette::HighlightedText, Qt::red);
    palette.setColor(QPalette::Inactive, QPalette::HighlightedText, Qt::green);
    palette.setColor(QPalette::Disabled, QPalette::Text, Qt::blue);
    combo.setPalette(palette);

    AetherSDR::configureCtcssToneComboLabels(
        &combo, AetherSDR::FmTonePresentation::Ctcss,
        AetherSDR::FmToneRole::Tx);
    ok &= expect(combo.itemText(codedRow) == QStringLiteral("TX: 123.0 3Z")
                     && combo.itemData(codedRow, AetherSDR::kCtcssTonePrefixRole)
                            == QStringLiteral("TX:"),
                 "CTCSS TX presentation preserves its role prefix in the popup model");
    const QImage expected = renderRow(combo, codedRow, selectedState);

    const QFontMetrics metrics(combo.font());
    const int frequencyWidth = AetherSDR::ctcssToneFrequencyColumnWidth(metrics);
    for (const AetherSDR::CtcssTone& tone : AetherSDR::kCtcssTones) {
        ok &= expect(frequencyWidth
                         >= metrics.horizontalAdvance(
                             QString::number(tone.frequency, 'f', 1)),
                     "the frequency column accommodates every rendered tone value");
    }
    const int prefixWidth = metrics.horizontalAdvance(QStringLiteral("TX:"));
    const int prefixGap = metrics.horizontalAdvance(QLatin1Char(' '));
    const int designationGap = metrics.horizontalAdvance(QLatin1Char(' ')) * 3;
    const QRect prefixRect(AetherSDR::kCtcssToneHorizontalPadding, 0,
                           prefixWidth, expected.height());
    const QRect frequencyRect(prefixRect.x() + prefixWidth + prefixGap, 0,
                              frequencyWidth, expected.height());
    const QRect designationRect(frequencyRect.right() + designationGap, 0,
                                expected.width() - frequencyRect.right() - designationGap
                                    - AetherSDR::kCtcssToneHorizontalPadding,
                                expected.height());
    ok &= expect(dominantPixels(expected, prefixRect, DominantChannel::Red) > 0
                     && dominantPixels(expected, frequencyRect, DominantChannel::Red) > 0
                     && dominantPixels(expected, designationRect, DominantChannel::Red) > 0,
                 "active selected rendering paints the role and both aligned columns");

    const int firstAlignedRow = combo.findData(QStringLiteral("165.5"));
    const int secondAlignedRow = combo.findData(QStringLiteral("199.5"));
    AetherSDR::configureCtcssToneComboLabels(
        &combo, AetherSDR::FmTonePresentation::Legacy,
        AetherSDR::FmToneRole::Tx);
    combo.setItemData(firstAlignedRow, QStringLiteral("1.5"), Qt::UserRole);
    combo.setItemData(secondAlignedRow, QStringLiteral("111.5"), Qt::UserRole);
    const QImage firstBareFrequency = renderRow(combo, firstAlignedRow, selectedState);
    const QImage secondBareFrequency = renderRow(combo, secondAlignedRow, selectedState);
    const int firstFrequencyRight = rightmostDominantPixel(
        firstBareFrequency, DominantChannel::Red);
    const int secondFrequencyRight = rightmostDominantPixel(
        secondBareFrequency, DominantChannel::Red);
    ok &= expect(firstAlignedRow >= 0 && secondAlignedRow >= 0
                     && firstFrequencyRight >= 0
                     && firstFrequencyRight == secondFrequencyRight,
                 "different bare frequencies share the same rendered right edge");
    AetherSDR::configureCtcssToneComboLabels(
        &combo, AetherSDR::FmTonePresentation::Ctcss,
        AetherSDR::FmToneRole::Tx);

    const QImage inactive = renderRow(
        combo, codedRow, QStyle::State_Enabled | QStyle::State_Selected);
    ok &= expect(dominantPixels(inactive, frequencyRect, DominantChannel::Green) > 0
                     && dominantPixels(inactive, designationRect, DominantChannel::Green) > 0,
                 "inactive selected rendering uses the inactive palette for both columns");

    const QImage disabled = renderRow(combo, codedRow, QStyle::State_None);
    ok &= expect(dominantPixels(disabled, frequencyRect, DominantChannel::Blue) > 0
                     && dominantPixels(disabled, designationRect, DominantChannel::Blue) > 0,
                 "disabled rendering uses the disabled palette for both columns");

    const QImage uncoded = renderRow(combo, interstitialRow, selectedState);
    ok &= expect(dominantPixels(uncoded, frequencyRect, DominantChannel::Red) > 0
                     && dominantPixels(uncoded, designationRect, DominantChannel::Red) == 0,
                 "an uncoded row paints frequency ink and leaves the designation column blank");

    AetherSDR::configureCtcssToneComboLabels(
        &combo, AetherSDR::FmTonePresentation::Ctcss,
        AetherSDR::FmToneRole::Rx);
    ok &= expect(combo.itemText(codedRow) == QStringLiteral("RX: 123.0 3Z")
                     && combo.itemData(codedRow, AetherSDR::kCtcssTonePrefixRole)
                            == QStringLiteral("RX:"),
                 "CTCSS RX presentation remains distinguishable in the popup model");
    AetherSDR::configureCtcssToneComboLabels(
        &combo, AetherSDR::FmTonePresentation::Legacy,
        AetherSDR::FmToneRole::Tx);
    ok &= expect(combo.itemText(codedRow) == QStringLiteral("123.0 3Z")
                     && combo.itemData(codedRow, AetherSDR::kCtcssTonePrefixRole)
                            .toString().isEmpty(),
                 "legacy presentation removes the role prefix without changing tone data");

    QString renderedPopupRules = AetherSDR::ctcssToneComboStyleRules();
    renderedPopupRules.replace(QStringLiteral("{{color.background.0}}"),
                               QStringLiteral("#010203"));
    renderedPopupRules.replace(QStringLiteral("{{color.background.3}}"),
                               QStringLiteral("#506070"));
    renderedPopupRules.replace(QStringLiteral("{{color.accent.bright}}"),
                               QStringLiteral("#00c8f0"));
    combo.setStyleSheet(renderedPopupRules);
    combo.resize(180, 24);
    combo.show();
    combo.showPopup();
    QApplication::processEvents();
    const int rowHeight = combo.view()->sizeHintForRow(0);
    const int popupHeight = combo.view()->height();
    QScrollBar* const popupScrollBar = combo.view()->verticalScrollBar();
    ok &= expect(rowHeight >= AetherSDR::kCtcssToneRowHeight,
                 "tone rows retain the reviewed vertical breathing room");
    ok &= expect(combo.view()->verticalScrollBar()->maximum() > 0,
                 "the capped popup scrolls to the remaining tones");
    ok &= expect(popupScrollBar->isVisible() && popupScrollBar->width() == 12,
                 "the production popup rules render the always-visible 12 px scrollbar");
    QImage scrollBarImage(popupScrollBar->size(), QImage::Format_ARGB32_Premultiplied);
    scrollBarImage.fill(Qt::transparent);
    QPainter scrollBarPainter(&scrollBarImage);
    popupScrollBar->render(&scrollBarPainter);
    scrollBarPainter.end();
    int themedThumbPixels = 0;
    for (int y = 0; y < scrollBarImage.height(); ++y) {
        for (int x = 0; x < scrollBarImage.width(); ++x) {
            if (scrollBarImage.pixelColor(x, y).rgb() == QColor("#506070").rgb()) {
                ++themedThumbPixels;
            }
        }
    }
    ok &= expect(themedThumbPixels > 0,
                 "the production popup rules visibly render the themed scrollbar thumb");
    ok &= expect(rowHeight > 0
                     && popupHeight <= rowHeight * AetherSDR::kCtcssTonePopupRows + 4,
                 "the rendered popup does not exceed 18 visible tone rows");
    combo.hidePopup();

    const QString popupRules = AetherSDR::ctcssToneComboStyleRules();
    ok &= expect(popupRules.contains(QStringLiteral("combobox-popup: 0;")),
                 "the shared style disables the native uncapped popup");
    ok &= expect(popupRules.contains(QStringLiteral("QScrollBar::handle:vertical"))
                     && popupRules.contains(QStringLiteral("{{color.background.3}}"))
                     && popupRules.contains(QStringLiteral("{{color.accent.bright}}")),
                 "the shared scrollbar has themed resting and hover contrast");

    std::cerr << (ok ? "ctcss_tone_label_test PASSED\n"
                     : "ctcss_tone_label_test FAILED\n");
    return ok ? 0 : 1;
}
