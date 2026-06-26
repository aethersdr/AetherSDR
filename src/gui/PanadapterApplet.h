#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTextEdit;
class RangeSlider;

namespace AetherSDR {

class SpectrumWidget;

// Container for a single panadapter display (FFT spectrum + waterfall).
// Adds a title bar with placeholder min/max/close buttons above the
// SpectrumWidget.  Prepares for future multi-slice stacking where each
// slice gets its own PanadapterApplet in a vertical splitter.
class PanadapterApplet : public QWidget {
    Q_OBJECT
    // Expose panId via the meta-object so the automation bridge (core/, no GUI
    // include) can map a pan back to its radio stream id without depending on
    // this header — used by `grab pan <index>` and `pan close <index>` (#3646).
    Q_PROPERTY(QString panId READ panId WRITE setPanId)

public:
    explicit PanadapterApplet(QWidget* parent = nullptr);

    SpectrumWidget* spectrumWidget() const { return m_spectrum; }

    QString panId() const { return m_panId; }
    void setPanId(const QString& id) { m_panId = id; }

    void setSliceId(int id, const QString& perClientLetter = QString());
    void clearSliceTitle();
    QString sliceTitle() const;

    void setMultiPanMode(bool multi);
    void setFloatingState(bool floating);

    // CW decode panel
    void setCwPanelVisible(bool visible);
    void appendCwText(const QString& text, float cost = 0.0f);
    void appendCwTextTx(const QString& text, float cost = 0.0f);
    void setCwStats(float pitchHz, float speedWpm);
    void clearCwText();
    QPushButton* lockPitchButton()  const { return m_lockPitchBtn; }
    QPushButton* lockSpeedButton()  const { return m_lockSpeedBtn; }
    float        cwCostThreshold()  const { return m_cwCostThreshold; }
    int speedRangeLow()   const;
    int speedRangeHigh()  const;
    int pitchRangeLow()   const;
    int pitchRangeHigh()  const;

    // RTTY decode panel
    void  setRttyPanelVisible(bool visible);
    void  appendRttyText(const QString& text, float confidence);
    void  setRttyStats(float markLevel, float spaceLevel, float snrDb, bool locked);
    void  clearRttyText();
    int   rttyMarkHz()  const;
    int   rttyShiftHz() const;
    float rttyBaud()    const;
    bool  rttyReverse() const;

    QSize sizeHint() const override { return {800, 316}; }

signals:
    void activated(const QString& panId);
    void closeRequested(const QString& panId);
    void popOutClicked();
    void dockClicked();
    void maximizeRequested(const QString& panId);

    // CW
    void pitchRangeChanged(int minHz, int maxHz);
    void speedRangeChanged(int minWpm, int maxWpm);
    void cwPanelCloseRequested();

    // RTTY
    void rttyMarkHzChanged(int hz);
    void rttyShiftHzChanged(int hz);
    void rttyBaudChanged(float baud);
    void rttyReverseChanged(bool rev);
    void rttyPanelCloseRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    // Re-apply the decoded-text stylesheet at the current font size (#3628).
    void applyCwFont();
    // Change the decoded-text font size by `delta` px, clamp, and persist (#3628).
    void adjustCwFont(int delta);
    // Persist + clamp the CW panel height after a grip drag (#3628).
    void setCwPanelHeight(int h);

    QString         m_panId;
    SpectrumWidget* m_spectrum{nullptr};
    QWidget*        m_titleBar{nullptr};
    QLabel*         m_titleLabel{nullptr};
    QPushButton*    m_popOutBtn{nullptr};
    QPushButton*    m_maxBtn{nullptr};
    QPushButton*    m_closeBtn{nullptr};
    bool            m_isFloating{false};

    // CW decode
    QWidget*      m_cwPanel{nullptr};
    QWidget*      m_cwGrip{nullptr};
    QTextEdit*    m_cwText{nullptr};
    QLabel*       m_cwStatsLabel{nullptr};
    QSlider*      m_cwSensSlider{nullptr};
    QPushButton*  m_lockPitchBtn{nullptr};
    QPushButton*  m_lockSpeedBtn{nullptr};
    RangeSlider*  m_pitchRangeSlider{nullptr};
    RangeSlider*  m_speedRangeSlider{nullptr};
    float         m_cwCostThreshold{0.70f};

    // Adjustable, persisted decoded-text display (#3628)
    int           m_cwFontPx{13};
    int           m_cwPanelHeight{80};
    bool          m_cwResizing{false};
    int           m_cwResizeStartY{0};
    int           m_cwResizeStartH{0};

    enum class CwTextSource { None, Rx, Tx };
    CwTextSource  m_lastCwTextSource{CwTextSource::None};

    // RTTY decode
    QWidget*      m_rttyPanel{nullptr};
    QTextEdit*    m_rttyText{nullptr};
    QLabel*       m_rttyStatsLabel{nullptr};
    QComboBox*    m_rttyMarkCombo{nullptr};
    QComboBox*    m_rttyShiftCombo{nullptr};
    QComboBox*    m_rttyBaudCombo{nullptr};
    QPushButton*  m_rttyRevBtn{nullptr};
};

} // namespace AetherSDR
