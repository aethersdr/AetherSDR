#include "AetherRxDialog.h"
#include "AetherDspWidget.h"
#include "AetherRxSettingsDialog.h"
#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "EditorFramelessTitleBar.h"
#include "CompactMetrics.h"
#include "ModemChrome.h"
#include "RxStageReorder.h"
#include "StripCompPanel.h"
#include "StripEqPanel.h"
#include "StripGatePanel.h"
#include "StripPuduPanel.h"
#include "StripRxOutputPanel.h"
#include "StripTubePanel.h"
#include "StripWaveformPanel.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"
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
    b->setMinimumHeight(36);
    return b;
}

// Carried by a row drag: the Stage the grip belongs to, as an int.
constexpr const char* kStageMime = "application/x-aethersdr-rxstage";

// The grab handle at the left of a chain-stage row. Two columns of dots, the
// conventional "this moves" mark, and the drag it starts carries a picture of
// the whole row so what follows the cursor is what was grabbed.
class StageGrip final : public QWidget {
public:
    explicit StageGrip(int stage, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_stage(stage)
    {
        setCursor(Qt::OpenHandCursor);
        setFixedWidth(kGripWidth);
        setToolTip(QObject::tr(
            "Drag to move this stage in the receive chain. The order of the "
            "bar is the order the audio passes through."));
    }

    static constexpr int kGripWidth = 12;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x5a, 0x6a, 0x7a));
        constexpr int kRows = 4;
        constexpr qreal kStep = 4.0;
        constexpr qreal kDot = 1.1;
        const qreal spanY = (kRows - 1) * kStep;
        const qreal x0 = width() / 2.0 - kStep / 2.0;
        const qreal y0 = height() / 2.0 - spanY / 2.0;
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < 2; ++c) {
                p.drawEllipse(QPointF(x0 + c * kStep, y0 + r * kStep),
                              kDot, kDot);
            }
        }
    }

    void mousePressEvent(QMouseEvent* ev) override
    {
        if (ev->button() == Qt::LeftButton) m_press = ev->pos();
    }

    void mouseMoveEvent(QMouseEvent* ev) override
    {
        if (!(ev->buttons() & Qt::LeftButton)) return;
        if ((ev->pos() - m_press).manhattanLength()
            < QApplication::startDragDistance()) {
            return;
        }
        auto* mime = new QMimeData;
        mime->setData(QLatin1String(kStageMime), QByteArray::number(m_stage));

        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        if (QWidget* row = parentWidget()) {
            drag->setPixmap(row->grab());
            drag->setHotSpot(mapTo(row, ev->pos()));
        }
        setCursor(Qt::ClosedHandCursor);
        drag->exec(Qt::MoveAction);
        setCursor(Qt::OpenHandCursor);
    }

private:
    int    m_stage;
    QPoint m_press;
};

// Stage <-> RxChainStage. Only five of the seven tabs are chain stages; the
// rest have no place in the order and are pinned at the ends.
bool isChainStage(AetherRxDialog::Stage s)
{
    return s == AetherRxDialog::Gate || s == AetherRxDialog::Eq
        || s == AetherRxDialog::Comp || s == AetherRxDialog::Tube
        || s == AetherRxDialog::Voice;
}

AudioEngine::RxChainStage toChainStage(AetherRxDialog::Stage s)
{
    switch (s) {
        case AetherRxDialog::Eq:    return AudioEngine::RxChainStage::Eq;
        case AetherRxDialog::Gate:  return AudioEngine::RxChainStage::Gate;
        case AetherRxDialog::Comp:  return AudioEngine::RxChainStage::Comp;
        case AetherRxDialog::Tube:  return AudioEngine::RxChainStage::Tube;
        case AetherRxDialog::Voice: return AudioEngine::RxChainStage::Pudu;
        default:                    return AudioEngine::RxChainStage::None;
    }
}

