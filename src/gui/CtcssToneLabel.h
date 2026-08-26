#pragma once

#include "core/CtcssTones.h"

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
inline constexpr int kCtcssToneFrequencyRole = Qt::UserRole + 1;
inline constexpr int kCtcssToneDesignationRole = Qt::UserRole + 2;

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

        const QString frequency = index.data(kCtcssToneFrequencyRole).toString();
        const QString designation = index.data(kCtcssToneDesignationRole).toString();
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

        const QFontMetrics metrics(option.font);
        const int frequencyWidth = ctcssToneFrequencyColumnWidth(metrics);
        const int gap = metrics.horizontalAdvance(QLatin1Char(' ')) * 3;
        const QRect content = option.rect.adjusted(
            kCtcssToneHorizontalPadding, 0, -kCtcssToneHorizontalPadding, 0);
        const QRect frequencyRect(content.left(), content.top(), frequencyWidth,
                                  content.height());
        const QRect designationRect(frequencyRect.right() + gap, content.top(),
                                    content.right() - frequencyRect.right() - gap,
                                    content.height());
        painter->drawText(frequencyRect, Qt::AlignRight | Qt::AlignVCenter, frequency);
        painter->drawText(designationRect, Qt::AlignLeft | Qt::AlignVCenter, designation);
        painter->restore();
    }
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
        combo->setItemData(row, frequency, kCtcssToneFrequencyRole);
        combo->setItemData(row, designation, kCtcssToneDesignationRole);
    }
}

} // namespace AetherSDR
