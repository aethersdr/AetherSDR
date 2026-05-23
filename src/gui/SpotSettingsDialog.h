#pragma once

#include <QDialog>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QColor>

namespace MasterSDR {

class RadioModel;

class SpotSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SpotSettingsDialog(RadioModel* model, QWidget* parent = nullptr);
    void setTotalSpots(int count) { m_totalSpotsLabel->setText(QString::number(count)); }

signals:
    void settingsChanged();  // live preview — emitted on every change

private:
    void updateColorSwatch(QPushButton* btn, const QColor& color);

    RadioModel* m_model;

    QPushButton* m_spotsToggle;
    QPushButton* m_memoriesToggle;
    QSlider*     m_levelsSlider;
    QLabel*      m_levelsValue;
    QSlider*     m_positionSlider;
    QLabel*      m_positionValue;
    QSlider*     m_fontSizeSlider;
    QLabel*      m_fontSizeValue;
    QPushButton* m_overrideColorsToggle;
    QPushButton* m_colorPickerBtn;
    QPushButton* m_overrideBgEnabled;
    QPushButton* m_overrideBgAuto;
    QPushButton* m_bgColorPickerBtn;
    QSlider*     m_bgOpacitySlider;
    QLabel*      m_bgOpacityValue;
    QPushButton* m_spotLinesToggle;
    QLabel*      m_totalSpotsLabel;

    bool   m_spotsEnabled{true};
    bool   m_memoriesEnabled{false};
    bool   m_overrideColors{false};
    bool   m_overrideBg{true};
    bool   m_overrideBgAutoMode{true};
    bool   m_spotShowLines{true};
    QColor m_spotColor{Qt::yellow};
    QColor m_bgColor{Qt::black};
    int    m_bgOpacity{48};
};

} // namespace MasterSDR