AetherRxDialog::Stage fromChainStage(AudioEngine::RxChainStage s)
{
    switch (s) {
        case AudioEngine::RxChainStage::Eq:   return AetherRxDialog::Eq;
        case AudioEngine::RxChainStage::Gate: return AetherRxDialog::Gate;
        case AudioEngine::RxChainStage::Comp: return AetherRxDialog::Comp;
        case AudioEngine::RxChainStage::Tube: return AetherRxDialog::Tube;
        case AudioEngine::RxChainStage::Pudu: return AetherRxDialog::Voice;
        default:                              return AetherRxDialog::StageCount;
    }
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
            // Through the theme, not setStyleSheet(): the replacement is a
            // {{token}} placeholder and nothing else would resolve it.
            AetherSDR::ThemeManager::instance().applyStyleSheet(w, sheet);
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
    , m_audio(audio)
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
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        m_tabsFrame, ModemChrome::styleSheet(ModemChrome::Scale::Dialog));
    // Wide enough for the longest label with a grip on one side of it and a
    // checkbox on the other.
    m_tabsFrame->setFixedWidth(180);
    m_tabsFrame->setAcceptDrops(true);
    m_tabsFrame->installEventFilter(this);
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
    addStage(Gate, QStringLiteral("Gate"), buildStagePage(m_gate));

    m_eq = new StripEqPanel(audio, this);
    // Straight through: the panel asks, MainWindow decides what a width means
    // for the mode the slice is in.
    connect(m_eq, &StripEqPanel::rxFilterWidthRequested,
            this, &AetherRxDialog::rxFilterWidthRequested);
    connect(m_eq, &StripEqPanel::cutoffsDragRequested,
            this, &AetherRxDialog::cutoffsDragRequested);
    addStage(Eq, QStringLiteral("EQ"), buildStagePage(m_eq));

    m_comp = new StripCompPanel(audio, this);
    addStage(Comp, QStringLiteral("Compressor"), buildStagePage(m_comp));

    m_tube = new StripTubePanel(audio, this);
    addStage(Tube, QStringLiteral("Tube"), buildStagePage(m_tube));

    m_voice = new StripPuduPanel(audio, this);
    addStage(Voice, QStringLiteral("Exciter"), buildStagePage(m_voice));

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
        addStage(Output, QStringLiteral("Final Output"), page);
    }

    tabsBox->addStretch(1);

    // Settings sits under the stretch, at the foot of the column: it is not a
    // stage, so it is not in the run of tabs, and the gap says so.
    {
        auto* settings = new QPushButton(tr("Settings"));
        settings->setObjectName(QStringLiteral("aetherRxSettingsButton"));
        settings->setAccessibleName(tr("AetherRX settings"));
        settings->setToolTip(tr("Profiles: save, load, import and export the "
                                "receive chain."));
        settings->setFlat(true);
        settings->setProperty("chrome", "tab");
        settings->setStyleSheet(QStringLiteral("text-align: left;"));
        settings->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        settings->setMinimumHeight(36);
        connect(settings, &QPushButton::clicked,
                this, &AetherRxDialog::showSettings);

        // Indented like every other label, so the column has one left edge.
        auto* row = new QWidget;
        auto* rowBox = new QHBoxLayout(row);
        rowBox->setContentsMargins(0, 0, 0, 0);
        rowBox->setSpacing(4);
        auto* pad = new QWidget;
        pad->setFixedWidth(StageGrip::kGripWidth);
        rowBox->addWidget(pad);
        rowBox->addWidget(settings, 1);
        tabsBox->addWidget(row);
    }

    // Out has no box; give its label the same start as every other.
    if (m_outIndent && m_stageChecks[Gate]) {
        m_outIndent->setFixedWidth(m_stageChecks[Gate]->sizeHint().width());
    }

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

    // The Client* stages are plain classes with no change signal, and the RX
    // chain strip toggles the same flags from outside this window, so the
    // boxes are polled rather than driven. Five times a second is far below
    // what a human notices and nowhere near what six bool reads cost.
    refreshStageChecks();
    relayoutStageRows();
    m_checkTimer = new QTimer(this);
    m_checkTimer->setInterval(200);
    connect(m_checkTimer, &QTimer::timeout, this, [this]() {
        refreshStageChecks();
        // The chain strip can reorder from outside this window too.
        relayoutStageRows();
    });

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

    // One row: grip, the tab that selects the page, then the stage's on/off
    // box at the right-hand end. Three separate widgets on purpose — grabbing
    // the grip must not change pages, clicking the tab must not switch the
    // stage off, and neither must start a drag.
    auto* row = new QWidget;
    m_stageRows[stage] = row;
    auto* rowBox = new QHBoxLayout(row);
    rowBox->setContentsMargins(0, 0, 0, 0);
    rowBox->setSpacing(4);

    if (isChainStage(stage)) {
        auto* grip = new StageGrip(stage);
        grip->setObjectName(QStringLiteral("aetherRxGrip") + label);
        grip->setAccessibleName(label + QStringLiteral(" chain position"));
        rowBox->addWidget(grip);
    } else {
        // Not in the chain, so nothing to drag — but the label still starts
        // where every other label starts.
        auto* pad = new QWidget;
        pad->setFixedWidth(StageGrip::kGripWidth);
        rowBox->addWidget(pad);
    }

    rowBox->addWidget(tab, 1);

    if (stage == Output) {
        // Nothing to bypass here, so no box — but the row still ends where
        // the others end rather than running past them. Sized from a real
        // checkbox once they all exist.
        m_outIndent = new QWidget;
        rowBox->addWidget(m_outIndent);
    } else {
        auto* box = new QCheckBox;
        box->setObjectName(QStringLiteral("aetherRxEnable") + label);
        box->setAccessibleName(label + QStringLiteral(" receive stage enabled"));
        box->setToolTip(tr("Enable the %1 stage. Unchecked, the receive chain "
                           "passes straight through it.").arg(label));
        m_stageChecks[stage] = box;
        connect(box, &QCheckBox::toggled, this, [this, stage](bool on) {
            setStageEnabled(stage, on);
        });
        rowBox->addWidget(box);
    }

    qobject_cast<QVBoxLayout*>(m_tabsFrame->layout())->addWidget(row);

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
    refreshStageChecks();
    relayoutStageRows();
    if (m_checkTimer) m_checkTimer->start();
}

