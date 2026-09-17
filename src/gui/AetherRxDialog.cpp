#include "AetherRxDialog.h"
#include "AetherDspWidget.h"
#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "EditorFramelessTitleBar.h"
#include "ModemChrome.h"
#include "StripCompPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripRxOutputPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// One tab in the left-hand bar. Same chrome as the method strip inside the
// AetherNR page -- checkable, property-selected, sized by the layout -- but
// left-aligned, because a column of centred labels of different lengths reads
// as ragged where a row of them reads as even.
QPushButton* makeStageTab(const QString& text)
{
    auto* b = new QPushButton(text);
    b->setProperty("chrome", "tab");
    b->setCheckable(true);
    b->setFlat(true);
    b->setStyleSheet(QStringLiteral("text-align: left;"));
    b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    b->setMinimumHeight(28);
    return b;
}

// The embedded panels each ship their own window chrome -- a title bar with a
// min/max/close trio, and a legacy band colour from when they were floating
// editors. Inside a host window the trio does nothing, so strip it, the same
// way the Aetherial strip does for its own embedded panels.
void tidyEmbeddedPanel(QWidget* panel)
{
    if (!panel) return;
    const auto recolour = [](QWidget* w) {
        QString sheet = w->styleSheet();
        if (sheet.contains(QLatin1String("#08121d"))) {
            sheet.replace(QLatin1String("#08121d"),
                          QLatin1String(ModemChrome::Colour::Background));
            w->setStyleSheet(sheet);
        }
    };
    recolour(panel);
    for (QObject* child : panel->children()) {
        // dynamic_cast rather than findChild: EditorFramelessTitleBar has no
        // Q_OBJECT macro.
        if (auto* tb = dynamic_cast<EditorFramelessTitleBar*>(child)) {
            tb->setControlsVisible(false);
            recolour(tb);
            break;
        }
    }
}

} // namespace

