#include "NetReminderBanner.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

NetReminderBanner::NetReminderBanner(QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setObjectName("NetReminderBanner");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAccessibleName("Net reminder");
    setMinimumWidth(320);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 12, 12, 12);
    root->setSpacing(12);

    auto* glyph = new QLabel(QStringLiteral("📻"), this);
    glyph->setStyleSheet("font-size: 22px;");
    root->addWidget(glyph, 0, Qt::AlignTop);

    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(2);
    m_headline = new QLabel(this);
    m_headline->setStyleSheet("font-weight: 600; font-size: 13px; color: palette(text);");
    m_headline->setWordWrap(true);
    m_detail = new QLabel(this);
    m_detail->setStyleSheet("color: palette(mid); font-size: 11px;");
    textCol->addWidget(m_headline);
    textCol->addWidget(m_detail);
    root->addLayout(textCol, 1);

    auto* btnCol = new QVBoxLayout();
    btnCol->setSpacing(6);
    auto* tuneBtn = new QPushButton(QStringLiteral("Tune Now"), this);
    tuneBtn->setObjectName("NetReminderTune");
    tuneBtn->setAccessibleName("Tune to this net");
    tuneBtn->setCursor(Qt::PointingHandCursor);
    tuneBtn->setDefault(true);
    auto* dismissBtn = new QPushButton(QStringLiteral("Dismiss"), this);
    dismissBtn->setAccessibleName("Dismiss reminder");
    dismissBtn->setCursor(Qt::PointingHandCursor);
    btnCol->addWidget(tuneBtn);
    btnCol->addWidget(dismissBtn);
    root->addLayout(btnCol, 0);

    connect(tuneBtn, &QPushButton::clicked, this, [this, tuneBtn] {
        // Disabled when no radio is connected; guard anyway.
        if (!tuneBtn->isEnabled())
            return;
        Q_EMIT tuneRequested(m_netId);
        close();
    });
    connect(dismissBtn, &QPushButton::clicked, this, [this] {
        Q_EMIT dismissed(m_netId);
        close();
    });

    // Keep a handle so showReminder can flip the enabled state.
    m_tuneButton = tuneBtn;
}

void NetReminderBanner::showReminder(const QString& netId, const QString& headline,
                                     const QString& detail, bool canTune)
{
    m_netId = netId;
    m_headline->setText(headline);
    m_detail->setText(detail);
    if (m_tuneButton) {
        m_tuneButton->setEnabled(canTune);
        m_tuneButton->setToolTip(canTune ? QString()
                                         : QStringLiteral("Connect a radio to tune to this net"));
    }
    adjustSize();
    anchorToParent();
    show();
    raise();
}

void NetReminderBanner::anchorToParent()
{
    QWidget* anchor = parentWidget();
    if (!anchor)
        return;
    const QRect pg = anchor->frameGeometry();
    const int margin = 24;
    move(pg.right() - width() - margin, pg.bottom() - height() - margin);
}

void NetReminderBanner::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor bg = palette().color(QPalette::Window);
    bg.setAlpha(248);
    p.setBrush(bg);
    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);
    QFrame::paintEvent(event);
}

} // namespace AetherSDR