void AetherRxDialog::hideEvent(QHideEvent* event)
{
    PersistentDialog::hideEvent(event);
    if (m_checkTimer) m_checkTimer->stop();
}

bool AetherRxDialog::stageEnabled(Stage stage) const
{
    if (!m_audio) return false;
    switch (stage) {
        case Nr:
            // Client-side NR is exclusive: the stage is on when any one of
            // the seven methods is.
            return m_audio->nr2Enabled()  || m_audio->nr4Enabled()
                || m_audio->mnrEnabled()  || m_audio->dfnrEnabled()
                || m_audio->rn2Enabled()  || m_audio->nvAfxEnabled()
                || m_audio->nnrEnabled();
        case Gate:
            return m_audio->clientGateRx() && m_audio->clientGateRx()->isEnabled();
        case Eq:
            return m_audio->clientEqRx() && m_audio->clientEqRx()->isEnabled();
        case Comp:
            return m_audio->clientCompRx() && m_audio->clientCompRx()->isEnabled();
        case Tube:
            return m_audio->clientTubeRx() && m_audio->clientTubeRx()->isEnabled();
        case Voice:
            return m_audio->clientPuduRx() && m_audio->clientPuduRx()->isEnabled();
        default:
            return false;
    }
}

void AetherRxDialog::setStageEnabled(Stage stage, bool on)
{
    if (!m_audio || m_syncingChecks) return;
    switch (stage) {
        case Nr: {
            // The same gesture the chain strip's DSP tile makes: switching the
            // stage off turns off whichever method is running, and switching it
            // back on restores the last one AetherDspWidget saved.
            if (!on) {
                QMetaObject::invokeMethod(m_audio, [audio = m_audio]() {
                    if (audio->nr2Enabled())    audio->setNr2Enabled(false);
                    if (audio->nr4Enabled())    audio->setNr4Enabled(false);
                    if (audio->mnrEnabled())    audio->setMnrEnabled(false);
                    if (audio->dfnrEnabled())   audio->setDfnrEnabled(false);
                    if (audio->rn2Enabled())    audio->setRn2Enabled(false);
                    if (audio->nvAfxEnabled())  audio->setNvAfxEnabled(false);
                    if (audio->nnrEnabled())    audio->setNnrEnabled(false);
                });
                break;
            }
            const QString name =
                AppSettings::instance().value("LastClientNr", "").toString();
            // Never been on, so there is nothing to restore; the next poll
            // puts the box back where it was.
            if (name.isEmpty()) break;
            if (name == QLatin1String("NR2")) {
                // NR2 needs the wisdom prep before the engine builds
                // SpectralNR — straight through the setter can crash the
                // next feedAudioData (#2275).
                emit nr2EnableWithWisdomRequested();
                break;
            }
            QMetaObject::invokeMethod(m_audio, [audio = m_audio, name]() {
                if (name == QLatin1String("NR4"))       audio->setNr4Enabled(true);
                else if (name == QLatin1String("MNR"))  audio->setMnrEnabled(true);
                else if (name == QLatin1String("DFNR")) audio->setDfnrEnabled(true);
                else if (name == QLatin1String("RN2"))  audio->setRn2Enabled(true);
                else if (name == QLatin1String("BNR"))  audio->setNvAfxEnabled(true);
                else if (name == QLatin1String("NNR"))  audio->setNnrEnabled(true);
            });
            break;
        }
        case Gate:
            if (auto* g = m_audio->clientGateRx()) {
                g->setEnabled(on);
                m_audio->saveClientGateRxSettings();
            }
            break;
        case Eq:
            if (auto* e = m_audio->clientEqRx()) {
                e->setEnabled(on);
                m_audio->saveClientEqSettings();
            }
            break;
        case Comp:
            if (auto* c = m_audio->clientCompRx()) {
                c->setEnabled(on);
                m_audio->saveClientCompRxSettings();
            }
            break;
        case Tube:
            if (auto* t = m_audio->clientTubeRx()) {
                t->setEnabled(on);
                m_audio->saveClientTubeRxSettings();
            }
            break;
        case Voice:
            if (auto* p = m_audio->clientPuduRx()) {
                p->setEnabled(on);
                m_audio->saveClientPuduRxSettings();
            }
            break;
        default:
            break;
    }
}

