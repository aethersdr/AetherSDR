#include "PanLayoutDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QPainter>
#include <QRect>
#include <QSize>

namespace AetherSDR {

// ── Layout definitions ───────────────────────────────────────────────────────

static const QVector<PanLayout> kAllLayouts = {
    {"2v",  "A / B",       2, {{1}, {1}}},
    {"2h",  "A | B",       2, {{1, 1}}},
    {"2h1", "A|B / C",     3, {{1, 1}, {1}}},
    {"12h", "A / B|C",     3, {{1}, {1, 1}}},
    {"3v",  "A / B / C",   3, {{1}, {1}, {1}}},
    {"2x2", "A|B / C|D",   4, {{1, 1}, {1, 1}}},
    {"4v",  "A/B/C/D",     4, {{1}, {1}, {1}, {1}}},
    {"3h2", "A|B|C / D|E", 5, {{1, 1, 1}, {1, 1}}},
    {"2x3", "A|B / C|D / E|F", 6, {{1, 1}, {1, 1}, {1, 1}}},
    {"4h3", "A|B|C|D / E|F|G", 7, {{1, 1, 1, 1}, {1, 1, 1}}},
    {"2x4", "A|B / C|D / E|F / G|H", 8, {{1, 1}, {1, 1}, {1, 1}, {1, 1}}},
    {"1",   "Single",      1, {{1}}},
};

// Letters for each cell in order
static const char kLetters[] = "ABCDEFGH";

// ── Thumbnail widget ─────────────────────────────────────────────────────────

class LayoutThumbnail : public QWidget {
public:
    LayoutThumbnail(const PanLayout& layout, bool isCurrent, bool enabled,
                    QWidget* parent = nullptr)
        : QWidget(parent), m_layout(layout), m_current(isCurrent), m_enabled(enabled)
    {
        setFixedSize(120, 90);
        setCursor(enabled ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int pad = 4;
        const int w = width() - pad * 2;
        const int h = height() - pad * 2;
        const int gap = 3;

        // Background
        QColor bg = m_current ? QColor(0x00, 0x60, 0x7a) : QColor(0x1a, 0x2a, 0x3a);
        if (!m_enabled) bg = QColor(0x10, 0x10, 0x18);
        p.fillRect(rect(), bg);

        // Border
        QColor border = m_current ? QColor(0x00, 0xb4, 0xd8) : QColor(0x30, 0x40, 0x50);
        if (!m_enabled) border = QColor(0x20, 0x20, 0x30);
        p.setPen(QPen(border, m_current ? 2 : 1));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);

        // Draw cells
        const auto& rows = m_layout.rows;
        const int totalRows = rows.size();
        const int rowH = (h - gap * (totalRows - 1)) / totalRows;

        QColor cellColor = m_enabled ? QColor(0x2a, 0x5a, 0x8a) : QColor(0x1a, 0x1a, 0x2a);
        QColor textColor = m_enabled ? QColor(0xc8, 0xd8, 0xe8) : QColor(0x40, 0x40, 0x50);

        int letterIdx = 0;
        for (int r = 0; r < totalRows; ++r) {
            const int cols = rows[r].size();
            const int colW = (w - gap * (cols - 1)) / cols;
            const int y = pad + r * (rowH + gap);

            for (int c = 0; c < cols; ++c) {
                const int x = pad + c * (colW + gap);
                QRect cellRect(x, y, colW, rowH);

                p.setPen(Qt::NoPen);
                p.setBrush(cellColor);
                p.drawRoundedRect(cellRect, 3, 3);

                // Letter label
                p.setPen(textColor);
                p.setFont(QFont("sans-serif", 14, QFont::Bold));
                if (letterIdx < 8)
                    p.drawText(cellRect, Qt::AlignCenter,
                               QString(QChar(kLetters[letterIdx++])));
            }
        }
    }

private:
    PanLayout m_layout;
    bool m_current;
    bool m_enabled;
};

// ── Dialog ───────────────────────────────────────────────────────────────────

PanLayoutDialog::PanLayoutDialog(int maxPans, const QString& currentLayout,
                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Panadapter Layout");
    setStyleSheet("QDialog { background: #0f0f1a; }"
                  "QLabel { color: #c8d8e8; }");
    setFixedSize(560, 520);
    FramelessResizer::install(this);
    buildUI(maxPans, currentLayout);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void PanLayoutDialog::buildUI(int maxPans, const QString& currentLayout)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("Panadapter Layout"), this);
    outer->addWidget(m_titleBar);

    auto* bodyWidget = new QWidget(this);
    auto* vbox = new QVBoxLayout(bodyWidget);
    vbox->setContentsMargins(9, 9, 9, 9);
    vbox->setSpacing(8);
    m_bodyLayout = vbox;
    outer->addWidget(bodyWidget, 1);

    auto* title = new QLabel("Choose panadapter layout:");
    title->setStyleSheet("QLabel { color: #8aa8c0; font-size: 13px; font-weight: bold; }");
    title->setAlignment(Qt::AlignCenter);
    vbox->addWidget(title);

    auto* grid = new QGridLayout;
    grid->setSpacing(8);

    int col = 0, row = 0;
    const int maxCols = 3;

    for (const auto& layout : kAllLayouts) {
        bool enabled = layout.panCount <= maxPans;
        bool isCurrent = layout.id == currentLayout;

        auto* thumb = new LayoutThumbnail(layout, isCurrent, enabled);

        auto* btn = new QPushButton;
        btn->setFixedSize(130, 115);
        btn->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: rgba(0, 180, 216, 30); "
            "border: 1px solid #00b4d8; border-radius: 4px; }");

        auto* btnLayout = new QVBoxLayout(btn);
        btnLayout->setContentsMargins(4, 4, 4, 2);
        btnLayout->setSpacing(2);
        btnLayout->addWidget(thumb, 0, Qt::AlignCenter);

        auto* label = new QLabel(QString("%1 (%2 pan%3)")
            .arg(layout.label)
            .arg(layout.panCount)
            .arg(layout.panCount > 1 ? "s" : ""));
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QString("QLabel { color: %1; font-size: 10px; }")
            .arg(enabled ? "#8aa8c0" : "#404050"));
        btnLayout->addWidget(label);

        btn->setEnabled(enabled);

        QString layoutId = layout.id;
        connect(btn, &QPushButton::clicked, this, [this, layoutId]() {
            m_selected = layoutId;
            accept();
        });

        grid->addWidget(btn, row, col);
        if (++col >= maxCols) { col = 0; ++row; }
    }

    vbox->addLayout(grid);

    auto* cancelBtn = new QPushButton("Cancel");
    cancelBtn->setFixedWidth(80);
    cancelBtn->setStyleSheet(
        "QPushButton { background: #1a3a5a; border: 1px solid #205070; "
        "border-radius: 3px; color: #c8d8e8; font-size: 11px; padding: 4px; }"
        "QPushButton:hover { background: #204060; }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    vbox->addWidget(cancelBtn, 0, Qt::AlignCenter);
}

void PanLayoutDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();
    const QSize targetSize(560, on ? 538 : 520);

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    setFixedSize(targetSize);
    if (wasVisible) {
        setGeometry(QRect(geom.topLeft(), targetSize));
    }

    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_bodyLayout) {
        m_bodyLayout->setContentsMargins(9, on ? 7 : 9, 9, 9);
    }
    if (wasVisible) {
        show();
    }
}

} // namespace AetherSDR
