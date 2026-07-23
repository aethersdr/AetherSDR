#include "AetherDspDialog.h"
#include "AetherDspWidget.h"

#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

AetherDspDialog::AetherDspDialog(AudioEngine* audio, QWidget* parent)
    : PersistentDialog("AetherDSP Settings", "AetherDspDialogGeometry", parent)
{
    theme::setContainer(this, QStringLiteral("dialog/aetherDsp"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");

    auto* body = new QVBoxLayout(bodyWidget());
    body->setSpacing(0);

    m_widget = new AetherDspWidget(audio, this);
    // Scale all internal fonts up to 13 px to match the VFO DSP toggle
    // row.  Applet path leaves this off and renders at the original sizes.
    m_widget->setDialogMode(true);
    body->addWidget(m_widget);

    // Forward every parameter-change signal so existing connections to
    // AetherDspDialog::* keep working unchanged.
    connect(m_widget, &AetherDspWidget::nr2GainMaxChanged,
            this,    &AetherDspDialog::nr2GainMaxChanged);
    connect(m_widget, &AetherDspWidget::nr2GainFloorChanged,
            this,    &AetherDspDialog::nr2GainFloorChanged);
    connect(m_widget, &AetherDspWidget::nr2GainSmoothChanged,
            this,    &AetherDspDialog::nr2GainSmoothChanged);
    connect(m_widget, &AetherDspWidget::nr2QsppChanged,
            this,    &AetherDspDialog::nr2QsppChanged);
    connect(m_widget, &AetherDspWidget::nr2GainMethodChanged,
            this,    &AetherDspDialog::nr2GainMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2NpeMethodChanged,
            this,    &AetherDspDialog::nr2NpeMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2AeFilterChanged,
            this,    &AetherDspDialog::nr2AeFilterChanged);
    connect(m_widget, &AetherDspWidget::nr2UseOriginalGeometryChanged,
            this,    &AetherDspDialog::nr2UseOriginalGeometryChanged);
    connect(m_widget, &AetherDspWidget::mnrEnabledChanged,
            this,    &AetherDspDialog::mnrEnabledChanged);
    connect(m_widget, &AetherDspWidget::mnrStrengthChanged,
            this,    &AetherDspDialog::mnrStrengthChanged);
    connect(m_widget, &AetherDspWidget::dfnrAttenLimitChanged,
            this,    &AetherDspDialog::dfnrAttenLimitChanged);
    connect(m_widget, &AetherDspWidget::dfnrPostFilterBetaChanged,
            this,    &AetherDspDialog::dfnrPostFilterBetaChanged);
    connect(m_widget, &AetherDspWidget::nr4ReductionChanged,
            this,    &AetherDspDialog::nr4ReductionChanged);
    connect(m_widget, &AetherDspWidget::nr4SmoothingChanged,
            this,    &AetherDspDialog::nr4SmoothingChanged);
    connect(m_widget, &AetherDspWidget::nr4WhiteningChanged,
            this,    &AetherDspDialog::nr4WhiteningChanged);
    connect(m_widget, &AetherDspWidget::nr4AdaptiveNoiseChanged,
            this,    &AetherDspDialog::nr4AdaptiveNoiseChanged);
    connect(m_widget, &AetherDspWidget::nr4NoiseMethodChanged,
            this,    &AetherDspDialog::nr4NoiseMethodChanged);
    connect(m_widget, &AetherDspWidget::nr4MaskingDepthChanged,
            this,    &AetherDspDialog::nr4MaskingDepthChanged);
    connect(m_widget, &AetherDspWidget::nr4SuppressionChanged,
            this,    &AetherDspDialog::nr4SuppressionChanged);
}

void AetherDspDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void AetherDspDialog::selectTab(const QString& name)
{
    if (m_widget) m_widget->selectTab(name);
}

} // namespace AetherSDR
