#pragma once

#include <QWidget>

class QTimer;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class ClientLevelerHistoryWidget;

// AetherTX page for the LVLR stage: the leveler's gain over the last five
// seconds with the Max gain ceiling, over Max gain, Attack, Decay and Hang.
// Geometry persists as StripLevelerPanelGeometry.
class StripLevelerPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripLevelerPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripLevelerPanel() override;

    void showForTx();
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
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    AudioEngine* m_audio{nullptr};
    ClientLevelerHistoryWidget* m_history{nullptr};
    ClientCompKnob* m_maxGain{nullptr};
    ClientCompKnob* m_attack{nullptr};
    ClientCompKnob* m_decay{nullptr};
    ClientCompKnob* m_hang{nullptr};
    QTimer* m_poll{nullptr};
    bool m_restoring{false};
};

// The LVLR knobs, shared by the page and the docked tile so both read and
// write the same ranges and scales.
namespace LevelerKnobs {
ClientCompKnob* makeMaxGain(int size);
ClientCompKnob* makeAttack(int size);
ClientCompKnob* makeDecay(int size);
ClientCompKnob* makeHang(int size);
}  // namespace LevelerKnobs

}  // namespace AetherSDR