bool AetherRxDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_tabsFrame) {
        return PersistentDialog::eventFilter(watched, event);
    }
    switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* ev = static_cast<QDragMoveEvent*>(event);
            if (ev->mimeData()->hasFormat(QLatin1String(kStageMime))) {
                ev->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop: {
            auto* ev = static_cast<QDropEvent*>(event);
            if (!ev->mimeData()->hasFormat(QLatin1String(kStageMime))) break;
            const int raw =
                ev->mimeData()->data(QLatin1String(kStageMime)).toInt();
            if (raw >= 0 && raw < StageCount) {
                dropStageAt(static_cast<Stage>(raw),
                            ev->position().toPoint().y());
            }
            ev->acceptProposedAction();
            return true;
        }
        default:
            break;
    }
    return PersistentDialog::eventFilter(watched, event);
}

void AetherRxDialog::dropStageAt(Stage moved, int y)
{
    if (!m_audio || !isChainStage(moved)) return;

    const QVector<AudioEngine::RxChainStage> stages = m_audio->rxChainStages();

    // Hand the rule the order and the row middles and let it do the
    // arithmetic — see RxStageReorder, which exists so this is testable.
    QVector<int> order;
    QVector<int> midpoints;
    for (auto s : stages) {
        const Stage rowStage = fromChainStage(s);
        if (rowStage >= StageCount) continue;
        QWidget* row = m_stageRows[rowStage];
        if (!row) continue;
        order.append(static_cast<int>(s));
        midpoints.append(row->y() + row->height() / 2);
    }

    const QVector<int> next = RxStageReorder::dropped(
        order, static_cast<int>(toChainStage(moved)), midpoints, y);
    if (next == order) return;

    QVector<AudioEngine::RxChainStage> reordered;
    reordered.reserve(next.size());
    for (int id : next) {
        reordered.append(static_cast<AudioEngine::RxChainStage>(id));
    }

    // Persists on its own — setRxChainStages writes ClientRxChainStages.
    m_audio->setRxChainStages(reordered);
    relayoutStageRows();
}

