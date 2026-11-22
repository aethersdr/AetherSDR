#pragma once

#include "core/ThemeManager.h"

#include <QLabel>
#include <QPainter>

namespace AetherSDR {

// A static text label with a painted Orbit mark. Its full name remains
// available through QLabel's accessible interface even when visually elided.
class AgentControlBadge final : public QLabel {
public:
    explicit AgentControlBadge(QWidget* parent)
        : QLabel(parent)
    {
        setObjectName(QStringLiteral("automationChip"));
        setTextFormat(Qt::PlainText);
        setFixedSize(136, 27);
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, qOverload<>(&QWidget::update));
        hide();
    }

    void showActivity(const QString& activity, const QString& agent)
    {
        if (text() != agent) {
            setText(agent);
            setAccessibleName(agent.isEmpty() ? QString() : tr("Agent control: %1").arg(agent));

        }
        setAccessibleDescription(activity);
        setToolTip(activity.isEmpty() ? QString()
            : QStringLiteral("<p>%1</p><p>%2</p>").arg(accessibleName().toHtmlEscaped(),
                                                        activity.toHtmlEscaped()));
        setVisible(!activity.isEmpty());
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        auto& theme = ThemeManager::instance();
        const auto color = [this, &theme](const char* token) {
            return theme.color(this, QString::fromLatin1(token));
        };
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(color("color.canon.raised"));
        p.setPen(color("color.canon.lineHi"));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 5, 5);

        const QRectF orbit(6, 4.5, 18, 18);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(orbit);
        p.setPen(QPen(color("color.canon.cyan"), 1.15, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(orbit, 20 * 16, 200 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(color("color.canon.sparkHot"));
        p.drawEllipse(QPointF(23.46, 10.42), 1.45, 1.45);
        p.setBrush(color("color.canon.ink"));
        p.drawPolygon(QPolygonF{{12.2, 8.95}, {20.5, 14.43}, {16.88, 15.15}, {15.16, 18.39}});

        QFont f = theme.font(this, QStringLiteral("font.family.ui"));
        f.setWeight(QFont::Normal);
        f.setPixelSize(8);
        f.setLetterSpacing(QFont::AbsoluteSpacing, .85);
        p.setFont(f);
        p.setPen(color("color.canon.cyan"));
        p.drawText(QPointF(31, 10), tr("AGENT CONTROL"));
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0);
        f.setPixelSize(15);
        p.setFont(f);
        p.setPen(color("color.canon.ink"));
        p.drawText(QPointF(31, 23), p.fontMetrics().elidedText(text(), Qt::ElideRight, width() - 38));
    }
};

} // namespace AetherSDR
