#include "AppletPicker.h"
#include "ComboStyle.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <algorithm>

namespace AetherSDR {

class AppletCategoryDelegate : public QStyledItemDelegate {
public:
    explicit AppletCategoryDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (!index.data(Qt::UserRole).toString().isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        ThemeManager& theme = ThemeManager::instance();
        painter->save();
        painter->fillRect(option.rect, theme.brush(option.widget, "color.background.2", option.rect));
        painter->setPen(theme.color(option.widget, "color.text.primary"));
        QFont font = option.font;
        font.setBold(true);
        painter->setFont(font);
        painter->drawText(option.rect.adjusted(8, 0, -8, 0),
                          Qt::AlignVCenter | Qt::AlignLeading, index.data().toString());
        painter->restore();
    }
};

AppletPicker::AppletPicker(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("appletPicker"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(4);
    auto* label = new QLabel(tr("Add:"), this);
    m_combo = new QComboBox(this);
    m_combo->setObjectName(QStringLiteral("appletPickerCombo"));
    m_combo->setAccessibleName(tr("Applet to add"));
    m_combo->setAccessibleDescription(tr("Choose an applet, then press Add. Open or unavailable applets cannot be added."));
    m_combo->setPlaceholderText(tr("Add applet…"));
    m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_combo->setMinimumWidth(0);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(8);
    m_combo->setMaxVisibleItems(14);
    m_combo->setView(new QListView(m_combo));
    m_combo->setItemDelegate(new AppletCategoryDelegate(m_combo));
    label->setBuddy(m_combo);
    applyComboStyle(m_combo, QStringLiteral(
        "QComboBox { min-height: 22px; combobox-popup: 0; }"
        "QComboBox:focus { border-color: {{color.border.accent}}; }"
        "QComboBox QAbstractItemView { min-width: 240px; }"
        "QComboBox QAbstractItemView::item { min-height: 26px; }"
        "QComboBox QAbstractItemView::item:disabled { color: {{color.text.secondary}}; }"));
    m_addButton = new QPushButton(QStringLiteral("+"), this);
    m_addButton->setObjectName(QStringLiteral("addAppletButton"));
    m_addButton->setAccessibleName(tr("Add selected applet"));
    m_addButton->setToolTip(tr("Add selected applet"));
    m_addButton->setFixedSize(28, 28);
    m_addButton->setFocusPolicy(Qt::StrongFocus);
    ThemeManager::instance().applyStyleSheet(this, QStringLiteral(
        "QWidget#appletPicker, QLabel { background: {{color.background.0}}; color: {{color.text.primary}}; }"
        "QPushButton { background: {{color.background.1}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.strong}}; border-radius: 3px; font-size: 18px; }"
        "QPushButton:hover, QPushButton:focus { border-color: {{color.border.accent}}; }"
        "QPushButton:disabled { background: {{color.button.background.disabled}};"
        " color: {{color.button.foreground.disabled}}; border-color: {{color.button.border.disabled}}; }"));
    row->addWidget(label);
    row->addWidget(m_combo, 1);
    row->addWidget(m_addButton);
    connect(m_combo, &QComboBox::currentIndexChanged, this, &AppletPicker::updateAddButton);
    connect(m_addButton, &QPushButton::clicked, this, [this]() {
        updateAddButton();
        if (m_addButton->isEnabled()) {
            emit addRequested(m_combo->currentData().toString());
        }
    });
    updateAddButton();
}

void AppletPicker::setEntries(const QList<Entry>& entries)
{
    const QString selected = m_combo->currentData().toString();
    const QSignalBlocker blocker(m_combo);
    QList<Entry> sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Entry& left, const Entry& right) {
        const int categoryOrder = QString::localeAwareCompare(left.category, right.category);
        return categoryOrder == 0
            ? QString::localeAwareCompare(left.title, right.title) < 0 : categoryOrder < 0;
    });
    m_combo->clear();
    auto* model = qobject_cast<QStandardItemModel*>(m_combo->model());
    QString category;
    for (const Entry& entry : sorted) {
        if (category != entry.category) {
            category = entry.category;
            auto* heading = new QStandardItem(category);
            QFont font = heading->font();
            font.setBold(true);
            heading->setFont(font);
            heading->setFlags(Qt::NoItemFlags);
            model->appendRow(heading);
        }
        QString text = entry.title;
        if (!entry.available) {
            text += tr(" — unavailable");
        } else if (entry.open) {
            text += tr(" — open");
        }
        auto* item = new QStandardItem(text);
        item->setData(entry.id, Qt::UserRole);
        item->setToolTip(text);
        item->setData(text, Qt::AccessibleTextRole);
        item->setFlags(entry.available && !entry.open
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
        model->appendRow(item);
    }
    const int index = selected.isEmpty() ? -1 : m_combo->findData(selected);
    m_combo->setCurrentIndex(index >= 0 && model->item(index)->isEnabled() ? index : -1);
    updateAddButton();
}

void AppletPicker::updateAddButton()
{
    const QModelIndex index = m_combo->model()->index(m_combo->currentIndex(), 0);
    m_addButton->setEnabled(!m_combo->currentData().toString().isEmpty()
        && index.flags().testFlag(Qt::ItemIsEnabled));
}

}
