#pragma once

#include <QWidget>

class QTimer;

namespace MasterSDR {

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
    // to "MasterExciter™" for the rebranded final-stage version.
    void setWordmark(const QString& mark);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    void tick();

    ClientPudu* m_pudu{nullptr};
    QTimer*     m_timer{nullptr};
    float       m_smoothedWetDb{-120.0f};   // smoothed level for pulse
    QString     m_wordmark;                 // empty ⇒ default "PooDoo™"
};

} // namespace MasterSDR
