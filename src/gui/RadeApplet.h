#pragma once

#ifdef HAVE_RADE

#include <QWidget>

class QLabel;

namespace AetherSDR {

// Mirrors VfoWidget RADE status in the sidebar. Signal-driven from RADEEngine — no separate timer.
class RadeApplet : public QWidget {
    Q_OBJECT
public:
    explicit RadeApplet(QWidget* parent = nullptr);

public slots:
    void setRadeActive(bool on, const QString& label = {});

    void setRadeSynced(bool synced);
    void setRadeSnr(float snrDb);
    void setRadeFreqOffset(float hz);
    void setRadeCallsign(const QString& callsign);

private:
    QLabel*  m_statusLabel{nullptr};
    QLabel*  m_snrLabel{nullptr};
    QLabel*  m_callsignLabel{nullptr};
    QLabel*  m_offsetLabel{nullptr};
    QWidget* m_dataRows{nullptr};
    QLabel*  m_inactiveLabel{nullptr};

    QString  m_modeLabel;
    bool     m_active{false};
};

} // namespace AetherSDR

#endif // HAVE_RADE
