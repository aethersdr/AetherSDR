#include "BandStackPanel.h"
#include "core/BandStackSettings.h"
#include "models/BandPlanManager.h"
#include "models/BandDefs.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QMenu>
#include <QMap>

namespace MasterSDR {

static const char* kMenuStyle =
    "QMenu { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; }"
    "QMenu::item:selected { background: #304050; }"
    "QMenu::item:disabled { color: #607080; }"
    "QMenu::separator { background: #304050; height: 1px; margin: 4px 8px; }";

BandStackPanel::BandStackPanel(QWidget* parent)
    : QWidget(parent)
{
    setFixedWidth(80);
    setStyleSheet("background: #0a0a14;");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // Scrollable area for bookmark buttons
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: #0a0a14; width: 6px; }"
        "QScrollBar::handle:vertical { background: #304050; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    auto* scrollContent = new QWidget;
    m_buttonLayout = new QVBoxLayout(scrollContent);
    m_buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_buttonLayout->setSpacing(3);
    m_buttonLayout->addStretch(1);  // push buttons to top

    m_scrollArea->setWidget(scrollContent);
    root->addWidget(m_scrollArea, 1);

    // Bottom button row: [Clear All] [+] [Settings]
    auto* bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(2);

    static const char* kSmallBtnStyle =
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 3px; color: #8aa8c0; font-size: 13px; font-weight: bold; "
        "padding: 0px; margin: 0px; }"
        "QPushButton:hover { background: #203040; color: #c8d8e8; }";

    m_clearAllButton = new QPushButton(QString::fromUtf8("\xc3\x97"), this);  // ×
    m_clearAllButton->setFixedSize(22, 26);
    m_clearAllButton->setStyleSheet(kSmallBtnStyle);
    m_clearAllButton->setToolTip("Clear all bookmarks");
    connect(m_clearAllButton, &QPushButton::clicked,
            this, &BandStackPanel::clearAllRequested);
    bottomRow->addWidget(m_clearAllButton);

    m_addButton = new QPushButton("+", this);
    m_addButton->setFixedSize(26, 26);
    m_addButton->setStyleSheet(
        "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
        "border-radius: 3px; color: #8aa8c0; font-size: 16px; font-weight: bold; "
        "padding: 0px; margin: 0px; }"
        "QPushButton:hover { background: #203040; color: #c8d8e8; }");
    m_addButton->setToolTip("Save current frequency as a bookmark");
    connect(m_addButton, &QPushButton::clicked, this, &BandStackPanel::addRequested);
    bottomRow->addWidget(m_addButton);

    m_settingsButton = new QPushButton(QString::fromUtf8("\xe2\x9a\x99"), this);  // gear
    m_settingsButton->setFixedSize(22, 26);
    m_settingsButton->setStyleSheet(kSmallBtnStyle);
    m_settingsButton->setToolTip(
        "Band stack options.\n"
        "Pair Auto-save (dwell) with Auto-expiry for a self-pruning rolling history.");
    connect(m_settingsButton, &QPushButton::clicked, this, [this]() {
        QMenu menu;
        menu.setStyleSheet(kMenuStyle);

        auto* groupAction = menu.addAction("Group by band");
        groupAction->setCheckable(true);
        groupAction->setChecked(m_grouped);

        menu.addSeparator();
        auto* expiryLabel = menu.addAction("Auto-expiry:");
        expiryLabel->setEnabled(false);

        const int expiryOpts[] = {0, 5, 15, 30, 60};
        const char* expiryNames[] = {"  Off", "  5 min", "  15 min", "  30 min", "  60 min"};
        QAction* expiryActions[5];
        for (int i = 0; i < 5; ++i) {
            expiryActions[i] = menu.addAction(expiryNames[i]);
            expiryActions[i]->setCheckable(true);
            expiryActions[i]->setChecked(m_autoExpiryMinutes == expiryOpts[i]);
        }

        menu.addSeparator();
        auto* dwellLabel = menu.addAction("Auto-save dwell:");
        dwellLabel->setEnabled(false);

        const int dwellOpts[] = {0, 10, 30, 60};
        const char* dwellNames[] = {"  Off", "  10 sec", "  30 sec", "  60 sec"};
        QAction* dwellActions[4];
        for (int i = 0; i < 4; ++i) {
            dwellActions[i] = menu.addAction(dwellNames[i]);
            dwellActions[i]->setCheckable(true);
            dwellActions[i]->setChecked(m_autoSaveDwellSeconds == dwellOpts[i]);
        }

        // Anchor popup at the button's bottom-left so the menu opens below it
        // rather than overlapping the button's top-left corner.
        QAction* result = menu.exec(
            m_settingsButton->mapToGlobal(m_settingsButton->rect().bottomLeft()));
        if (!result) return;

        if (result == groupAction) {
            m_grouped = !m_grouped;
            emit groupByBandChanged(m_grouped);
            rebuildLayout();
            return;
        }
        for (int i = 0; i < 5; ++i) {
            if (result == expiryActions[i]) {
                m_autoExpiryMinutes = expiryOpts[i];
                emit autoExpiryChanged(expiryOpts[i]);
                return;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (result == dwellActions[i]) {
                m_autoSaveDwellSeconds = dwellOpts[i];
                emit autoSaveDwellChanged(dwellOpts[i]);
                return;
            }
        }
    });
    bottomRow->addWidget(m_settingsButton);

    root->addLayout(bottomRow);
}

QColor BandStackPanel::colorForFrequency(double freqMhz, const BandPlanManager* bpm)
{
    if (!bpm) return QColor(0x30, 0x40, 0x50);  // dark grey default
    for (const auto& seg : bpm->segments()) {
        if (freqMhz >= seg.lowMhz && freqMhz <= seg.highMhz) {
            return seg.color;
        }
    }
    return QColor(0x30, 0x40, 0x50);
}

QString BandStackPanel::bandNameForFrequency(double freqMhz)
{
    for (int i = 0; i < kBandCount; ++i) {
        if (freqMhz >= kBands[i].lowMhz && freqMhz <= kBands[i].highMhz) {
            return QString(kBands[i].name);
        }
    }
    return "Other";
}

void BandStackPanel::loadBookmarks(const QString& radioSerial, const BandPlanManager* bpm)
{
    clear();
    const auto entries = BandStackSettings::instance().entries(radioSerial);
    for (int i = 0; i < entries.size(); ++i) {
        const BandStackEntry& entry = entries[i];
        Bookmark bm;
        bm.entry = entry;
        bm.color = colorForFrequency(entry.frequencyMhz, bpm);
        bm.settingsIndex = i;
        m_bookmarks.append(bm);
    }
    rebuildLayout();
}

QLabel* BandStackPanel::createBandHeader(const QString& bandName,
                                         double lowMhz, double highMhz)
{
    auto* label = new QLabel(bandName, m_scrollArea->widget());
    label->setFixedHeight(18);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(
        "QLabel { color: #607080; font-size: 10px; font-weight: bold; "
        "border-bottom: 1px solid #203040; padding-bottom: 1px; margin-top: 4px; }");

    if (lowMhz > 0 || highMhz > 0) {
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QLabel::customContextMenuRequested, this,
                [this, bandName, lowMhz, highMhz, label](const QPoint&) {
            QMenu menu;
            menu.setStyleSheet(kMenuStyle);
            auto* clearAction = menu.addAction(
                QString("Clear %1").arg(bandName));
            if (menu.exec(label->mapToGlobal(QPoint(label->width(), 0)))
                    == clearAction) {
                emit clearBandRequested(lowMhz, highMhz);
            }
        });
    }

    return label;
}

void BandStackPanel::rebuildLayout()
{
    // Remove all items from layout without deleting bookmark buttons
    qDeleteAll(m_bandHeaders);
    m_bandHeaders.clear();

    while (QLayoutItem* item = m_buttonLayout->takeAt(0)) {
        // Widget items: don't delete the widget (buttons are tracked in m_bookmarks)
        delete item;
    }

    // Create buttons for each bookmark
    for (int i = 0; i < m_bookmarks.size(); ++i) {
        auto& bm = m_bookmarks[i];
        if (bm.button) {
            bm.button->disconnect();
            continue;  // button already exists
        }

        auto* btn = new QPushButton(m_scrollArea->widget());
        btn->setFixedSize(72, 32);
        btn->setText(QString::number(bm.entry.frequencyMhz, 'f', 3));
        btn->setToolTip(QString("%1 MHz  %2  %3")
            .arg(bm.entry.frequencyMhz, 0, 'f', 6)
            .arg(bm.entry.mode)
            .arg(bm.entry.rxAntenna));

        QColor bg = bm.color.darker(130);
        btn->setStyleSheet(
            QString("QPushButton { background: %1; border: 1px solid %2; "
                    "border-radius: 3px; color: #ffffff; font-size: 11px; font-weight: bold; }"
                    "QPushButton:hover { background: %3; }")
                .arg(bg.name(), bm.color.name(), bm.color.name()));

        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        bm.button = btn;
    }

    // Connect all buttons with correct settings indices
    for (int i = 0; i < m_bookmarks.size(); ++i) {
        auto& bm = m_bookmarks[i];
        auto* btn = bm.button;
        int sIdx = bm.settingsIndex;

        connect(btn, &QPushButton::clicked, this, [this, i]() {
            if (i < m_bookmarks.size()) {
                emit recallRequested(m_bookmarks[i].entry);
            }
        });

        connect(btn, &QPushButton::customContextMenuRequested, this,
                [this, btn, sIdx]() {
            QMenu menu;
            menu.setStyleSheet(kMenuStyle);
            auto* removeAction = menu.addAction("Remove");
            if (menu.exec(btn->mapToGlobal(QPoint(btn->width(), 0)))
                    == removeAction) {
                emit removeRequested(sIdx);
            }
        });
    }

    if (m_grouped) {
        // Group bookmarks by band (kBands order, "Other" last)
        QMap<int, QVector<int>> bandGroups;  // band index -> bookmark indices
        QVector<int> otherIndices;

        for (int i = 0; i < m_bookmarks.size(); ++i) {
            double freq = m_bookmarks[i].entry.frequencyMhz;
            int bandIdx = -1;
            for (int b = 0; b < kBandCount; ++b) {
                if (freq >= kBands[b].lowMhz && freq <= kBands[b].highMhz) {
                    bandIdx = b;
                    break;
                }
            }
            if (bandIdx >= 0)
                bandGroups[bandIdx].append(i);
            else
                otherIndices.append(i);
        }

        for (int b = 0; b < kBandCount; ++b) {
            if (!bandGroups.contains(b)) continue;
            auto* header = createBandHeader(
                QString(kBands[b].name), kBands[b].lowMhz, kBands[b].highMhz);
            m_buttonLayout->addWidget(header, 0, Qt::AlignHCenter);
            m_bandHeaders.append(header);
            for (int idx : bandGroups[b]) {
                m_buttonLayout->addWidget(m_bookmarks[idx].button, 0,
                                          Qt::AlignHCenter);
            }
        }

        if (!otherIndices.isEmpty()) {
            auto* header = createBandHeader("Other", 0, 0);
            m_buttonLayout->addWidget(header, 0, Qt::AlignHCenter);
            m_bandHeaders.append(header);
            for (int idx : otherIndices) {
                m_buttonLayout->addWidget(m_bookmarks[idx].button, 0,
                                          Qt::AlignHCenter);
            }
        }
    } else {
        // Flat: insertion order
        for (auto& bm : m_bookmarks) {
            m_buttonLayout->addWidget(bm.button, 0, Qt::AlignHCenter);
        }
    }

    m_buttonLayout->addStretch(1);
}

void BandStackPanel::setGrouped(bool grouped)
{
    if (m_grouped == grouped) return;
    m_grouped = grouped;
    rebuildLayout();
}

void BandStackPanel::setAutoExpiryMinutes(int minutes)
{
    m_autoExpiryMinutes = minutes;
}

void BandStackPanel::setAutoSaveDwellSeconds(int seconds)
{
    m_autoSaveDwellSeconds = seconds;
}

void BandStackPanel::clear()
{
    qDeleteAll(m_bandHeaders);
    m_bandHeaders.clear();
    for (auto& bm : m_bookmarks) {
        m_buttonLayout->removeWidget(bm.button);
        delete bm.button;
    }
    m_bookmarks.clear();
}

} // namespace MasterSDR
