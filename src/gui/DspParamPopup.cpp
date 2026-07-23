#include "DspParamPopup.h"
#include "GuardedSlider.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QRadioButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QEvent>
#include <QApplication>
#include <QScreen>
#include "core/ThemeManager.h"

namespace AetherSDR {

static const QString kPopupStyle = QStringLiteral(
    "QWidget#DspParamPopup { background: rgba(15, 15, 26, 240);"
    "  border: 1px solid #304050; border-radius: 4px; }"
    "QLabel { color: #8090a0; font-size: 11px; border: none; }"
    "QRadioButton { color: #c8d8e8; font-size: 11px; }"
    "QCheckBox { color: #c8d8e8; font-size: 11px; }"
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
    "  border-radius: 3px; padding: 3px 8px; font-size: 11px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }");

DspParamPopup::DspParamPopup(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setObjectName("DspParamPopup");
    setStyleSheet(kPopupStyle);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(220);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(10, 8, 10, 8);
    m_layout->setSpacing(6);
}

void DspParamPopup::SliderControl::setEnabled(bool enabled) const
{
    label->setEnabled(enabled);
    slider->setEnabled(enabled);
    valueLabel->setEnabled(enabled);
}

void DspParamPopup::SliderControl::setToolTip(
    const QString& tooltip) const
{
    label->setToolTip(tooltip);
    slider->setToolTip(tooltip);
    valueLabel->setToolTip(tooltip);
}

void DspParamPopup::addRadioGroup(const QString& label, const QStringList& options,
                                   int defaultIdx, std::function<void(int)> onChange)
{
    auto* lbl = new QLabel(label);
    AetherSDR::ThemeManager::instance().applyStyleSheet(lbl, "QLabel { color: {{color.text.secondary}}; font-size: 10px; font-weight: bold; }");
    m_layout->addWidget(lbl);

    auto* row = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    for (int i = 0; i < options.size(); ++i) {
        auto* rb = new QRadioButton(options[i]);
        group->addButton(rb, i);
        row->addWidget(rb);
    }
    if (auto* btn = group->button(defaultIdx))
        btn->setChecked(true);
    m_layout->addLayout(row);

    connect(group, &QButtonGroup::idClicked, this, [onChange](int id) {
        if (onChange) onChange(id);
    });

    m_resetters.append([group, defaultIdx]() {
        if (auto* btn = group->button(defaultIdx))
            btn->setChecked(true);
    });
}

DspParamPopup::SliderControl DspParamPopup::addSlider(
    const QString& label, int min, int max, int defaultVal,
    std::function<QString(int)> format,
    std::function<void(int)> onChange,
    bool enabled,
    const QString& tooltip)
{
    auto* row = new QHBoxLayout;

    auto* lbl = new QLabel(label);
    lbl->setFixedWidth(90);
    row->addWidget(lbl);

    auto* slider = new GuardedSlider(Qt::Horizontal);
    slider->setRange(min, max);
    slider->setValue(defaultVal);
    slider->setDragValueFormatter(format);
    applyPrimarySliderStyle(slider);
    row->addWidget(slider);

    auto* val = new QLabel(format ? format(defaultVal) : QString::number(defaultVal));
    AetherSDR::ThemeManager::instance().applyStyleSheet(val, "QLabel { color: {{color.text.primary}}; min-width: 36px; }");
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(val);

    const SliderControl control{lbl, slider, val};
    control.setEnabled(enabled);
    if (!tooltip.isEmpty()) {
        control.setToolTip(tooltip);
    }

    m_layout->addLayout(row);

    connect(slider, &QSlider::valueChanged, this, [val, format, onChange](int v) {
        if (format) val->setText(format(v));
        if (onChange) onChange(v);
    });

    m_resetters.append([slider, defaultVal]() {
        slider->setValue(defaultVal);
    });

    return control;
}

QCheckBox* DspParamPopup::addCheckbox(
    const QString& label, bool defaultVal,
    std::function<void(bool)> onChange)
{
    auto* cb = new QCheckBox(label);
    cb->setChecked(defaultVal);
    m_layout->addWidget(cb);

    connect(cb, &QCheckBox::toggled, this, [onChange](bool on) {
        if (onChange) onChange(on);
    });

    m_resetters.append([cb, defaultVal]() {
        cb->setChecked(defaultVal);
    });

    return cb;
}

void DspParamPopup::finalize(std::function<void()> onMore, std::function<void()> onReset)
{
    m_layout->addSpacing(4);

    auto* btnRow = new QHBoxLayout;

    auto* moreBtn = new QPushButton("AetherDSP Settings...");
    connect(moreBtn, &QPushButton::clicked, this, [this, onMore]() {
        if (onMore) onMore();
        close();
    });
    btnRow->addWidget(moreBtn);

    auto* resetBtn = new QPushButton("Reset");
    connect(resetBtn, &QPushButton::clicked, this, [this, onReset]() {
        for (auto& fn : m_resetters) fn();
        if (onReset) onReset();
    });
    btnRow->addWidget(resetBtn);

    m_layout->addLayout(btnRow);
}

void DspParamPopup::showAt(const QPoint& globalPos)
{
    adjustSize();
    // Position above-right of the click point, keep on screen
    QPoint pos = globalPos;
    QRect screen = QApplication::primaryScreen()->availableGeometry();
    if (pos.x() + width() > screen.right())
        pos.setX(screen.right() - width());
    if (pos.y() + height() > screen.bottom())
        pos.setY(globalPos.y() - height());
    move(pos);
    show();
}

bool DspParamPopup::event(QEvent* ev)
{
    // Qt::Popup auto-closes on click outside — no custom handling needed
    return QWidget::event(ev);
}

} // namespace AetherSDR
