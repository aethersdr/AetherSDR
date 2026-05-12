#include "FramelessWindowTitleBar.h"
#include "FramelessMoveHelper.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>

namespace AetherSDR {

namespace {

constexpr const char* kBarStyle =
    "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
    "stop:0 #5a7494, stop:0.5 #384e68, stop:1 #1e2e3e); "
    "border-bottom: 1px solid #0a1a28; }";

constexpr const char* kTitleStyle =
    "QLabel { background: transparent; color: #e0ecf4;"
    " font-size: 10px; font-weight: bold; }";

constexpr const char* kButtonStyle =
    "QPushButton { background: transparent; border: none;"
    " color: #c8d8e8; font-size: 11px; font-weight: bold;"
    " padding: 0px 4px; }"
    "QPushButton:hover { color: #ffffff; }";

constexpr const char* kCloseButtonStyle =
    "QPushButton { background: transparent; border: none;"
    " color: #c8d8e8; font-size: 11px; font-weight: bold;"
    " padding: 0px 4px; }"
    "QPushButton:hover { color: #ffffff; background: #cc2030; }";

}

FramelessWindowTitleBar::FramelessWindowTitleBar(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(18);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kBarStyle);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 0, 2, 0);
    row->setSpacing(4);

    auto* grip = new QLabel(QString::fromUtf8("\xe2\x8b\xae\xe2\x8b\xae"), this);
    grip->setStyleSheet(
        "QLabel { background: transparent; color: #a0b4c8; font-size: 10px; }");
    row->addWidget(grip);

    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setStyleSheet(kTitleStyle);
    row->addWidget(m_titleLabel);
    row->addStretch();

    auto* minButton = new QPushButton(QString::fromUtf8("\xe2\x80\x94"), this);
    minButton->setFixedSize(16, 16);
    minButton->setCursor(Qt::ArrowCursor);
    minButton->setStyleSheet(kButtonStyle);
    minButton->setToolTip("Minimize");
    connect(minButton, &QPushButton::clicked, this, [this] {
        if (auto* w = window())
            w->showMinimized();
    });
    row->addWidget(minButton);

    auto* maxButton = new QPushButton(QString::fromUtf8("\xe2\x96\xa1"), this);
    maxButton->setFixedSize(16, 16);
    maxButton->setCursor(Qt::ArrowCursor);
    maxButton->setStyleSheet(kButtonStyle);
    maxButton->setToolTip("Maximize");
    connect(maxButton, &QPushButton::clicked, this, [this] {
        if (auto* w = window()) {
            if (w->isMaximized()) {
                w->showNormal();
            } else {
                w->showMaximized();
            }
        }
    });
    row->addWidget(maxButton);

    auto* closeButton = new QPushButton(QString::fromUtf8("\xc3\x97"), this);
    closeButton->setFixedSize(16, 16);
    closeButton->setCursor(Qt::ArrowCursor);
    closeButton->setStyleSheet(kCloseButtonStyle);
    closeButton->setToolTip("Close");
    connect(closeButton, &QPushButton::clicked, this, [this] {
        if (auto* w = window())
            w->close();
    });
    row->addWidget(closeButton);
}

void FramelessWindowTitleBar::setTitleText(const QString& title)
{
    if (m_titleLabel)
        m_titleLabel->setText(title);
}

void FramelessWindowTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (FramelessMoveHelper::start(this, event)) {
        return;
    }
    QWidget::mousePressEvent(event);
}

void FramelessWindowTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (FramelessMoveHelper::move(this, event)) {
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FramelessWindowTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (FramelessMoveHelper::finish(this, event)) {
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FramelessWindowTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        FramelessMoveHelper::toggleMaximized(this);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace AetherSDR
