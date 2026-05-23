#pragma once

#include "PersistentDialog.h"

#include <QMetaObject>
#include <QPointer>
#include <QVector>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;
class QShortcut;
class QCloseEvent;
class QEvent;
class QKeyEvent;

namespace AetherSDR {

class SliceModel;
class VirtualFlexControlWheel;

class FlexControlDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit FlexControlDialog(QWidget* parent = nullptr);
    ~FlexControlDialog() override;

    void setSlice(SliceModel* slice);
    void setStepSize(int hz);
    void setPhysicalReady(bool ready, const QString& port = {});
    void refreshButtonActions();
    void setActiveAuxButton(int button);
    void reflectButtonPress(int button, int action);

signals:
    void virtualWheelSteps(const QString& actionId, int steps);
    void virtualButtonPressed(int button, int action);
    void flexControlSettingsChanged();
    void physicalDetectRequested();
    void physicalDisconnectRequested();

public slots:
    void reject() override;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void recordVirtualTune(int steps);
    bool handleEscapeRelease(QEvent* event);
    void releaseWheelCapture();
    void activateAuxButton(int index, int gesture);
    void clearAuxSelection();
    void updateAuxIndicators();
    void updateModeReadout();
    QString auxActionId(int index) const;
    QString activeKnobActionId() const;
    bool auxActionControlsWheel(int index) const;
    bool externalSpinEnabled() const;
    void animateExternalTune(double mhz);
    void updateSliceReadout();
    void updateOptionButtons();
    void updateCompactMode();
    void applyCompactWindowSize();
    void setCaptureHintActive(bool active);

    QPointer<SliceModel> m_slice;
    QMetaObject::Connection m_frequencyConnection;
    QMetaObject::Connection m_stepConnection;
    QMetaObject::Connection m_letterConnection;
    VirtualFlexControlWheel* m_wheel{nullptr};
    QLabel* m_captureHint{nullptr};
    QLabel* m_sliceLabel{nullptr};
    QLabel* m_frequencyLabel{nullptr};
    QLabel* m_stepLabel{nullptr};
    QLabel* m_modeLabel{nullptr};
    QLabel* m_pushHintLabel{nullptr};
    QPushButton* m_physicalButton{nullptr};
    QPushButton* m_compactButton{nullptr};
    QPushButton* m_externalSpinButton{nullptr};
    QPushButton* m_reverseButton{nullptr};
    QPushButton* m_pushButton{nullptr};
    QSlider* m_spinSlider{nullptr};
    QShortcut* m_releaseShortcut{nullptr};
    QFrame* m_controlStrip{nullptr};
    QFrame* m_deviceFrame{nullptr};
    QFrame* m_knobPanel{nullptr};
    QFrame* m_pushActionsFrame{nullptr};
    QLabel* m_singleTapHeader{nullptr};
    QLabel* m_doubleTapHeader{nullptr};
    QVector<QLabel*> m_auxDots;
    QVector<QPushButton*> m_auxButtons;
    QVector<QComboBox*> m_auxCombos;
    QVector<QComboBox*> m_auxDoubleCombos;
    QVector<QWidget*> m_compactHiddenWidgets;
    QComboBox* m_pushSingleCombo{nullptr};
    QComboBox* m_pushDoubleCombo{nullptr};
    double m_lastSliceFrequencyMhz{0.0};
    bool m_haveLastSliceFrequency{false};
    int m_stepHz{100};
    int m_activeAux{-1};
    bool m_ignoreNextReject{false};
};

} // namespace AetherSDR
