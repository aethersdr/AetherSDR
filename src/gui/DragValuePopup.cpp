#include "DragValuePopup.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kMinWidth = 58;
constexpr int kMinHeight = 46;
constexpr int kAnchorGap = 16;
constexpr int kScreenMargin = 8;

} // namespace

DragValuePopup::DragValuePopup(QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("DragValuePopup"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(
        "QWidget#DragValuePopup { background: transparent; border: none; }"
        "QLabel#DragValuePopupLabel { color: #f4fbff; background: transparent;"
        " border: none; border-radius: 0; padding: 0; margin: 0;"
        " font-size: 20px; font-weight: 800; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 7);
    layout->setSpacing(0);

    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("DragValuePopupLabel"));
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setFrameShape(QFrame::NoFrame);
    m_label->setLineWidth(0);
    m_label->setMidLineWidth(0);
    m_label->setMinimumSize(kMinWidth - 20, kMinHeight - 13);
    layout->addWidget(m_label);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    connect(m_hideTimer, &QTimer::timeout, this, &QWidget::hide);
}

void DragValuePopup::showValue(const QPoint& globalAnchor, const QString& text)
{
    if (text.isEmpty()) {
        hideNow();
        return;
    }

    m_hideTimer->stop();
    m_label->setText(text);
    adjustSize();
    resize(sizeHint().expandedTo(QSize(kMinWidth, kMinHeight)));
    move(positionForAnchor(globalAnchor));

    if (!isVisible())
        show();
    raise();
}

void DragValuePopup::linger(int msec)
{
    if (isVisible())
        m_hideTimer->start(msec);
}

void DragValuePopup::hideNow()
{
    m_hideTimer->stop();
    hide();
}

QPoint DragValuePopup::positionForAnchor(const QPoint& globalAnchor) const
{
    QScreen* screen = QGuiApplication::screenAt(globalAnchor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    const QRect bounds = screen
        ? screen->availableGeometry().adjusted(kScreenMargin, kScreenMargin,
                                               -kScreenMargin, -kScreenMargin)
        : QRect(globalAnchor - QPoint(400, 300), QSize(800, 600));

    QPoint pos(globalAnchor.x() - width() / 2,
               globalAnchor.y() - height() - kAnchorGap);

    if (pos.y() < bounds.top())
        pos.setY(globalAnchor.y() + kAnchorGap);

    pos.setX(std::clamp(pos.x(), bounds.left(), bounds.right() - width() + 1));
    pos.setY(std::clamp(pos.y(), bounds.top(), bounds.bottom() - height() + 1));
    return pos;
}

void DragValuePopup::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect().adjusted(1, 1, -1, -1);
    QPainterPath path;
    path.addRoundedRect(r, 5, 5);

    p.fillPath(path, QColor(8, 14, 24, 236));
    p.setPen(QPen(QColor(0, 180, 216, 210), 1.2));
    p.drawPath(path);
}

} // namespace AetherSDR
