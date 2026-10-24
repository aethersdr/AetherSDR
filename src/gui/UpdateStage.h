#pragma once

#include "gui/CanonWindow.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QString>

namespace AetherSDR {

// The Updates window's hero: glowing stacked spectrum traces on orbit rings
// with the brand spark circling them, after aethersdr.com/experience.
// Decorative: what it means is carried by the labels laid over it.
//
// Motion is this window's style-guide exception (aethersdr-style-guide.md
// §Motion): besides the 7 s spark, the core glow breathes over 4.5 s and the
// traces drift over 6 s. All of it runs only while visible, slows while the
// window is not exposed, and holds still under reduced motion, through
// SparkWidget.
class UpdateStage : public SparkWidget {
    Q_OBJECT

public:
    // Vivid for a new release; Calm when current, checking or failed.
    enum class Mood { Vivid, Calm };

    explicit UpdateStage(QWidget* parent = nullptr);

    void setMood(Mood mood);
    Mood mood() const { return m_mood; }

    // The versions the timeline along the bottom names; an empty `latest`
    // shows only the installed one.
    void setTimeline(const QString& installed, const QString& latest);

    QSize sizeHint() const override;

    static constexpr int kHeight = 340;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void rebuildLayers();
    qreal phase(qreal periodMs) const;   // 0..1, frozen when not animating
    QPointF coreCentre() const;

    Mood    m_mood{Mood::Vivid};
    QString m_installed;
    QString m_latest;
    QPixmap m_rings;     // orbit rings, DPR-aware, rebuilt on resize/theme/mood
    QPixmap m_traces;    // stacked traces, drawn with a vertical drift offset
    QElapsedTimer m_clock;
};

} // namespace AetherSDR
