#pragma once

#include <QWidget>
#include <QVector>
#include <functional>

class QVBoxLayout;
class QSlider;
class QLabel;
class QPushButton;
class QRadioButton;
class QButtonGroup;
class QCheckBox;

namespace AetherSDR {

// DspParamPopup — floating right-click popup for quick NR parameter access.
// Shows essential controls with an "AetherDSP Settings..." link to the full dialog.
// Auto-dismisses on click outside (like the band/ant/dsp sub-panels).
class DspParamPopup : public QWidget {
    Q_OBJECT

public:
    struct SliderControl {
        QLabel* label{nullptr};
        QSlider* slider{nullptr};
        QLabel* valueLabel{nullptr};

        void setEnabled(bool enabled) const;
        void setToolTip(const QString& tooltip) const;
    };

    explicit DspParamPopup(QWidget* parent = nullptr);

    // Add controls programmatically before showing
    void addRadioGroup(const QString& label, const QStringList& options,
                       int defaultIdx, std::function<void(int)> onChange);
    SliderControl addSlider(const QString& label, int min, int max,
                            int defaultVal,
                            std::function<QString(int)> format,
                            std::function<void(int)> onChange,
                            bool enabled = true,
                            const QString& tooltip = QString());
    QCheckBox* addCheckbox(const QString& label, bool defaultVal,
                           std::function<void(bool)> onChange);

    // Finalize layout (adds AetherDSP Settings + Reset buttons)
    void finalize(std::function<void()> onMore, std::function<void()> onReset);

    // Show anchored to a button position
    void showAt(const QPoint& globalPos);

protected:
    bool event(QEvent* ev) override;

private:
    QVBoxLayout* m_layout{nullptr};
    QVector<std::function<void()>> m_resetters;
};

} // namespace AetherSDR
