#pragma once

#include <QWidget>

class QPushButton;

namespace AetherSDR {

class AudioEngine;
class ClientCompKnob;
class PooDooLogo;

// Floating editor for the PUDU exciter.  Same six knobs + A/B toggle
// + logo as the docked applet, just at larger scale with dedicated
// geometry persistence.  Keeps the cognitive load low — users don't
// have to re-learn a separate UI in the floating window.
class StripPuduPanel : public QWidget {
    Q_OBJECT

public:
    enum class Side { Tx, Rx };

    explicit StripPuduPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripPuduPanel() override;

    void showForTx();
    void showForRx();

    // Pull every knob / button / label state from the bound engine.
    // Called after preset load when the engine is mutated externally.
    void syncControlsFromEngine();

signals:
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

    void onModeToggled(int id);
    void applyPooDrive(float db);
    void applyPooTune(float hz);
    void applyPooMix(float v);
    void applyDooTune(float hz);
    void applyDooHarmonics(float db);
    void applyDooMix(float v);

    AudioEngine*    m_audio{nullptr};
    Side            m_side{Side::Tx};
    QWidget*        m_titleBar{nullptr};   // EditorFramelessTitleBar*
    class ClientPudu* pudu() const;
    void              savePuduSettings() const;
    PooDooLogo*     m_logo{nullptr};
    QPushButton*    m_bypass{nullptr};
    QPushButton*    m_modeA{nullptr};
    QPushButton*    m_modeB{nullptr};
    ClientCompKnob* m_pooDrive{nullptr};
    ClientCompKnob* m_pooTune{nullptr};
    ClientCompKnob* m_pooMix{nullptr};
    ClientCompKnob* m_dooTune{nullptr};
    ClientCompKnob* m_dooHarmonics{nullptr};
    ClientCompKnob* m_dooMix{nullptr};
    bool            m_restoring{false};
};

} // namespace AetherSDR
