#include "MasterDspDialog.h"
#include "MasterDspWidget.h"

#include <QVBoxLayout>

namespace MasterSDR {

MasterDspDialog::MasterDspDialog(AudioEngine* audio, QWidget* parent)
    : PersistentDialog("MasterDsp Settings", "MasterDspDialogGeometry", parent)
{
    setStyleSheet("QDialog { background: #0f0f1a; color: #c8d8e8; }");

    auto* body = new QVBoxLayout(bodyWidget());
    body->setSpacing(0);

    m_widget = new MasterDspWidget(audio, this);
    // Scale all internal fonts up to 13 px to match the VFO DSP toggle
    // row.  Applet path leaves this off and renders at the original sizes.
    m_widget->setDialogMode(true);
    body->addWidget(m_widget);

    // Forward every parameter-change signal so existing connections to
    // MasterDspDialog::* keep working unchanged.
    connect(m_widget, &MasterDspWidget::nr2GainMaxChanged,
            this,    &MasterDspDialog::nr2GainMaxChanged);
    connect(m_widget, &MasterDspWidget::nr2GainSmoothChanged,
            this,    &MasterDspDialog::nr2GainSmoothChanged);
    connect(m_widget, &MasterDspWidget::nr2QsppChanged,
            this,    &MasterDspDialog::nr2QsppChanged);
    connect(m_widget, &MasterDspWidget::nr2GainMethodChanged,
            this,    &MasterDspDialog::nr2GainMethodChanged);
    connect(m_widget, &MasterDspWidget::nr2NpeMethodChanged,
            this,    &MasterDspDialog::nr2NpeMethodChanged);
    connect(m_widget, &MasterDspWidget::nr2AeFilterChanged,
            this,    &MasterDspDialog::nr2AeFilterChanged);
    connect(m_widget, &MasterDspWidget::mnrEnabledChanged,
            this,    &MasterDspDialog::mnrEnabledChanged);
    connect(m_widget, &MasterDspWidget::mnrStrengthChanged,
            this,    &MasterDspDialog::mnrStrengthChanged);
    connect(m_widget, &MasterDspWidget::dfnrAttenLimitChanged,
            this,    &MasterDspDialog::dfnrAttenLimitChanged);
    connect(m_widget, &MasterDspWidget::dfnrPostFilterBetaChanged,
            this,    &MasterDspDialog::dfnrPostFilterBetaChanged);
    connect(m_widget, &MasterDspWidget::nr4ReductionChanged,
            this,    &MasterDspDialog::nr4ReductionChanged);
    connect(m_widget, &MasterDspWidget::nr4SmoothingChanged,
            this,    &MasterDspDialog::nr4SmoothingChanged);
    connect(m_widget, &MasterDspWidget::nr4WhiteningChanged,
            this,    &MasterDspDialog::nr4WhiteningChanged);
    connect(m_widget, &MasterDspWidget::nr4AdaptiveNoiseChanged,
            this,    &MasterDspDialog::nr4AdaptiveNoiseChanged);
    connect(m_widget, &MasterDspWidget::nr4NoiseMethodChanged,
            this,    &MasterDspDialog::nr4NoiseMethodChanged);
    connect(m_widget, &MasterDspWidget::nr4MaskingDepthChanged,
            this,    &MasterDspDialog::nr4MaskingDepthChanged);
    connect(m_widget, &MasterDspWidget::nr4SuppressionChanged,
            this,    &MasterDspDialog::nr4SuppressionChanged);
}

void MasterDspDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void MasterDspDialog::selectTab(const QString& name)
{
    if (m_widget) m_widget->selectTab(name);
}

} // namespace MasterSDR
