#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTextEdit;

namespace MasterSDR {

class SpectrumWidget;

// Container for a single panadapter display (FFT spectrum + waterfall).
// Adds a title bar with placeholder min/max/close buttons above the
// SpectrumWidget.  Prepares for future multi-slice stacking where each
// slice gets its own PanadapterApplet in a vertical splitter.
class PanadapterApplet : public QWidget {
    Q_OBJECT

public:
    explicit PanadapterApplet(QWidget* parent = nullptr);

    SpectrumWidget* spectrumWidget() const { return m_spectrum; }

    // Panadapter identity (e.g. "0x40000000")
    QString panId() const { return m_panId; }
    void setPanId(const QString& id) { m_panId = id; }

    // Set the slice ID (0=A .. 7=H) shown in the title bar.  Optional
    // perClientLetter overrides the displayed letter (Multi-Flex sessions
    // use the radio-provided index_letter so the title matches the slice
    // badge — see #2606).  Empty falls back to 'A' + id.
    void setSliceId(int id, const QString& perClientLetter = QString());
    void clearSliceTitle();
    QString sliceTitle() const;

    // CW decode panel
    void setMultiPanMode(bool multi);  // show/hide title bar decorations
    void setFloatingState(bool floating);  // switch pop-out ↔ dock icon
    void setCwPanelVisible(bool visible);
    void appendCwText(const QString& text, float cost = 0.0f);
    // TX-side decoded text (#2417): rendered with a [TX] prefix and a
    // distinct color so the operator can tell their own keying apart
    // from received CW when both directions are decoded into the same
    // panel.
    void appendCwTextTx(const QString& text, float cost = 0.0f);
    void setCwStats(float pitchHz, float speedWpm);
    void clearCwText();
    QPushButton* lockPitchButton() const { return m_lockPitchBtn; }
    QPushButton* lockSpeedButton() const { return m_lockSpeedBtn; }

    QSize sizeHint() const override { return {800, 316}; }

signals:
    void activated(const QString& panId);
    void closeRequested(const QString& panId);
    void popOutClicked();
    void dockClicked();
    void maximizeRequested(const QString& panId);
    void pitchRangeChanged(int minHz, int maxHz);
    void cwPanelCloseRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    QString m_panId;
    SpectrumWidget* m_spectrum{nullptr};
    QWidget*        m_titleBar{nullptr};
    QLabel*         m_titleLabel{nullptr};
    QPushButton*    m_popOutBtn{nullptr};
    QPushButton*    m_maxBtn{nullptr};
    QPushButton*    m_closeBtn{nullptr};
    bool            m_isFloating{false};

    // CW decode
    QWidget*   m_cwPanel{nullptr};
    QTextEdit* m_cwText{nullptr};
    QLabel*    m_cwStatsLabel{nullptr};
    QSlider*      m_cwSensSlider{nullptr};
    QPushButton*  m_lockPitchBtn{nullptr};
    QPushButton*  m_lockSpeedBtn{nullptr};
    QSlider*      m_pitchMinSlider{nullptr};
    QSlider*      m_pitchMaxSlider{nullptr};
    QLabel*       m_pitchMinValLabel{nullptr};
    QLabel*       m_pitchMaxValLabel{nullptr};
    float         m_cwCostThreshold{0.70f};

    // Last CW text source — used by appendCwText / appendCwTextTx so the
    // [TX] prefix is inserted once per TX burst, not per ggmorse chunk
    // (#2417).
    enum class CwTextSource { None, Rx, Tx };
    CwTextSource  m_lastCwTextSource{CwTextSource::None};
};

} // namespace MasterSDR
