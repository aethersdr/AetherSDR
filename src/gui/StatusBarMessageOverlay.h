#pragma once

#include <QAccessible>
#include <QAccessibleEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QLabel>
#include <QMargins>
#include <QPoint>
#include <QPointer>
#include <QStatusBar>
#include <QString>
#include <QtMath>

namespace AetherSDR {

// The status row remains permanent. Temporary messages use an unmanaged child
// above it, so showing or clearing text cannot hide controls or change layout.
class StatusBarMessageOverlay : public QLabel {
public:
    StatusBarMessageOverlay(QStatusBar* bar, QWidget* row)
        : QLabel(bar->parentWidget()), m_bar(bar)
    {
        bar->addPermanentWidget(row, 1);
        setObjectName(QStringLiteral("statusBarMessageOverlay"));
        setTextFormat(Qt::PlainText);
        setContentsMargins(10, 5, 10, 5);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        hide();
        bar->installEventFilter(this);
        parentWidget()->installEventFilter(this);
        connect(bar, &QStatusBar::messageChanged, this, [this](const QString& message) {
            m_message = message;
            setAccessibleName(message);
            updateNotice();
            if (!message.isEmpty() && isVisible() && QAccessible::isActive()) {
                QAccessibleAnnouncementEvent event(this, message);
                event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
                QAccessible::updateAccessibility(&event);
            }
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
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
        const int availableWidth = qMax(1, parentWidget()->width() - 2 * kEdgeMargin);
        const int textWidth = qCeil(QFontMetricsF(font()).horizontalAdvance(singleLine));
        const int noticeWidth = qMin(qMin(kMaximumWidth, availableWidth), textWidth + horizontalPadding);
        const int noticeHeight = fontMetrics().height() + margins.top() + margins.bottom();
        setText(fontMetrics().elidedText(singleLine, Qt::ElideRight,
                                        qMax(0, noticeWidth - horizontalPadding)));
        const QPoint barTop = m_bar->mapTo(parentWidget(), QPoint(0, 0));
        setGeometry(kEdgeMargin, qMax(0, barTop.y() - noticeHeight - kEdgeMargin),
                    noticeWidth, noticeHeight);
        show();
        raise();
    }

    QPointer<QStatusBar> m_bar;
    QString m_message;
};

} // namespace AetherSDR