AetherRxDialog::AetherRxDialog(AudioEngine* audio, QWidget* parent)
    : PersistentDialog("AetherRX", "AetherRxDialogGeometry", parent)
{
    theme::setContainer(this, QStringLiteral("dialog/aetherRx"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");
    setStyleSheet(ModemChrome::styleSheet(ModemChrome::Scale::Dialog));
    // Opens at a size that sits on the desktop rather than filling it. The EQ
    // and waveform pages are wider than this at their natural size and scroll
    // to fit; that is the trade for a window you can put somewhere. The
    // minimum is what the narrowest page (AetherNR's two-column radio row)
    // needs beside the tab column, and geometry persists, so a window resized
    // once reopens where it was left.
    setMinimumSize(600, 400);
    resize(710, 450);

    auto* body = new QHBoxLayout(bodyWidget());
    body->setContentsMargins(8, 8, 8, 8);
    body->setSpacing(8);

    // -- Left tab column ------------------------------------------------
    m_tabsFrame = new QFrame(this);
    m_tabsFrame->setObjectName(QStringLiteral("TabsFrame"));
    m_tabsFrame->setAttribute(Qt::WA_StyledBackground, true);
    m_tabsFrame->setFixedWidth(132);
    auto* tabsBox = new QVBoxLayout(m_tabsFrame);
    tabsBox->setContentsMargins(6, 6, 6, 6);
    tabsBox->setSpacing(2);
    body->addWidget(m_tabsFrame);

    m_tabGroup = new QButtonGroup(this);
    m_tabGroup->setExclusive(true);

    m_stack = new QStackedWidget(this);
    body->addWidget(m_stack, 1);

    connect(m_tabGroup, &QButtonGroup::idClicked,
            m_stack, &QStackedWidget::setCurrentIndex);

    // -- AetherNR: the noise-reduction body, method strip and all --------
    m_widget = new AetherDspWidget(audio, this);
    m_widget->setDialogMode(true);
    addStage(Nr, QStringLiteral("AetherNR"), m_widget);

    // -- The RX chain, in the order the signal meets it ------------------
    m_gate = new StripGatePanel(audio, this);
    addStage(Gate, QStringLiteral("AGC-G"), buildStagePage(m_gate));

    m_eq = new StripEqPanel(audio, this);
    addStage(Eq, QStringLiteral("EQ"), buildStagePage(m_eq));

    m_comp = new StripCompPanel(audio, this);
    addStage(Comp, QStringLiteral("AGC-C"), buildStagePage(m_comp));

    m_tube = new StripTubePanel(audio, this);
    addStage(Tube, QStringLiteral("Tube"), buildStagePage(m_tube));

    m_voice = new StripPuduPanel(audio, this);
    addStage(Voice, QStringLiteral("AetherVoice"), buildStagePage(m_voice));

    // Output and waveform share the last tab: the meter is what you read and
    // the waveform is what you read it against, so splitting them would mean
    // switching tabs to answer one question.
    {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(8);
        m_output = new StripRxOutputPanel(audio, this);
        m_waveform = new StripWaveformPanel(audio, this);
        col->addWidget(m_output, 0, Qt::AlignTop);
        col->addWidget(m_waveform, 1);
        addStage(Output, QStringLiteral("Out"), page);
    }

    tabsBox->addStretch(1);

    // Pin every panel to its RX engine instance. Without this the EQ canvas
    // has no engine to enumerate bands for and collapses to its "(no EQ
    // connected)" placeholder.
    if (m_gate)     m_gate->showForRx();
    if (m_eq)       m_eq->showForPath(ClientEqApplet::Path::Rx);
    if (m_comp)     m_comp->showForRx();
    if (m_tube)     m_tube->showForRx();
    if (m_voice)    m_voice->showForRx();
    if (m_output)   m_output->showForRx();
    if (m_waveform) m_waveform->showForRx();

    for (QWidget* panel : {static_cast<QWidget*>(m_gate),
                           static_cast<QWidget*>(m_eq),
                           static_cast<QWidget*>(m_comp),
                           static_cast<QWidget*>(m_tube),
                           static_cast<QWidget*>(m_voice),
                           static_cast<QWidget*>(m_output),
                           static_cast<QWidget*>(m_waveform)}) {
        tidyEmbeddedPanel(panel);
    }

    if (auto* first = m_tabGroup->button(Nr)) {
        first->setChecked(true);
    }
    m_stack->setCurrentIndex(Nr);

    // Forward every parameter-change signal so existing connections to
    // AetherRxDialog::* keep working unchanged.
    connect(m_widget, &AetherDspWidget::nr2GainMaxChanged,
            this,    &AetherRxDialog::nr2GainMaxChanged);
    connect(m_widget, &AetherDspWidget::nr2GainFloorChanged,
            this,    &AetherRxDialog::nr2GainFloorChanged);
    connect(m_widget, &AetherDspWidget::nr2GainSmoothChanged,
            this,    &AetherRxDialog::nr2GainSmoothChanged);
    connect(m_widget, &AetherDspWidget::nr2QsppChanged,
            this,    &AetherRxDialog::nr2QsppChanged);
    connect(m_widget, &AetherDspWidget::nr2GainMethodChanged,
            this,    &AetherRxDialog::nr2GainMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2NpeMethodChanged,
            this,    &AetherRxDialog::nr2NpeMethodChanged);
    connect(m_widget, &AetherDspWidget::nr2AeFilterChanged,
            this,    &AetherRxDialog::nr2AeFilterChanged);
    connect(m_widget, &AetherDspWidget::nr2Post2SettingsChanged,
            this,    &AetherRxDialog::nr2Post2SettingsChanged);
    connect(m_widget, &AetherDspWidget::mnrStrengthChanged,
            this,    &AetherRxDialog::mnrStrengthChanged);
    connect(m_widget, &AetherDspWidget::rn2DryMixChanged,
            this,    &AetherRxDialog::rn2DryMixChanged);
    connect(m_widget, &AetherDspWidget::dfnrAttenLimitChanged,
            this,    &AetherRxDialog::dfnrAttenLimitChanged);
    connect(m_widget, &AetherDspWidget::dfnrPostFilterBetaChanged,
            this,    &AetherRxDialog::dfnrPostFilterBetaChanged);
    connect(m_widget, &AetherDspWidget::nr4ReductionChanged,
            this,    &AetherRxDialog::nr4ReductionChanged);
    connect(m_widget, &AetherDspWidget::nr4SmoothingChanged,
            this,    &AetherRxDialog::nr4SmoothingChanged);
    connect(m_widget, &AetherDspWidget::nr4WhiteningChanged,
            this,    &AetherRxDialog::nr4WhiteningChanged);
    connect(m_widget, &AetherDspWidget::nr4AdaptiveNoiseChanged,
            this,    &AetherRxDialog::nr4AdaptiveNoiseChanged);
    connect(m_widget, &AetherDspWidget::nr4NoiseMethodChanged,
            this,    &AetherRxDialog::nr4NoiseMethodChanged);
    connect(m_widget, &AetherDspWidget::nr4MaskingDepthChanged,
            this,    &AetherRxDialog::nr4MaskingDepthChanged);
    connect(m_widget, &AetherDspWidget::nr4SuppressionChanged,
            this,    &AetherRxDialog::nr4SuppressionChanged);
}

// A stage panel sized to its own content, pinned to the top of the page and
// scrollable when the window is shorter than the panel wants to be. The panels
// were written as floating editors with fixed natural heights, so without the
// scroller a short window clips the bottom of one with no way to reach it.
QWidget* AetherRxDialog::buildStagePage(QWidget* panel)
{
    auto* host = new QWidget;
    auto* col = new QVBoxLayout(host);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    col->addWidget(panel, 0, Qt::AlignTop);
    col->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidget(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    return scroll;
}

void AetherRxDialog::addStage(Stage stage, const QString& label, QWidget* page)
{
    auto* tab = makeStageTab(label);
    tab->setObjectName(QStringLiteral("aetherRxTab") + label);
    tab->setAccessibleName(label + QStringLiteral(" receive stage"));
    m_tabGroup->addButton(tab, stage);
    qobject_cast<QVBoxLayout*>(m_tabsFrame->layout())->addWidget(tab);

    const int index = m_stack->addWidget(page);
    Q_ASSERT(index == stage);   // ids double as stack indices
    Q_UNUSED(index);
}

void AetherRxDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void AetherRxDialog::selectTab(const QString& name)
{
    static const struct { Stage stage; const char* label; } kStages[] = {
        {Nr, "AetherNR"}, {Gate, "AGC-G"},  {Eq, "EQ"},      {Comp, "AGC-C"},
        {Tube, "Tube"},   {Voice, "AetherVoice"}, {Output, "Out"},
    };
    for (const auto& entry : kStages) {
        if (name.compare(QLatin1String(entry.label), Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (auto* tab = m_tabGroup->button(entry.stage)) tab->setChecked(true);
        if (m_stack) m_stack->setCurrentIndex(entry.stage);
        return;
    }

    // Not a stage name, so it is a noise-reduction method: show the NR tab and
    // let the body pick the method. Every caller that predates this window
    // passes one of these.
    if (auto* tab = m_tabGroup->button(Nr)) tab->setChecked(true);
    if (m_stack) m_stack->setCurrentIndex(Nr);
    if (m_widget) m_widget->selectTab(name);
}

} // namespace AetherSDR
