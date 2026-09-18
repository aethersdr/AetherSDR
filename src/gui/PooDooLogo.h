#pragma once

#include <QWidget>

class QHideEvent;
class QShowEvent;

class QTimer;

namespace AetherSDR {

class ClientPudu;

// PooDoo™ Audio logo — the pulsing centrepiece of the PUDU applet.
// Renders "PooDoo™" in a bold amber typeface with a soft glow whose
// brightness scales with the bound ClientPudu's wetRmsDb() — dim
// when bypassed, bright when the exciter is actively adding content.
//
// The logo is a passive read-only widget.  It polls the engine at
// ~30 Hz via an internal QTimer and repaints when the RMS
// meaningfully changes.
class PooDooLogo : public QWidget {
    Q_OBJECT

public:
    explicit PooDooLogo(QWidget* parent = nullptr);

    void setPudu(ClientPudu* p);
    // Override the rendered wordmark.  Default is "PooDoo™" (used by
    // the docked applet + floating editor).  The strip panel sets it
    // to "AetherVoice™" for the strip and the AetherRX page.
    void setWordmark(const QString& mark);

protected:
    void paintEvent(QPaintEvent* ev) override;
    // Polling stops while this widget is hidden — a stacked page behind
    // another tab still gets its timer events, but not its repaints.
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void tick();

    ClientPudu* m_pudu{nullptr};
    QTimer*     m_timer{nullptr};
    float       m_smoothedWetDb{-120.0f};   // smoothed level for pulse
    QString     m_wordmark;                 // empty ⇒ default "PooDoo™"
};

} // namespace AetherSDR
