#pragma once

#include <QByteArray>
#include <QWidget>

class QLabel;
class QFrame;
class GuardedComboBox;
class GuardedSlider;

namespace AetherSDR {

class WaveformWidget;

class WaveApplet : public QWidget {
    Q_OBJECT

public:
    explicit WaveApplet(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    void appendScopeSamples(const QByteArray& monoFloat32Pcm, int sampleRate, bool tx);
    void setTransmitting(bool tx);

    // Lean mode: fully disable the scope — hide the applet and short-circuit
    // the appendScopeSamples handler so the m_waveform sample buffer +
    // 24 Hz software repaint stop entirely. The upstream
    // AudioEngine::{tx,rx}PostChainScopeReady signal still fires per audio
    // callback (Qt queues the event onto the GUI thread either way), so the
    // "drop sample feed" framing means the appended-and-repainted work is
    // skipped, not the signal emission itself. (#3283)
    void setActive(bool on);
    bool isActive() const { return m_active; }

private:
    void buildSettingsDrawer();
    void setSettingsExpanded(bool expanded);
    void updateZoomLabel();
    void updateRefreshLabel();
    void updateWindowLabel();

    bool m_active{true};  // false in lean mode — applet hidden + feed dropped
    WaveformWidget* m_waveform{nullptr};
    QFrame* m_settingsDrawer{nullptr};
    GuardedComboBox* m_viewCombo{nullptr};
    GuardedSlider* m_zoomSlider{nullptr};
    GuardedSlider* m_refreshSlider{nullptr};
    GuardedSlider* m_windowSlider{nullptr};
    QLabel* m_zoomValue{nullptr};
    QLabel* m_refreshValue{nullptr};
    QLabel* m_windowValue{nullptr};
};

} // namespace AetherSDR
