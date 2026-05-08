#include "ContainerTitleBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>

namespace AetherSDR {

namespace {

constexpr const char* kBarStyle =
    "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
    "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
    "border-bottom: 1px solid #0a1a28; }";

constexpr const char* kTitleStyle =
    "QLabel { background: transparent; color: #e0ecf4; "
    "font-size: 10px; font-weight: bold; }";

constexpr const char* kBtnStyle =
    "QPushButton { background: transparent; border: none; "
    "color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 0px 4px; } "
    "QPushButton:hover { color: #ffffff; }";

// Same as kBtnStyle but with an explicit "checked" colour so an
// engaged pin reads visibly different from an idle one.
constexpr const char* kPinBtnStyle =
    "QPushButton { background: transparent; border: none; "
    "color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 0px 4px; } "
    "QPushButton:hover { color: #ffffff; } "
    "QPushButton:checked { color: #ffd166; }";

constexpr int kDragThresholdPx = 6;

} // namespace

ContainerTitleBar::ContainerTitleBar(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kHeight);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kBarStyle);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 2, 0);
    layout->setSpacing(4);

    // Drag grip glyph (⋮⋮) — purely decorative; actual drag events
    // come from mouseMoveEvent on the bar as a whole.
    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"));
    grip->setStyleSheet(
        "QLabel { background: transparent; color: #a0b4c8; font-size: 10px; }");
    layout->addWidget(grip);

    m_titleLabel = new QLabel(title);
    m_titleLabel->setStyleSheet(kTitleStyle);
    layout->addWidget(m_titleLabel);
    layout->addStretch();

    m_pinBtn = new QPushButton(QString::fromUtf8("\xf0\x9f\x93\x8c"));  // 📌
    m_pinBtn->setStyleSheet(kPinBtnStyle);
    m_pinBtn->setFixedSize(16, 16);
    m_pinBtn->setCheckable(true);
    m_pinBtn->setToolTip("Always on top");
    m_pinBtn->setCursor(Qt::ArrowCursor);
    m_pinBtn->setVisible(false);  // shown only in float mode
    connect(m_pinBtn, &QPushButton::toggled, this,
            [this](bool on) {
                m_alwaysOnTop = on;
                emit alwaysOnTopToggled(on);
            });
    layout->addWidget(m_pinBtn);

    m_floatBtn = new QPushButton(QString::fromUtf8("\xe2\x9a\x8a"));  // ⚊ single line
    m_floatBtn->setStyleSheet(kBtnStyle);
    m_floatBtn->setFixedSize(16, 16);
    m_floatBtn->setToolTip("Pop out into a floating window");
    m_floatBtn->setCursor(Qt::ArrowCursor);
    connect(m_floatBtn, &QPushButton::clicked,
            this, &ContainerTitleBar::floatToggleClicked);
    layout->addWidget(m_floatBtn);

    m_closeBtn = new QPushButton(QString::fromUtf8("\xc3\x97"));  // ×
    m_closeBtn->setStyleSheet(kBtnStyle);
    m_closeBtn->setFixedSize(16, 16);
    m_closeBtn->setToolTip("Hide this container");
    m_closeBtn->setCursor(Qt::ArrowCursor);
    connect(m_closeBtn, &QPushButton::clicked,
            this, &ContainerTitleBar::closeClicked);
    layout->addWidget(m_closeBtn);
}

void ContainerTitleBar::setTitle(const QString& title)
{
    if (m_titleLabel) m_titleLabel->setText(title);
}

QString ContainerTitleBar::title() const
{
    return m_titleLabel ? m_titleLabel->text() : QString();
}

void ContainerTitleBar::setFloatingState(bool isFloating)
{
    m_isFloating = isFloating;
    // "↙" when floating (to dock) / "⚊" when docked (to float).
    m_floatBtn->setText(isFloating
        ? QString::fromUtf8("\xe2\x86\x99")
        : QString::fromUtf8("\xe2\x9a\x8a"));
    m_floatBtn->setToolTip(isFloating
        ? "Return to panel"
        : "Pop out into a floating window");
    // ↙ already covers "close + dock" while floating, so the redundant
    // × button hides itself in float mode and reappears when docked.
    if (m_closeBtn) m_closeBtn->setVisible(m_closeAllowed && !isFloating);
    // The pin makes no sense for in-panel containers — only show it
    // when this container hosts an actual top-level window.
    if (m_pinBtn) m_pinBtn->setVisible(isFloating);
}

void ContainerTitleBar::setAlwaysOnTopState(bool on)
{
    m_alwaysOnTop = on;
    if (!m_pinBtn) return;
    QSignalBlocker blocker(m_pinBtn);
    m_pinBtn->setChecked(on);
}

void ContainerTitleBar::setCloseButtonVisible(bool visible)
{
    m_closeAllowed = visible;
    if (m_closeBtn) m_closeBtn->setVisible(visible && !m_isFloating);
}

void ContainerTitleBar::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) { QWidget::mousePressEvent(ev); return; }
    m_pressed = true;
    m_pressPos = ev->globalPosition().toPoint();
    setCursor(Qt::ClosedHandCursor);
    ev->accept();
}

void ContainerTitleBar::mouseMoveEvent(QMouseEvent* ev)
{
    if (!m_pressed || !(ev->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(ev);
        return;
    }
    const QPoint g = ev->globalPosition().toPoint();
    if ((g - m_pressPos).manhattanLength() < kDragThresholdPx) return;
    // Let the owner handle the actual QDrag — Phase 1 just signals
    // that the user moved past the drag threshold.  Phases 3 / 4
    // hook reorder into this.
    emit dragStartRequested(g);
    m_pressed = false;
    setCursor(Qt::OpenHandCursor);
}

} // namespace AetherSDR
