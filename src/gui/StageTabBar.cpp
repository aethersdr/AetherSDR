#include "StageTabBar.h"
#include "ModemChrome.h"
#include "RxStageReorder.h"
#include "core/ThemeManager.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <utility>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Carried by a row drag: the stage id, as an int, under a MIME type private
// to the window that started the drag.
//
// The type MUST be per-window. Both bars accept drops, both windows can be
// open at once, and the payload is a bare id whose meaning depends entirely
// on which chain it came from: RxChainStage{Eq=1,Gate=2,Comp=3,Tube=4,Pudu=5}
// against TxChainStage{Gate=1,Eq=2,DeEss=3,Comp=4,Tube=5}. Every RX id is
// also a valid TX id, so one shared type let a drag out of the AetherRX
// column silently reorder the transmit chain -- dragging RX "Eq" (1) onto the
// TX bar moved TX "Gate" (1). Before these two columns were one shared
// widget they used different types by accident of having been written
// separately; this keeps that separation on purpose.
QString stageMimeFor(const QString& prefix)
{
    return QStringLiteral("application/x-aethersdr-stage.") + prefix;
}

// The grab handle at the left of a chain-stage row. Two columns of dots, the
// conventional "this moves" mark, and the drag it starts carries a picture of
// the whole row so what follows the cursor is what was grabbed.
class StageGrip final : public QWidget {
public:
    explicit StageGrip(int stage, QString mime, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_stage(stage)
        , m_mime(std::move(mime))
    {
        setCursor(Qt::OpenHandCursor);
        setFixedWidth(kGripWidth);
        setToolTip(QObject::tr(
            "Drag to move this stage in the chain. The order of the bar is "
            "the order the audio passes through."));
    }

    static constexpr int kGripWidth = 12;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(ModemChrome::colour(ModemChrome::Colour::TextDim));
        constexpr int kRows = 4;
        constexpr qreal kStep = 4.0;
        constexpr qreal kDot = 1.1;
        const qreal spanY = (kRows - 1) * kStep;
        const qreal x0 = width() / 2.0 - kStep / 2.0;
        const qreal y0 = height() / 2.0 - spanY / 2.0;
        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < 2; ++c) {
                p.drawEllipse(QPointF(x0 + c * kStep, y0 + r * kStep), kDot, kDot);
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
        mime->setData(m_mime, QByteArray::number(m_stage));

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
    int     m_stage;
    QString m_mime;
    QPoint  m_press;
};

// One tab in the column. Same chrome as the method strip inside the AetherNR
// page — checkable, property-selected, sized by the layout — but left-aligned,
// because a column of centred labels of different lengths reads as ragged
// where a row of them reads as even.
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

} // namespace

StageTabBar::StageTabBar(const QString& objectPrefix, QWidget* parent)
    : QFrame(parent)
    , m_prefix(objectPrefix)
    , m_mime(stageMimeFor(objectPrefix))
{
    setObjectName(objectPrefix + QStringLiteral("TabsFrame"));
    setAttribute(Qt::WA_StyledBackground, true);
    // The chrome sheet lives on the column rather than on the window. A Qt
    // stylesheet cascades to every descendant, and its 14 px base font reaches
    // inside the stage panels, which were drawn against the application font:
    // three points more is enough to push the minus sign out of a knob's 76 px
    // value editor.
    ThemeManager::instance().applyStyleSheet(
        this, ModemChrome::styleSheet(ModemChrome::Scale::Dialog));
    setFixedWidth(180);
    setAcceptDrops(true);
    installEventFilter(this);

    m_rows = new QVBoxLayout(this);
    m_rows->setContentsMargins(6, 6, 6, 6);
    m_rows->setSpacing(2);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    connect(m_group, &QButtonGroup::idClicked, this, &StageTabBar::stageSelected);
}

void StageTabBar::setHost(Host host)
{
    m_host = std::move(host);
}

void StageTabBar::addStage(int id, const QString& label, bool wantsCheckbox)
{
    // Object names carry no spaces: the automation bridge splits its command
    // line on whitespace, so "aetherTxTabFinal Output" is a target it can
    // never name. Accessible names keep the label as written.
    const QString slug = QString(label).remove(QLatin1Char(' '));

    auto* tab = makeStageTab(label);
    tab->setObjectName(m_prefix + QStringLiteral("Tab") + slug);
    tab->setAccessibleName(label + QStringLiteral(" stage"));
    m_group->addButton(tab, id);

    // One row: the grip, the tab that selects the page, then the stage's
    // on/off box at the right-hand end. Three separate widgets on purpose —
    // grabbing the grip must not change pages, clicking the tab must not
    // switch the stage off, and neither must start a drag.
    auto* row = new QWidget;
    auto* rowBox = new QHBoxLayout(row);
    rowBox->setContentsMargins(0, 0, 0, 0);
    rowBox->setSpacing(4);

    Row entry;
    entry.id = id;
    entry.row = row;

    const bool chainStage = m_host.isChainStage && m_host.isChainStage(id);
    if (chainStage) {
        auto* grip = new StageGrip(id, m_mime);
        grip->setObjectName(m_prefix + QStringLiteral("Grip") + slug);
        grip->setAccessibleName(label + QStringLiteral(" chain position"));
        rowBox->addWidget(grip);
    } else {
        // Not in the chain, so nothing to drag — but the label still starts
        // where every other label starts.
        entry.indent = new QWidget;
        entry.indent->setFixedWidth(StageGrip::kGripWidth);
        rowBox->addWidget(entry.indent);
    }

    rowBox->addWidget(tab, 1);

    if (wantsCheckbox && m_host.setStageEnabled) {
        auto* box = new QCheckBox;
        box->setObjectName(m_prefix + QStringLiteral("Enable") + slug);
        box->setAccessibleName(label + QStringLiteral(" stage enabled"));
        box->setToolTip(tr("Enable the %1 stage. Unchecked, the chain passes "
                           "straight through it.").arg(label));
        connect(box, &QCheckBox::toggled, this, [this, id](bool on) {
            if (m_syncing || !m_host.setStageEnabled) return;
            m_host.setStageEnabled(id, on);
        });
        entry.check = box;
        rowBox->addWidget(box);
    } else {
        // Nothing to bypass here, so no box — but the row still ends where
        // the others end rather than running past them.
        auto* pad = new QWidget;
        pad->setFixedWidth(18);
        rowBox->addWidget(pad);
    }

    m_rows->addWidget(row);
    m_stages.append(entry);
}

