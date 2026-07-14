#pragma once

#include <QWidget>

namespace AetherSDR {

class CrossNeedleMeterWidget;

// Independent PWR applet that owns the cross-needle meter's client-only
// appearance settings and context menu. Radio telemetry remains supplied by
// AppletPanel so the original VU meter and this applet share one truth source.
class CrossNeedleMeterApplet : public QWidget {
    Q_OBJECT

public:
    explicit CrossNeedleMeterApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    CrossNeedleMeterWidget* meterWidget() const { return m_meter; }

    void setTxMeters(float forwardWatts, float swr);
    void setTxPowers(float forwardWatts, float reflectedWatts);
    void setTransmitting(bool transmitting);
    void setPowerScale(int maxWatts, bool amplifierActive);
    void setFloating(bool floating);

private:
    void loadSettings();
    void persistSettings() const;
    void setFaceTheme(const QString& theme, bool persist);
    void showContextMenu(const QPoint& position);

    CrossNeedleMeterWidget* m_meter{nullptr};
    QString m_faceTheme;
    bool m_floating{false};
};

} // namespace AetherSDR
