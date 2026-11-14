#pragma once

#include <QAccessible>
#include <QAccessibleEvent>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QPoint>
#include <QPointer>
#include <QRegion>
#include <QStatusBar>
#include <QString>
#include <QTimer>
#include <QtMath>

namespace AetherSDR {

// The status row remains permanent. Temporary messages use an unmanaged child
// above it, so showing or clearing text cannot hide controls or change layout.
class StatusBarMessageOverlay : public QLabel {
    Q_OBJECT
public:
    explicit StatusBarMessageOverlay(QStatusBar* bar, QWidget* surface = nullptr)
        : QLabel(bar->parentWidget()), m_bar(bar), m_surface(surface)
    {
        setObjectName(QStringLiteral("statusBarMessageOverlay"));
        setTextFormat(Qt::PlainText);
        setContentsMargins(10, 5, 10, 5);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        hide();
        bar->installEventFilter(this);
        parentWidget()->installEventFilter(this);
        if (m_surface) {
            m_surface->installEventFilter(this);
        }
        m_announcementTimer.setSingleShot(true);
        m_announcementTimer.setInterval(100);
        connect(&m_announcementTimer, &QTimer::timeout, this, [this]() {
            // Announced even when no gap or a hidden bar keeps the notice off screen.
            if (!m_message.isEmpty() && QAccessible::isActive()) {
                QAccessibleAnnouncementEvent event(this, m_message);
                event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
                QAccessible::updateAccessibility(&event);
            }
        });
        connect(bar, &QStatusBar::messageChanged, this, [this](const QString& message) {
            m_message = message;
            setAccessibleName(message);
            updateNotice();
            if (message.isEmpty()) {
                m_announcementTimer.stop();
            } else {
                m_announcementTimer.start();
            }
        });
    }

    // The workspace canvas replaces the pan stack while it is mounted; it is
    // created after the overlay, so it joins here.
    void setAlternateSurface(QWidget* surface)
    {
        m_alternateSurface = surface;
        if (surface) {
            surface->installEventFilter(this);
        }
        updateNotice();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::LayoutRequest:
            updateNotice();
            break;
        default:
            break;
        }
        return QLabel::eventFilter(watched, event);
    }

private:
    void updateNotice()
    {
        if (m_message.isEmpty() || !m_bar || !m_bar->isVisible()) {
            clear();
            hide();
            return;
        }

        constexpr int kEdgeMargin = 8;
        constexpr int kMaximumWidth = 640;
        ensurePolished();
        const QMargins margins = contentsMargins();
        const QString singleLine = m_message.simplified();
        const int horizontalPadding = margins.left() + margins.right();
        const int textWidth = qCeil(QFontMetricsF(font()).horizontalAdvance(singleLine));
        const int noticeHeight = fontMetrics().height() + margins.top() + margins.bottom();
        const QPoint barTop = m_bar->mapTo(parentWidget(), QPoint(0, 0));
        const int top = qMax(0, barTop.y() - noticeHeight - kEdgeMargin);
        QWidget* surface = m_surface && m_surface->isVisible() ? m_surface.data()
            : m_alternateSurface && m_alternateSurface->isVisible() ? m_alternateSurface.data()
                                                                   : parentWidget();
        const QRect surfaceRect(surface->mapTo(parentWidget(), QPoint()), surface->size());
        const QRect band(surfaceRect.left() + kEdgeMargin, top,
                         qMax(0, surfaceRect.width() - 2 * kEdgeMargin), noticeHeight);
        QRegion clearArea(band);
        // Stay inside the pan area and clear its zoom/scroll/overlay controls.
        // Choose the gap independently of text length, keeping the anchor steady.
        for (QWidget* child : surface->findChildren<QWidget*>()) {
            // Floating pans are separate top-levels; their geometry does not map here.
            if (child == this || !child->isVisible() || child->window() != window()
                || !(qobject_cast<QAbstractButton*>(child) || qobject_cast<QAbstractSlider*>(child)
                     || qobject_cast<QComboBox*>(child) || qobject_cast<QLineEdit*>(child)
                     || (child->testAttribute(Qt::WA_SetCursor)
                         && child->cursor().shape() == Qt::PointingHandCursor))) {
                continue;
            }
            const QRect control(child->mapTo(parentWidget(), QPoint()), child->size());
            if (control.intersects(band)) {
                clearArea -= QRect(control.left() - kEdgeMargin, top,
                                   control.width() + 2 * kEdgeMargin, noticeHeight);
            }
        }
        QRect space;
        for (const QRect& gap : clearArea) {
            if (gap.width() > space.width()) {
                space = gap;
            }
        }
        if (space.isEmpty()) {
            hide();
            return;
        }
        const int noticeWidth = qMin(qMin(kMaximumWidth, space.width()), textWidth + horizontalPadding);
        setText(fontMetrics().elidedText(singleLine, Qt::ElideRight,
                                        qMax(0, noticeWidth - horizontalPadding)));
        setGeometry(space.left(), top, noticeWidth, noticeHeight);
        show();
        raise();
    }

    QPointer<QStatusBar> m_bar;
    QPointer<QWidget> m_surface;
    QPointer<QWidget> m_alternateSurface;
    QTimer m_announcementTimer;
    QString m_message;
};

} // namespace AetherSDR
