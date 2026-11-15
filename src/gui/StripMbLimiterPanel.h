#pragma once

#include <QWidget>

#include <array>

class QLabel;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class MbLimiterGrBar;

// AetherTX page for the multiband limiter (ClientMbLimiter), fixed just ahead
// of Peak control: the three crossovers, then a column per band with its
// gain reduction, Threshold and Release. Not part of the reorderable chain;
// its tab checkbox switches it on. Geometry persists as
// StripMbLimiterPanelGeometry.
class StripMbLimiterPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripMbLimiterPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripMbLimiterPanel() override;

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
    static constexpr int kBands = 4;
    static constexpr int kCrossovers = 3;

    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    AudioEngine* m_audio{nullptr};
    std::array<ClientCompKnob*, kCrossovers> m_crossover{};
    std::array<ClientCompKnob*, kBands> m_threshold{};
    std::array<ClientCompKnob*, kBands> m_release{};
    std::array<MbLimiterGrBar*, kBands> m_gr{};
    std::array<QLabel*, kBands> m_bandRange{};
    QTimer* m_poll{nullptr};
    bool m_restoring{false};
};

}  // namespace AetherSDR
