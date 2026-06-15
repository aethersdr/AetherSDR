#pragma once

#include <QDockWidget>
#include <QString>
#include <QStringList>
#include <QImage>

class QLineEdit;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QWidget;
class QButtonGroup;

namespace AetherSDR {

class WebSdrWaterfallView;   // mini-waterfall canvas (WebSdrPanel.cpp)

// Dockable WebSDR module panel. M1: connection + tuning controls and an
// "Audio → WebSDR" toggle. The live mini-waterfall canvas is added in M2.
//
// Pure view/controller: emits intent signals, never touches the Flex model.
class WebSdrPanel : public QDockWidget {
    Q_OBJECT
public:
    explicit WebSdrPanel(QWidget* parent = nullptr);

signals:
    void connectRequested(const QString& host);
    void disconnectRequested();
    void tuneRequested(double freqKHz, const QString& mode, double loKHz, double hiKHz);
    void audioToWebSdrToggled(bool on);
    void followSliceChanged(int sliceId);   // -1 = stop following

public slots:
    void setSourceState(int state, const QString& detail);   // WebSdrSource::State
    void setBandSpan(double centerKHz, double srKHz, const QString& name);
    void addWaterfallRow(const QImage& row);
    // Drive freq/mode/passband from outside (Flex slice follow). De-dups.
    void applyExternalTune(double freqKHz, const QString& mode, double loKHz, double hiKHz);
    // Rebuild the per-slice follow buttons (A/B/…) from the current Flex slices.
    // colors: per-slice hex strings (e.g. "#00d4ff") for consistency with the flags.
    void setSliceButtons(const QList<int>& ids, const QStringList& labels,
                         const QStringList& colors);

private slots:
    void onConnectClicked();
    void onTuneChanged();

private:
    void loadSettings();
    void saveSettings() const;
    void tuneNow();                       // emit tuneRequested + refresh marker
    void passbandFor(const QString& mode, double& loKHz, double& hiKHz) const;

    QLineEdit*      m_host{nullptr};
    QDoubleSpinBox* m_freq{nullptr};
    QPushButton*    m_connectBtn{nullptr};
    QPushButton*    m_audioToggle{nullptr};   // checkable toggle
    QWidget*        m_sliceRow{nullptr};      // hosts the per-slice follow buttons
    QButtonGroup*   m_sliceGroup{nullptr};
    int             m_followingId{-1};
    QLabel*         m_stateLabel{nullptr};
    QLabel*         m_bandLabel{nullptr};
    WebSdrWaterfallView* m_waterfall{nullptr};

    QString m_mode{QStringLiteral("CW")};   // demod mode (no UI: follow/settings drive it)
    bool    m_connected{false};
    double  m_curLoKHz{0.0};
    double  m_curHiKHz{2.7};
};

} // namespace AetherSDR
