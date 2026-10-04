#pragma once

#include <QAccessible>
#include <QAccessibleEvent>
#include <QLabel>
#include <QSizePolicy>
#include <QStatusBar>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// A short-lived notice in a permanent status-bar label. QStatusBar::showMessage()
// hides every non-permanent widget while its text is up, the TX indicator among
// them; a permanent widget sits beside them. The label never sets the bar's
// height or minimum width, and a screen reader is told the same text (#4896).
class StatusBarNoticeLabel : public QLabel {
public:
    explicit StatusBarNoticeLabel(QStatusBar* bar)
        : QLabel(bar)
    {
        setObjectName(QStringLiteral("statusBarNoticeLabel"));
        setTextFormat(Qt::PlainText);
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        setMinimumWidth(1);
        m_hideTimer.setSingleShot(true);
        QObject::connect(&m_hideTimer, &QTimer::timeout, this, [this] { clearNotice(); });
        if (bar) {
            bar->addPermanentWidget(this);
        }
        hide();
    }

    void showNotice(const QString& text, int durationMs)
    {
        setText(text);
        setAccessibleName(text);
        show();
        m_hideTimer.start(durationMs);
        // Polite: it never interrupts speech the operator asked for.
        if (QAccessible::isActive()) {
            QAccessibleAnnouncementEvent ev(this, text);
            ev.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
            QAccessible::updateAccessibility(&ev);
        }
    }

    void clearNotice()
    {
        m_hideTimer.stop();
        hide();
        clear();
        setAccessibleName(QString());
    }

private:
    QTimer m_hideTimer;
};

} // namespace AetherSDR