void StageTabBar::beginFooter()
{
    // The stretch that pushes the footer to the bottom goes in once, however
    // many things end up down there.
    if (m_footerStarted) return;
    m_rows->addStretch(1);
    m_footerStarted = true;
}

void StageTabBar::addFooterWidget(QWidget* w)
{
    beginFooter();
    // Indented to the column's one left edge, like every other row.
    auto* row = new QWidget;
    auto* box = new QHBoxLayout(row);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    auto* pad = new QWidget;
    pad->setFixedWidth(StageGrip::kGripWidth);
    box->addWidget(pad);
    box->addWidget(w, 1);
    m_rows->addWidget(row);
}

void StageTabBar::addFooterButton(const QString& label, const QString& objectName,
                                  const QString& tooltip)
{
    beginFooter();

    auto* button = makeStageTab(label);
    button->setCheckable(false);
    button->setObjectName(objectName);
    button->setAccessibleName(label);
    button->setToolTip(tooltip);
    connect(button, &QPushButton::clicked, this, &StageTabBar::footerButtonClicked);

    // Indented like every other label, so the column has one left edge.
    m_footerRow = new QWidget;
    auto* rowBox = new QHBoxLayout(m_footerRow);
    rowBox->setContentsMargins(0, 0, 0, 0);
    rowBox->setSpacing(4);
    auto* pad = new QWidget;
    pad->setFixedWidth(StageGrip::kGripWidth);
    rowBox->addWidget(pad);
    rowBox->addWidget(button, 1);
    m_rows->addWidget(m_footerRow);
}

void StageTabBar::setCurrentStage(int id)
{
    if (auto* b = m_group->button(id)) b->setChecked(true);
}

int StageTabBar::currentStage() const
{
    return m_group->checkedId();
}

void StageTabBar::refreshFromHost()
{
    if (m_host.stageEnabled) {
        m_syncing = true;
        for (const Row& r : m_stages) {
            if (!r.check) continue;
            const bool on = m_host.stageEnabled(r.id);
            if (r.check->isChecked() != on) r.check->setChecked(on);
        }
        m_syncing = false;
    }
    relayoutRows();
}

bool StageTabBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != this) return QFrame::eventFilter(watched, event);
    switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* ev = static_cast<QDragMoveEvent*>(event);
            if (ev->mimeData()->hasFormat(m_mime)) {
                ev->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop: {
            auto* ev = static_cast<QDropEvent*>(event);
            if (!ev->mimeData()->hasFormat(m_mime)) break;
            dropStageAt(ev->mimeData()->data(m_mime).toInt(),
                        ev->position().toPoint().y());
            ev->acceptProposedAction();
            return true;
        }
        default:
            break;
    }
    return QFrame::eventFilter(watched, event);
}

void StageTabBar::dropStageAt(int movedId, int y)
{
    if (!m_host.chainOrder || !m_host.commitChainOrder) return;
    if (m_host.isChainStage && !m_host.isChainStage(movedId)) return;

    const QVector<int> order = m_host.chainOrder();

    // The middles of the rows those ids occupy, in the same order, so the
    // rule can be arithmetic — see RxStageReorder.
    QVector<int> midpoints;
    midpoints.reserve(order.size());
    for (int id : order) {
        const QWidget* row = nullptr;
        for (const Row& r : m_stages) {
            if (r.id == id) { row = r.row; break; }
        }
        if (!row) return;             // a chain id with no row: do nothing
        midpoints.append(row->y() + row->height() / 2);
    }

    const QVector<int> next = RxStageReorder::dropped(order, movedId, midpoints, y);
    if (next == order) return;
    m_host.commitChainOrder(next);
    relayoutRows();
}

void StageTabBar::relayoutRows()
{
    if (!m_host.chainOrder) return;

    // Non-chain rows hold their declared position; the chain rows fill the
    // gaps between them in the host's order.
    const QVector<int> chain = m_host.chainOrder();
    QVector<int> wanted;
    wanted.reserve(m_stages.size());
    int nextChain = 0;
    for (const Row& r : m_stages) {
        const bool isChain = m_host.isChainStage && m_host.isChainStage(r.id);
        if (!isChain) {
            wanted.append(r.id);
            continue;
        }
        if (nextChain < chain.size()) wanted.append(chain[nextChain++]);
    }
    // Anything the host did not list still needs a row, after the rest.
    for (const Row& r : m_stages) {
        if (!wanted.contains(r.id)) wanted.append(r.id);
    }

    if (wanted == m_laidOut) return;
    m_laidOut = wanted;

    for (int i = 0; i < wanted.size(); ++i) {
        for (const Row& r : m_stages) {
            if (r.id != wanted[i]) continue;
            m_rows->removeWidget(r.row);
            m_rows->insertWidget(i, r.row);
            break;
        }
    }
}

} // namespace AetherSDR