void AetherRxDialog::relayoutStageRows()
{
    if (!m_audio || !m_tabsFrame) return;

    // AetherNR first, the chain in engine order, Out last.
    QVector<int> wanted;
    wanted.append(Nr);
    for (auto s : m_audio->rxChainStages()) {
        const Stage rowStage = fromChainStage(s);
        if (rowStage < StageCount) wanted.append(rowStage);
    }
    // A stage the engine did not list still needs a row; keep it in the order
    // it was declared in, after the ones that were listed.
    for (int i = 0; i < StageCount; ++i) {
        if (i != Nr && i != Output && !wanted.contains(i)) wanted.append(i);
    }
    wanted.append(Output);

    if (wanted == m_rowOrder) return;
    m_rowOrder = wanted;

    auto* box = qobject_cast<QVBoxLayout*>(m_tabsFrame->layout());
    if (!box) return;
    for (int i = 0; i < wanted.size(); ++i) {
        QWidget* row = m_stageRows[wanted[i]];
        if (!row) continue;
        box->removeWidget(row);
        box->insertWidget(i, row);
    }
}

void AetherRxDialog::refreshStageChecks()
{
    if (!m_audio) return;
    m_syncingChecks = true;
    for (int i = 0; i < StageCount; ++i) {
        QCheckBox* box = m_stageChecks[i];
        if (!box) continue;
        const bool on = stageEnabled(static_cast<Stage>(i));
        if (box->isChecked() != on) box->setChecked(on);
    }
    m_syncingChecks = false;
}

void AetherRxDialog::showSettings()
{
    // Modeless would let the operator load a profile while a stage page is
    // mid-drag; modal keeps the chain still while it is being rewritten.
    AetherRxSettingsDialog dlg(m_audio, this);
    connect(&dlg, &AetherRxSettingsDialog::profileApplied, this, [this]() {
        // A profile can reorder the chain and flip every enable, so the whole
        // window re-reads the engine rather than waiting for the poll.
        syncFromEngine();
        refreshStageChecks();
        relayoutStageRows();
    });
    dlg.exec();
}

void AetherRxDialog::syncFromEngine()
{
    if (m_widget) m_widget->syncFromEngine();
}

void AetherRxDialog::selectTab(const QString& name)
{
    // Both spellings resolve. These stages were AGC-G, AGC-C, AetherVoice and
    // Out until the tabs were renamed, and callers elsewhere — plus anything
    // driving this window through the automation bridge — still say those.
    static const struct { Stage stage; const char* label; } kStages[] = {
        {Nr, "AetherNR"},   {Gate, "Gate"},        {Eq, "EQ"},
        {Comp, "Compressor"}, {Tube, "Tube"},      {Voice, "Exciter"},
        {Output, "Final Output"},
        {Gate, "AGC-G"},    {Comp, "AGC-C"},      {Voice, "AetherVoice"},
        {Output, "Out"},
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
