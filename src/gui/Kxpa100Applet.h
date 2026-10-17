#pragma once
#include "CanonSurface.h"
#include "models/Kxpa100Types.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <array>
#include <deque>
class QLabel;
class QPushButton;
class QCheckBox;
class QScrollArea;
class QSpinBox;
class QFileDialog;
class QComboBox;
namespace AetherSDR
{
class Kxpa100Model;
class Kxpa100DebugOutput;
class HGauge;
class Kxpa100Applet : public CanonApplet
{
    Q_OBJECT
  public:
    explicit Kxpa100Applet(QWidget* parent = nullptr);
    ~Kxpa100Applet() override;
    void setModel(Kxpa100Model* model);
    void setFloating(bool floating);
    void setRadioContext(const QString& radioName, int txFrequencyKhz, bool associated);
    void setAssociationResult(bool saved);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
  signals:
    void advancedSettingsRequested();
    void associateRadioRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void refresh();
    void presentation();
    void savePreferences();
    void renderHistory();
    void saveDebug();
    QPointer<Kxpa100Model> m_model;
    QPointer<QFileDialog> m_fileDialog;
    QScrollArea* m_scroll;
    QWidget* m_content;
    QWidget* m_modesPane;
    QWidget* m_paPane;
    QWidget* m_auxPane;
    QComboBox* m_historyWindow;
    QLabel* m_association;
    QPushButton* m_associate;
    std::array<QLabel*,7> m_meterLabels{};
    std::array<HGauge*,7> m_meters{};
    QWidget* m_antennasPane;
    QWidget* m_physicalPane;
    QWidget* m_manualPane;
    QWidget* m_settingsPane;
    QWidget* m_historyPane;
    QWidget* m_debugPane;
    QPushButton* m_power;
    QPushButton* m_tune;
    QPushButton* m_mini = nullptr;
    QPushButton* m_manualToggle;
    QPushButton* m_settings;
    QPushButton* m_historyToggle;
    QPushButton* m_debugToggle;
    std::array<QPushButton*, 3> m_modes{};
    std::array<QPushButton*, 2> m_antennas{};
    std::array<QCheckBox*, 8> m_capBits{}, m_indBits{};
    QCheckBox* m_side;
    QCheckBox* m_verbose;
    QCheckBox* m_pause;
    QSpinBox* m_cap;
    QSpinBox* m_ind;
    QSpinBox* m_frequencyRecall;
    QLabel* m_frequency;
    QPushButton* m_swr;
    std::array<QPushButton*, 2> m_physical{};
    QLabel* m_readiness;
    QLabel* m_miniState;
    QLabel* m_fault;
    QLabel* m_identity;
    QLabel* m_observed;
    QLabel* m_availability;
    QLabel* m_result;
    QLabel* m_graph;
    HGauge* m_swrGauge;
    Kxpa100DebugOutput* m_debug;
    struct Point {
        qint64 time;
        std::optional<double> value;
    };
    std::array<std::deque<Point>, 3> m_history;
    QElapsedTimer m_clock;
    QTimer m_timer;
    std::array<std::optional<int>, 2> m_lastRelayMasks{};
    bool m_frequencyEdited = false;
    bool m_floating = false;
    bool m_showingBypass = false;
    int m_txFrequencyKhz = 0;
    bool m_associated = false;
};
} // namespace AetherSDR
