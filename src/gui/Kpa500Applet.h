#pragma once

#include "CanonSurface.h"
#include "core/Kpa500Protocol.h"
#include <QElapsedTimer>
#include <QTimer>
#include <array>
#include <deque>
#include <optional>

class QLabel;
class QPushButton;
class QComboBox;
class QCheckBox;
class QPlainTextEdit;
class QScrollArea;
class QSpacerItem;

namespace AetherSDR {
class HGauge;
class MeterModel;

class Kpa500Applet : public CanonApplet {
    Q_OBJECT
public:
    explicit Kpa500Applet(QWidget* parent = nullptr);
    void applyStatus(const Kpa500::Status& status);
    void setConnected(bool connected);
    void setResponding(bool responding);
    void setConnectionDescription(const QString& text);
    void setBandTrackingText(const QString& text);
    void setRadioTransmitting(bool transmitting);
    void updateRadioDrive(const MeterModel& meters, bool radioConnected);
    void setMaintenanceBusy(bool busy);
    void recordTelemetry(const QString& command, const Kpa500::Status& status);
    void appendDiagnostic(const QString& line);
    void setFloating(bool floating);
    bool followTx() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void setIdentification(const QString& firmware, const QString& serial);

signals:
    void advancedSettingsRequested();
    void operateToggled(bool operate);
    void clearFaultClicked();
    void fanSpeedChangeRequested(int speed);
    void powerRequested(bool on);
    void speakerRequested(bool on);
    void bandRequested(int band);
    void followTxChanged(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class Kpa500AppletTest;
    void refresh();
    void setRadioDrivePower(std::optional<float> watts);
    void updatePresentation();
    void savePreferences();
    void renderHistory();
    QString debugText() const;
    struct Point { qint64 time; std::optional<float> value; };
    static constexpr int kHistorySeconds = 600;
    static constexpr int kHistorySamples = 4096;
    static constexpr int kDebugLines = 2000;
    std::array<std::deque<Point>, 5> m_history;
    std::deque<QString> m_debugLines;
    std::deque<QString> m_verboseDebugLines;
    Kpa500::Status m_status;
    QElapsedTimer m_clock;
    qint64 m_lastReply{-1};
    QString m_connection;
    bool m_connected{false};
    bool m_responding{false};
    bool m_radioTransmitting{false};
    bool m_maintenanceBusy{false};
    bool m_mini{false};
    bool m_floating{false};
    bool m_historyExpanded{false};
    bool m_fahrenheit{false};
    QTimer m_timer;
    QLabel* m_driveReadout;
    HGauge* m_driveGauge;
    QLabel* m_state;
    QLabel* m_fault;
    QLabel* m_tracking;
    QLabel* m_trackingDetails;
    QLabel* m_link;
    QLabel* m_identity;
    QLabel* m_graph;
    QLabel* m_historyHelp;
    std::array<QLabel*, 5> m_readouts{};
    std::array<HGauge*, 5> m_gauges{};
    QPushButton* m_power;
    QPushButton* m_operate;
    QPushButton* m_standby;
    QPushButton* m_clear;
    QPushButton* m_miniButton;
    QPushButton* m_historyButton;
    QPushButton* m_settingsButton;
    QPushButton* m_debugButton;
    QPushButton* m_pause;
    QComboBox* m_band;
    QComboBox* m_fan;
    QComboBox* m_range;
    QCheckBox* m_verbose;
    QCheckBox* m_follow;
    QCheckBox* m_units;
    QCheckBox* m_speaker;
    QScrollArea* m_scroll;
    QWidget* m_content;
    QSpacerItem* m_bottomSpace;
    QWidget* m_detail;
    QWidget* m_modesPane;
    QWidget* m_toolsPane;
    QWidget* m_historyPane;
    QWidget* m_settingsPane;
    QWidget* m_debugPane;
    QPlainTextEdit* m_debug;
};
} // namespace AetherSDR
