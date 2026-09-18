#include "AetherRxDialog.h"
#include "AetherDspWidget.h"
#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "EditorFramelessTitleBar.h"
#include "CompactMetrics.h"
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
#include <QResizeEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

// The size this window opens at, every time — see showEvent().
constexpr QSize kLaunchSize(720, 480);

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
//
// `keepTitle` decides whether the name plate goes too. A page showing one panel
// has already named it in the tab, and a second copy of "EQ" above the graph is
// a row of pixels saying nothing; a page stacking two panels keeps both plates,
// because there the titles are what tell them apart.
void tidyEmbeddedPanel(QWidget* panel, bool keepTitle = false)
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
            if (keepTitle) {
                tb->setControlsVisible(false);
                recolour(tb);
            } else {
                tb->hide();
            }
            break;
        }
    }
}

// Holds one stage panel and shrinks its graphics until it fits the page.
//
// These panels were drawn for a window twice the size of this one, and they
// spend that space on knobs, curves and meters rather than on text. So the
// page keeps every label at the size it was meant to be read at and takes the
// graphics down instead (CompactMetrics), rather than scaling the panel whole
// and shrinking the type with it.
//
// Nothing scrolls: whatever is on this page is all of it.
class StagePage final : public QWidget {
public:
    explicit StagePage(QWidget* panel, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_panel(panel)
        , m_metrics(panel)
        , m_designed(panel->size().expandedTo(panel->minimumSizeHint()))
    {
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(0);
        col->addWidget(panel);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        refit();
    }

    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        // Panels finish sizing themselves in showForRx(), which runs after
        // this page is built, so the first honest measurement is here.
        refit();
    }

private:
    void refit()
    {
        if (!m_panel || m_metrics.isEmpty() || m_refitting) return;
        const QSize room = size();
        if (room.isEmpty() || m_designed.isEmpty()) return;

        m_refitting = true;
        // Start from what the designed size asks for, then walk down until the
        // panel's own minimum fits the page. A couple of passes settles it:
        // the graphics do not shrink linearly, because the text between them
        // does not shrink at all.
        qreal factor = std::min({qreal(1.0),
                                 qreal(room.width()) / m_designed.width(),
                                 qreal(room.height()) / m_designed.height()});
        for (int pass = 0; pass < 4; ++pass) {
            m_metrics.apply(factor);
            m_panel->ensurePolished();
            const QSize needs = m_panel->minimumSizeHint();
            if ((needs.width() <= room.width() && needs.height() <= room.height())
                || factor <= CompactMetrics::kMinFactor) {
                break;
            }
            factor *= 0.9;
        }
        m_refitting = false;
    }

    QWidget*       m_panel{nullptr};
    CompactMetrics m_metrics;
    QSize          m_designed;
    bool           m_refitting{false};
};

} // namespace

AetherRxDialog::AetherRxDialog(AudioEngine* audio, QWidget* parent)
    // The geometry key carries a version because the default size changed
    // after the window had shipped once: a stored geometry always wins over
    // resize(), so anyone who had opened it would have kept the old 1420x900
    // for ever and never seen the new default. Bumping the key retires those
    // saved rectangles; the window persists its size again from here.
    : PersistentDialog("AetherRX", "AetherRxDialogGeometry2", parent)
{
    theme::setContainer(this, QStringLiteral("dialog/aetherRx"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }");
    // Opens at a size that sits on the desktop rather than filling it. The EQ
    // and waveform pages are wider than this at their natural size and scroll
    // to fit; that is the trade for a window you can put somewhere. The
    // minimum is what the narrowest page (AetherNR's two-column radio row)
    // needs beside the tab column, and geometry persists, so a window resized
    // once reopens where it was left.
    setMinimumSize(600, 400);
    resize(kLaunchSize);

    auto* body = new QHBoxLayout(bodyWidget());
    body->setContentsMargins(8, 8, 8, 8);
    body->setSpacing(8);

    // -- Left tab column ------------------------------------------------
    m_tabsFrame = new QFrame(this);
    m_tabsFrame->setObjectName(QStringLiteral("TabsFrame"));
    m_tabsFrame->setAttribute(Qt::WA_StyledBackground, true);
    // The chrome sheet lives on the tab column rather than on the window. A
    // Qt stylesheet cascades to every descendant, and its 14 px base font
    // reaches inside the stage panels, which were drawn against the
    // application font: three points more is enough to push the minus sign
    // out of a knob's 76 px value editor. The AetherNR body applies the same
    // sheet to itself, so the only thing that loses by this is nothing.
    m_tabsFrame->setStyleSheet(ModemChrome::styleSheet(ModemChrome::Scale::Dialog));
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
    // Straight through: the panel asks, MainWindow decides what a width means
    // for the mode the slice is in.
    connect(m_eq, &StripEqPanel::rxFilterWidthRequested,
            this, &AetherRxDialog::rxFilterWidthRequested);
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

    // One panel to a page: the tab already names it, so the plate comes off.
    for (QWidget* panel : {static_cast<QWidget*>(m_gate),
                           static_cast<QWidget*>(m_eq),
                           static_cast<QWidget*>(m_comp),
                           static_cast<QWidget*>(m_tube),
                           static_cast<QWidget*>(m_voice)}) {
        tidyEmbeddedPanel(panel);
    }
    // The Out page stacks two, and their plates are what say which is which.
    tidyEmbeddedPanel(m_output, /*keepTitle=*/true);
    tidyEmbeddedPanel(m_waveform, /*keepTitle=*/true);

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

// A stage panel that always fits, by shrinking its knobs and curves rather
// than its labels. No scrollbars, so a control is never half a drag off the
// edge of the page.
QWidget* AetherRxDialog::buildStagePage(QWidget* panel)
{
    return new StagePage(panel);
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

// Opens at kLaunchSize every time, whatever is stored.
//
// Deliberate and temporary: the size this window wants is still being settled,
// and a restored geometry wins over the default in the constructor, so without
// this the first size anyone happened to leave it at is the size they keep.
// The position is still restored and still saved — only the size is pinned.
// To give the size back to the operator, delete this override; the geometry
// key underneath it has been recording all along.
void AetherRxDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    if (size() != kLaunchSize) {
        resize(kLaunchSize);
    }
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
