#pragma once

#include "DragValuePopup.h"
#include "MeterSmoother.h"

#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace AetherSDR {

// Combined horizontal level meter + gain slider used by the TCI and
// DAX applets (4 RX channels + 1 TX per applet).  Background shows
// level smoothed with the shared MeterSmoother ballistics so motion
// reads identically to every other metering surface in the app;
// draggable thumb controls gain.
class MeterSlider : public QWidget {
    Q_OBJECT

public:
    using DragValueFormatter = std::function<QString(float)>;

    explicit MeterSlider(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setFixedHeight(16);
        setMinimumWidth(60);
        setCursor(Qt::PointingHandCursor);

        m_animTimer.setTimerType(Qt::PreciseTimer);
        m_animTimer.setInterval(kMeterSmootherIntervalMs);
        connect(&m_animTimer, &QTimer::timeout, this, [this]() {
            if (!m_smooth.tick(m_animElapsed.restart()))
                m_animTimer.stop();
            update();
        });
    }

    float gain() const { return m_gain; }
    float level() const { return m_smooth.value(); }

    void setDragValueFormatter(DragValueFormatter formatter) {
        m_dragValueFormatter = std::move(formatter);
    }

    void setDragValuePopupEnabled(bool enabled) {
        m_dragValuePopupEnabled = enabled;
        if (!enabled && m_dragValuePopup)
            m_dragValuePopup->hideNow();
    }

    void setGain(float g) {
        g = std::clamp(g, 0.0f, 1.0f);
        if (g != m_gain) {
            m_gain = g;
            // The level meter is drawn post-fader (level × gain), so a
            // gain change must repaint to reflect the new effective
            // output level immediately.
            update();
        }
    }

    // Level is in [0, 1].  Target-only — the animation timer below
    // interpolates the displayed bar toward this target with the
    // shared MeterSmoother ballistics, so high-rate setLevel() calls
    // from the audio thread don't twitch the bar.
    void setLevel(float l) {
        l = std::clamp(l, 0.0f, 1.0f);
        m_smooth.setTarget(l);
        if (!m_smooth.needsAnimation()) {
            if (m_animTimer.isActive()) m_animTimer.stop();
            update();
        } else if (!m_animTimer.isActive()) {
            m_animElapsed.restart();
            m_animTimer.start();
        }
    }

signals:
    void gainChanged(float gain);  // 0.0–1.0

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int w = width();
        const int h = height();
        const int margin = 1;
        const int barH = h - 2 * margin;
        const int barW = w - 2 * margin;

        // Background
        p.fillRect(rect(), QColor(0x0a, 0x0a, 0x18));
        p.setPen(QColor(0x1e, 0x2e, 0x3e));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        // Level meter fill (behind the slider) — post-fader: the
        // smoothed RMS is multiplied by the current gain so the bar
        // reflects the actual output level rather than the raw input.
        // Moving the fader gives immediate visual feedback.
        const float lvl = m_smooth.value() * m_gain;
        if (lvl > 0.0f) {
            int fillW = static_cast<int>(lvl * barW);
            QColor fillColor = lvl < 0.7f ? QColor(0x00, 0x80, 0xa0, 120)
                             : lvl < 0.9f ? QColor(0xa0, 0xa0, 0x20, 120)
                                          : QColor(0xc0, 0x30, 0x30, 120);
            p.fillRect(margin, margin, fillW, barH, fillColor);
        }

        // Gain thumb position
        int thumbX = margin + static_cast<int>(m_gain * barW);
        thumbX = std::clamp(thumbX, margin, margin + barW);

        // Gain fill (solid, up to thumb)
        if (m_gain > 0.0f) {
            int gainW = static_cast<int>(m_gain * barW);
            p.fillRect(margin, margin, gainW, barH, QColor(0x00, 0xb4, 0xd8, 60));
        }

        // Thumb line
        p.setPen(QPen(QColor(0x00, 0xb4, 0xd8), 2));
        p.drawLine(thumbX, margin, thumbX, margin + barH);

        // Thumb triangle (top)
        QPolygon tri;
        tri << QPoint(thumbX - 3, margin)
            << QPoint(thumbX + 3, margin)
            << QPoint(thumbX, margin + 4);
        p.setBrush(QColor(0x00, 0xb4, 0xd8));
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            updateGainFromMouse(e->pos().x());
            showDragValuePopup(e->globalPosition().toPoint());
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_dragging) {
            updateGainFromMouse(e->pos().x());
            showDragValuePopup(e->globalPosition().toPoint());
            e->accept();
            return;
        }
        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (m_dragging && e->button() == Qt::LeftButton) {
            showDragValuePopup(e->globalPosition().toPoint());
            m_dragging = false;
            if (m_dragValuePopup)
                m_dragValuePopup->linger();
            e->accept();
            return;
        }
        QWidget::mouseReleaseEvent(e);
    }

private:
    void updateGainFromMouse(int x) {
        float g = static_cast<float>(x - 1) / static_cast<float>(width() - 2);
        g = std::clamp(g, 0.0f, 1.0f);
        if (g != m_gain) {
            m_gain = g;
            emit gainChanged(m_gain);
            update();
        }
    }

    QString dragValueText() const {
        if (m_dragValueFormatter)
            return m_dragValueFormatter(m_gain);
        return QStringLiteral("%1%").arg(std::lround(m_gain * 100.0f));
    }

    QPoint dragValueAnchor(const QPoint& fallbackGlobal) const {
        const int margin = 1;
        const int barW = width() - 2 * margin;
        if (barW <= 0)
            return fallbackGlobal;
        int thumbX = margin + static_cast<int>(m_gain * barW);
        thumbX = std::clamp(thumbX, margin, margin + barW);
        return mapToGlobal(QPoint(thumbX, height() / 2));
    }

    void showDragValuePopup(const QPoint& fallbackGlobal) {
        if (!m_dragValuePopupEnabled)
            return;
        if (!m_dragValuePopup)
            m_dragValuePopup = new DragValuePopup(this);
        m_dragValuePopup->showValue(dragValueAnchor(fallbackGlobal),
                                    dragValueText());
    }

    float         m_gain{0.5f};
    MeterSmoother m_smooth;
    QTimer        m_animTimer;
    QElapsedTimer m_animElapsed;
    DragValueFormatter m_dragValueFormatter;
    DragValuePopup* m_dragValuePopup{nullptr};
    bool          m_dragValuePopupEnabled{true};
    bool          m_dragging{false};
};

} // namespace AetherSDR
