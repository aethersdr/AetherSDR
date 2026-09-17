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
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QPushButton>
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

// Holds one stage panel and scales it down to whatever room the window gives.
//
// The panels were written as floating editors and carry the natural size that
// implies: the EQ alone wants about 900x520, because ten bands of icons and
// per-band frequency/gain/Q readouts do not compress -- squeezing its columns
// turns "1.50 kHz" into "50 kHz", which is worse than not showing it. Scaling
// keeps the whole panel laid out as designed, just smaller, and because the
// scene draws the widget through QPainter rather than blitting a pixmap, the
// text and curves stay sharp at any factor.
//
// Never scales up: a panel that already fits is left at its natural size, so
// this costs nothing on the pages that do not need it.
class FitToViewHost final : public QGraphicsView {
public:
    explicit FitToViewHost(QWidget* panel, QWidget* parent = nullptr)
        : QGraphicsView(parent)
        , m_panel(panel)
    {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                       | QPainter::SmoothPixmapTransform);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setBackgroundBrush(Qt::NoBrush);
        setStyleSheet(QStringLiteral("QGraphicsView { background: transparent; }"));
        auto* scene = new QGraphicsScene(this);
        setScene(scene);
        // The panel must arrive parentless: QGraphicsScene::addWidget only
        // adopts a top-level widget, and one that is already someone's child
        // is left exactly where it was -- which draws every stage panel on top
        // of every other one, with no error anywhere.
        Q_ASSERT(panel->parentWidget() == nullptr);
        // The size the panel was designed at, taken before anything here has
        // touched it: every one of these panels resizes itself to its own
        // kDefaultWidth/kDefaultHeight in its constructor, which is the size
        // its layout was drawn for. Captured once and never re-measured --
        // asking the panel again after scaling it makes the answer a function
        // of the last answer, and the two chase each other a few pixels at a
        // time without ever settling.
        m_natural = panel->size()
                        .expandedTo(panel->minimumSizeHint())
                        .expandedTo(QSize(1, 1));
        m_proxy = scene->addWidget(panel);
        // A panel's minimum size can grow after construction -- the gate's
        // curve view and the EQ's band rows size themselves once the engine
        // hands them something to draw. Nothing resizes this host when that
        // happens, so without watching for it the page keeps the scale it was
        // measured at and the panel grows off the bottom of it.
        panel->installEventFilter(this);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        // Resize as well as LayoutRequest: these panels restore their own
        // saved geometry in showForRx(), after this host has already sized
        // them, and a panel that makes itself taller than the scene rect is
        // simply clipped by it -- knobs vanish off the bottom with no
        // scrollbar and nothing in the log.
        if (watched == m_panel && !m_inRescale
            && (event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::Resize)) {
            rescale();
        }
        return QGraphicsView::eventFilter(watched, event);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QGraphicsView::resizeEvent(event);
        rescale();
    }

    void showEvent(QShowEvent* event) override
    {
        QGraphicsView::showEvent(event);
        // Panels size themselves in showForRx(), which runs after this host is
        // built, so the first honest measurement is here.
        rescale();
    }

private:
    void rescale()
    {
        if (!m_proxy || !scene() || m_inRescale) return;
        // Resizing the proxy lays the panel out again, which posts another
        // LayoutRequest straight back at the filter above.
        m_inRescale = true;
        const QSize natural = m_natural;
        const QSize view = viewport()->size();
        if (view.isEmpty()) {
            m_inRescale = false;
            return;
        }

        // Scale only as far as the tighter of the two axes demands, and never
        // past 1: a panel that fits is shown at its own size.
        const qreal scale = std::min({qreal(1.0),
                                      qreal(view.width()) / natural.width(),
                                      qreal(view.height()) / natural.height()});

        // Lay the panel out at exactly the size it was designed for and scale
        // that. Stretching it to fill the page instead looks tidier on the EQ,
        // but these panels do not all survive a size they were not drawn at:
        // the gate reports a 285 px minimum height and then needs 700, and
        // Qt neither clips nor complains -- the knob row simply ends up past
        // the bottom edge. The designed size is the one size every panel is
        // known to render correctly.
        const QSize laidOut = natural;
        m_proxy->resize(laidOut);

        scene()->setSceneRect(0, 0, laidOut.width(), laidOut.height());
        setTransform(QTransform::fromScale(scale, scale));
        m_inRescale = false;
    }

    QWidget*              m_panel{nullptr};
    QSize                 m_natural;
    QGraphicsProxyWidget* m_proxy{nullptr};
    bool                  m_inRescale{false};
};

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
    resize(720, 480);

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
    m_gate = new StripGatePanel(audio, nullptr);
    addStage(Gate, QStringLiteral("AGC-G"), buildStagePage(m_gate));

    m_eq = new StripEqPanel(audio, nullptr);
    addStage(Eq, QStringLiteral("EQ"), buildStagePage(m_eq));

    m_comp = new StripCompPanel(audio, nullptr);
    addStage(Comp, QStringLiteral("AGC-C"), buildStagePage(m_comp));

    m_tube = new StripTubePanel(audio, nullptr);
    addStage(Tube, QStringLiteral("Tube"), buildStagePage(m_tube));

    m_voice = new StripPuduPanel(audio, nullptr);
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

// A stage panel that always fits: scaled down when the window is smaller than
// the panel's natural size, left alone when it is not. No scrollbars, so a
// control is never half a drag off the edge of the page.
QWidget* AetherRxDialog::buildStagePage(QWidget* panel)
{
    return new FitToViewHost(panel);
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
