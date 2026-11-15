#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCfcCurveWidget;
class ClientCompKnob;

// AetherTX page for the CFC stage: the compression and post-EQ curves with
// live compression behind them, the selected band's values, Pre-comp and Post
// gain, the Post-EQ switch and the FFT size with the delay it adds.
// Geometry persists as StripCfcPanelGeometry.
class StripCfcPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripCfcPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripCfcPanel() override;

    void showForTx();
    // Pull every control from the engine (after a profile load, or another
    // surface changing the stage).
    void syncControlsFromEngine();

signals:
    // API symmetry with the other stage pages; bypass lives on the CHAIN widget.
    void bypassToggled(bool bypassed);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void showSelectedBand();
    void editSelectedBand();
    void pollCompression();
    void updateFftLabel(int fftSize);
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    AudioEngine* m_audio{nullptr};
    ClientCfcCurveWidget* m_curve{nullptr};
    QLabel* m_bandLabel{nullptr};
    QDoubleSpinBox* m_hz{nullptr};
    QDoubleSpinBox* m_comp{nullptr};
    QDoubleSpinBox* m_eq{nullptr};
    ClientCompKnob* m_precomp{nullptr};
    ClientCompKnob* m_postGain{nullptr};
    QPushButton* m_postEq{nullptr};
    QSlider* m_fft{nullptr};
    QLabel* m_fftLabel{nullptr};
    QTimer* m_poll{nullptr};
    bool m_restoring{false};
    bool m_syncing{false};
};

}  // namespace AetherSDR
