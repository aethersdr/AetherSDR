#pragma once

#include "core/Kpa1500Protocol.h"

#include <QPushButton>
#include <QTimer>
#include <QWidget>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for an Elecraft KPA1500 (#4097) — a sibling of
// AmpApplet (PGXL), AcomApplet, SpeApplet and VkampApplet, not a variant
// of any of them. See docs/architecture/kpa1500-amplifier-design.md.
//
// A dedicated applet rather than a reuse of AmpApplet for two concrete
// reasons: AmpApplet's "● RADIO / ● DIRECT" source badge describes the
// PGXL's radio-relayed telemetry path, which has no meaning for a device
// the Flex radio cannot see at all; and the KPA1500's internal ATU and
// antenna switch need somewhere to live that is not bolted onto the PGXL
// panel.
class Kpa1500Applet : public QWidget {
    Q_OBJECT

public:
    explicit Kpa1500Applet(QWidget* parent = nullptr);

    // One call per status snapshot — the applet reads the fields it shows.
    // Unset optionals render as "—", never as 0: the amp answers each query
    // independently, so "not reported yet" and "reported as zero" are
    // genuinely different states here.
    void setStatus(const Kpa1500::Status& status);
    void setConnected(bool connected);

signals:
    void operateToggled(bool operate);
    void tuneRequested();
    void atuInlineToggled(bool inLine);
    void antennaSelected(int port);  // 1-3
    void faultClearRequested();

private:
    void updateValueLabels();
    void refreshControls();

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_refGauge{nullptr};
    HGauge* m_swrGauge{nullptr};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_refLabel{nullptr};
    QLabel* m_swrLabel{nullptr};

    QLabel* m_statusPill{nullptr};
    QLabel* m_tempLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_atuLabel{nullptr};
    QLabel* m_faultLabel{nullptr};

    QPushButton* m_operateBtn{nullptr};
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_atuInlineBtn{nullptr};
    QPushButton* m_ant1Btn{nullptr};
    QPushButton* m_ant2Btn{nullptr};
    QPushButton* m_ant3Btn{nullptr};
    QPushButton* m_clearFaultBtn{nullptr};

    QTimer m_labelTimer;

    Kpa1500::Status m_status;
    bool m_connected{false};

    // setStatus() arrives at the connection's poll rate — funneled through
    // the m_labelTimer tick, the same throttle convention AcomApplet/
    // VkampApplet/AmpApplet use, rather than repainting and re-announcing
    // accessibility text on every reply.
    bool m_valuesDirty{false};
};

}  // namespace AetherSDR
