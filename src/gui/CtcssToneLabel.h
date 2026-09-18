#pragma once

#include "core/CtcssTones.h"
#include "gui/FmTonePresentation.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>

#include <algorithm>

namespace AetherSDR {

inline constexpr int kCtcssTonePopupRows = 18;
inline constexpr int kCtcssToneRowHeight = 22;
inline constexpr int kCtcssToneHorizontalPadding = 8;
inline constexpr int kCtcssToneDesignationRole = Qt::UserRole + 1;
inline constexpr int kCtcssTonePrefixRole = Qt::UserRole + 2;

inline QString ctcssToneLabel(const QString& frequency, const QString& designation)
{
    if (designation.isEmpty()) {
        return frequency;
    }
    return QStringLiteral("%1 %2")
        .arg(frequency, designation);
}

inline QString ctcssToneLabel(const CtcssTone& tone)
{
    return ctcssToneLabel(QString::number(tone.frequency, 'f', 1),
                          QString::fromLatin1(tone.designation));
}

inline QString ctcssToneComboStyleRules()
{
    return QStringLiteral(
        "QComboBox { combobox-popup: 0; }"
        "QComboBox QAbstractItemView QScrollBar:vertical {"
        " background: {{color.background.0}}; width: 12px; margin: 2px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical {"
        " background: {{color.background.3}}; border-radius: 4px;"
        " min-height: 28px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical:hover {"
        " background: {{color.accent.bright}}; }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical,"
        "QComboBox QAbstractItemView QScrollBar::sub-line:vertical {"
        " height: 0; }");
}

inline int ctcssToneFrequencyColumnWidth(const QFontMetrics& metrics)
{
    int width = 0;
    for (const CtcssTone& tone : kCtcssTones) {
        width = std::max(
            width,
            metrics.horizontalAdvance(QString::number(tone.frequency, 'f', 1)));
    }
    return width;
}

class CtcssToneItemDelegate final : public QStyledItemDelegate
{
public:
    explicit CtcssToneItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const ColumnLayout layout = columns(option, index);
        size.setWidth(std::max(size.width(), layout.requiredWidth));
        size.setHeight(std::max(size.height(), kCtcssToneRowHeight));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem backgroundOption(option);
        initStyleOption(&backgroundOption, index);
        backgroundOption.text.clear();
        const QStyle* style = option.widget ? option.widget->style()
                                            : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &backgroundOption,
                           painter, option.widget);

        const QString frequency = index.data(Qt::UserRole).toString();
        const QString designation = index.data(kCtcssToneDesignationRole).toString();
        const QString prefix = index.data(kCtcssTonePrefixRole).toString();
        if (frequency.isEmpty()) {
            return;
        }

        painter->save();
        painter->setFont(option.font);
        const QPalette::ColorGroup group = !(option.state & QStyle::State_Enabled)
            ? QPalette::Disabled
            : option.state & QStyle::State_Active ? QPalette::Active
                                                   : QPalette::Inactive;
        const QPalette::ColorRole role = option.state & QStyle::State_Selected
            ? QPalette::HighlightedText : QPalette::Text;
        painter->setPen(option.palette.color(group, role));

        const ColumnLayout layout = columns(option, index);
        painter->drawText(layout.prefix, Qt::AlignLeft | Qt::AlignVCenter, prefix);
        painter->drawText(layout.frequency, Qt::AlignRight | Qt::AlignVCenter, frequency);
        painter->drawText(layout.designation, Qt::AlignLeft | Qt::AlignVCenter, designation);
        painter->restore();
    }

private:
    struct ColumnLayout {
        QRect prefix;
        QRect frequency;
        QRect designation;
        int requiredWidth = 0;
    };

    ColumnLayout columns(const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
    {
        updateMetrics(option.font);
        const QFontMetrics metrics(option.font);
        const QString prefix = index.data(kCtcssTonePrefixRole).toString();
        const int prefixWidth = metrics.horizontalAdvance(prefix);
        const int prefixGap = prefix.isEmpty()
            ? 0 : metrics.horizontalAdvance(QLatin1Char(' '));
        const int designationGap = m_designationWidth == 0
            ? 0 : metrics.horizontalAdvance(QLatin1Char(' ')) * 3;
        const QRect content = option.rect.adjusted(
            kCtcssToneHorizontalPadding, 0, -kCtcssToneHorizontalPadding, 0);
        ColumnLayout layout;
        layout.prefix = QRect(content.left(), content.top(), prefixWidth,
                              content.height());
        const int frequencyLeft = content.left() + prefixWidth + prefixGap;
        layout.frequency = QRect(frequencyLeft, content.top(), m_frequencyWidth,
                                 content.height());
        layout.designation = QRect(layout.frequency.right() + designationGap,
                                   content.top(), m_designationWidth,
                                   content.height());
        layout.requiredWidth = kCtcssToneHorizontalPadding * 2 + prefixWidth
            + prefixGap + m_frequencyWidth + designationGap + m_designationWidth;
        return layout;
    }

    void updateMetrics(const QFont& font) const
    {
        if (m_hasCachedMetrics && font == m_cachedFont) {
            return;
        }
        const QFontMetrics metrics(font);
        m_frequencyWidth = ctcssToneFrequencyColumnWidth(metrics);
        m_designationWidth = 0;
        for (const CtcssTone& tone : kCtcssTones) {
            m_designationWidth = std::max(
                m_designationWidth,
                metrics.horizontalAdvance(QString::fromLatin1(tone.designation)));
        }
        m_cachedFont = font;
        m_hasCachedMetrics = true;
    }

    mutable QFont m_cachedFont;
    mutable int m_frequencyWidth = 0;
    mutable int m_designationWidth = 0;
    mutable bool m_hasCachedMetrics = false;
};

inline void populateCtcssToneCombo(QComboBox* combo)
{
    combo->setMaxVisibleItems(kCtcssTonePopupRows);
    combo->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    combo->setItemDelegate(new CtcssToneItemDelegate(combo));
    for (const CtcssTone& tone : kCtcssTones) {
        const QString frequency = QString::number(tone.frequency, 'f', 1);
        const QString designation = QString::fromLatin1(tone.designation);
        combo->addItem(ctcssToneLabel(tone), frequency);
        const int row = combo->count() - 1;
        combo->setItemData(row, designation, kCtcssToneDesignationRole);
    }
}

inline void configureCtcssToneComboLabels(QComboBox* combo,
                                          FmTonePresentation presentation,
                                          FmToneRole role)
{
    const QString prefix = presentation == FmTonePresentation::Ctcss
        ? role == FmToneRole::Tx ? QStringLiteral("TX:")
                                 : QStringLiteral("RX:")
        : QString();
    for (int i = 0; i < combo->count(); ++i) {
        const QString frequency = combo->itemData(i, Qt::UserRole).toString();
        const QString designation = combo->itemData(
            i, kCtcssToneDesignationRole).toString();
        const QString toneLabel = ctcssToneLabel(frequency, designation);
        combo->setItemData(i, prefix, kCtcssTonePrefixRole);
        combo->setItemText(i, fmToneDisplayLabel(presentation, role, toneLabel));
    }
}

} // namespace AetherSDR
