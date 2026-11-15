#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientDbHistoryWidget;

// AetherTX page for the peak control (ClientCessb), the last client stage
// before Final Output: the SSB envelope peak over the last five seconds with
// the ceiling, over Clip, CESSB and Ceiling, and the passband it is holding
// inside the radio's TX filter. Not part of the reorderable chain; its tab
// checkbox switches it on. Geometry persists as StripPeakPanelGeometry.
class StripPeakPanel : public QWidget {
    Q_OBJECT

public:
    static constexpr float kTraceFloorDb = -24.0f;

    explicit StripPeakPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripPeakPanel() override;

    void showForTx();
    void syncControlsFromEngine();

signals:
    // API symmetry with the other stage pages.
    void bypassToggled(bool bypassed);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    AudioEngine* m_audio{nullptr};
    ClientDbHistoryWidget* m_history{nullptr};
    ClientCompKnob* m_clip{nullptr};
    ClientCompKnob* m_ceiling{nullptr};
    QPushButton* m_cessb{nullptr};
    QWidget* m_routeRow{nullptr};
    QPushButton* m_routeChain{nullptr};
    QPushButton* m_routeModulator{nullptr};
    QLabel* m_passband{nullptr};
    QTimer* m_poll{nullptr};
    bool m_restoring{false};
};

// The peak-control knobs, shared by the page and any docked tile.
namespace PeakKnobs {
ClientCompKnob* makeClip(int size);
ClientCompKnob* makeCeiling(int size);
}  // namespace PeakKnobs

}  // namespace AetherSDR
