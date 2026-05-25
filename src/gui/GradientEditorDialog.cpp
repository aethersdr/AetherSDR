#include "GradientEditorDialog.h"
#include "Theme.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadialGradient>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kStripHeight    = 56;
constexpr int kMarkerStripH   = 22;
constexpr int kMarkerHalfW    = 6;
constexpr int kStripMargin    = 8;

QString colorToTokenHex(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb)
                            : c.name(QColor::HexArgb);
}

QIcon stopSwatchIcon(const QColor& c)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(0.5, 0.5, 15.0, 15.0), 3, 3);
    p.save();
    p.setClipPath(clip);
    p.fillRect(QRect(0, 0, 16, 16), QColor(0xc8, 0xc8, 0xc8));
    p.fillRect(QRect(0, 0, 8, 8),   QColor(0x80, 0x80, 0x80));
    p.fillRect(QRect(8, 8, 8, 8),   QColor(0x80, 0x80, 0x80));
    p.restore();

    p.setBrush(c);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, 15.0, 15.0), 3, 3);
    return QIcon(pm);
}

} // namespace

// ───────────────────────────────────── GradientStrip ─────────────────────

GradientStrip::GradientStrip(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(kStripHeight + kMarkerStripH + kStripMargin);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void GradientStrip::setGradient(const ThemeGradient& g)
{
    m_g = g;
    update();
}

void GradientStrip::setSelectedStop(int index)
{
    if (index == m_selectedIndex) return;
    m_selectedIndex = index;
    update();
}

QRect GradientStrip::stripRect() const
{
    return QRect(kStripMargin,
                 kStripMargin / 2,
                 width() - kStripMargin * 2,
                 kStripHeight);
}

QRect GradientStrip::markerRect(qreal at) const
{
    const int x = positionToX(at);
    const int yTop = stripRect().bottom() + 2;
    return QRect(x - kMarkerHalfW, yTop, kMarkerHalfW * 2, kMarkerStripH - 4);
}

int GradientStrip::positionToX(qreal at) const
{
    const QRect r = stripRect();
    return r.left() + static_cast<int>(std::round(std::clamp(at, 0.0, 1.0) * r.width()));
}

qreal GradientStrip::xToPosition(int x) const
{
    const QRect r = stripRect();
    if (r.width() <= 0) return 0.0;
    return std::clamp(static_cast<qreal>(x - r.left()) / r.width(), 0.0, 1.0);
}

int GradientStrip::hitTestStop(const QPoint& p) const
{
    // Walk stops in declared order; first contained-by marker wins.
    // Slightly inflate the hit-rect vertically to make small markers
    // easier to grab.
    for (int i = 0; i < m_g.stops.size(); ++i) {
        QRect mr = markerRect(m_g.stops[i].at).adjusted(-1, -2, 1, 2);
        if (mr.contains(p)) return i;
    }
    return -1;
}

void GradientStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r = stripRect();

    // Checkerboard under the strip to make translucent stops obvious.
    p.save();
    QPainterPath clipPath;
    clipPath.addRoundedRect(r, 4, 4);
    p.setClipPath(clipPath);
    constexpr int tile = 8;
    for (int y = r.top(); y < r.bottom(); y += tile) {
        for (int x = r.left(); x < r.right(); x += tile) {
            const bool dark = ((x / tile + y / tile) & 1) == 0;
            p.fillRect(QRect(x, y, tile, tile),
                       dark ? QColor(0x80, 0x80, 0x80) : QColor(0xc8, 0xc8, 0xc8));
        }
    }
    p.restore();

    // The gradient itself.
    if (m_g.type == ThemeGradient::Linear) {
        // Horizontal preview regardless of stored angle — the strip is a
        // 1D representation of stop positions, not a faithful mapping of
        // every angle to screen coords (radial angles aren't meaningful
        // here either).  The stored angle is what gets applied in the
        // actual rendering, the strip just sequences the stops 0→1 L→R.
        QLinearGradient lg(r.topLeft(), r.topRight());
        for (const auto& s : m_g.stops) lg.setColorAt(s.at, s.color);
        p.fillRect(r, QBrush(lg));
    } else {
        QRadialGradient rg(r.center(), r.height() / 2.0);
        for (const auto& s : m_g.stops) rg.setColorAt(s.at, s.color);
        p.fillRect(r, QBrush(rg));
    }

    // Border around the preview.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.drawRoundedRect(r, 4, 4);

    // Stop markers — small downward-pointing triangles below the strip,
    // colour-matched to each stop.  Selected stop gets a cyan halo.
    for (int i = 0; i < m_g.stops.size(); ++i) {
        const QRect mr = markerRect(m_g.stops[i].at);
        const int cx = mr.center().x();
        const int top = mr.top() + 2;
        const int bot = mr.bottom() - 2;
        QPolygon tri;
        tri << QPoint(cx, top)
            << QPoint(cx - kMarkerHalfW, bot)
            << QPoint(cx + kMarkerHalfW, bot);

        if (i == m_selectedIndex) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0x9b, 0xc4), 2));
            QRect halo = mr.adjusted(-2, -2, 2, 2);
            p.drawRoundedRect(halo, 4, 4);
        }

        p.setBrush(m_g.stops[i].color);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawPolygon(tri);
    }
}

void GradientStrip::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int idx = hitTestStop(event->pos());
    if (idx >= 0) {
        m_draggingIndex = idx;
        emit stopActivated(idx);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GradientStrip::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingIndex < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const qreal newAt = xToPosition(event->pos().x());
    emit stopMoved(m_draggingIndex, newAt);
    event->accept();
}

void GradientStrip::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_draggingIndex >= 0) {
        m_draggingIndex = -1;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void GradientStrip::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    // Double-click on a marker → fall through to single-click behaviour
    // (already opened the picker on the press); double-click on the
    // strip body → request a new stop.
    if (hitTestStop(event->pos()) >= 0) {
        event->accept();
        return;
    }
    if (stripRect().contains(event->pos())) {
        emit requestNewStop(xToPosition(event->pos().x()));
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void GradientStrip::contextMenuEvent(QContextMenuEvent* event)
{
    const int idx = hitTestStop(event->pos());
    if (idx >= 0) {
        emit requestDeleteStop(idx);
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

// ───────────────────────────────────── GradientEditorDialog ───────────────

GradientEditorDialog::GradientEditorDialog(const QString& tokenName,
                                           const ThemeGradient& initial,
                                           QWidget* parent)
    : PersistentDialog(QStringLiteral("Edit gradient — %1").arg(tokenName),
                       QStringLiteral("GradientEditorDialogGeometry"),
                       parent),
      m_tokenName(tokenName), m_gradient(initial)
{
    setModal(true);
    setMinimumSize(520, 420);
    applyAppTheme(this);
    // Inherit the global FramelessWindow setting so the chrome matches
    // whatever the main window + Theme Editor are showing.  PersistentDialog
    // owns the title-bar widget, so all we do is flip the mode here.
    setFramelessMode(AppSettings::instance()
                         .value("FramelessWindow", "True").toString() == "True");

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto* hdr = new QLabel(QStringLiteral(
        "<b>%1</b><br>Click a stop to recolour it, drag along the strip "
        "to reposition, right-click to delete, or double-click an empty "
        "spot to add a new stop.").arg(tokenName.toHtmlEscaped()), bodyWidget());
    hdr->setWordWrap(true);
    root->addWidget(hdr);

    m_strip = new GradientStrip(bodyWidget());
    root->addWidget(m_strip);

    // Stop list + per-stop buttons row.
    auto* listRow = new QHBoxLayout;
    m_stopList = new QListWidget(bodyWidget());
    m_stopList->setIconSize(QSize(18, 18));
    m_stopList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stopList->setMinimumHeight(140);
    listRow->addWidget(m_stopList, 1);

    auto* stopBtnCol = new QVBoxLayout;
    auto* addBtn  = new QPushButton(QStringLiteral("+ Add stop"), bodyWidget());
    auto* delBtn  = new QPushButton(QStringLiteral("− Delete stop"), bodyWidget());
    auto* editBtn = new QPushButton(QStringLiteral("Edit colour…"), bodyWidget());
    stopBtnCol->addWidget(addBtn);
    stopBtnCol->addWidget(delBtn);
    stopBtnCol->addWidget(editBtn);
    stopBtnCol->addStretch(1);
    listRow->addLayout(stopBtnCol);
    root->addLayout(listRow);

    // Angle row — only meaningful for linear gradients.
    auto* angleRow = new QHBoxLayout;
    m_angleLabel = new QLabel(QStringLiteral("Angle:"), bodyWidget());
    angleRow->addWidget(m_angleLabel);
    m_angleSpin = new QSpinBox(bodyWidget());
    m_angleSpin->setRange(0, 360);
    m_angleSpin->setSuffix(QStringLiteral("°"));
    m_angleSpin->setSingleStep(15);
    m_angleSpin->setValue(static_cast<int>(std::round(m_gradient.angle)) % 361);
    angleRow->addWidget(m_angleSpin);
    angleRow->addStretch(1);
    root->addLayout(angleRow);
    const bool linear = m_gradient.type == ThemeGradient::Linear;
    m_angleLabel->setVisible(linear);
    m_angleSpin->setVisible(linear);

    auto* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        Qt::Horizontal, bodyWidget());
    // QDialogButtonBox::RestoreDefaults sits at the left of the row by
    // default and reads as "wipe my edits and put the canonical value
    // back" — exactly what we want.
    auto* resetBtn = btnBox->button(QDialogButtonBox::RestoreDefaults);
    if (resetBtn) {
        resetBtn->setText(QStringLiteral("Reset to default"));
        resetBtn->setToolTip(QStringLiteral(
            "Restore the canonical stops shipped in the bundled Default "
            "Dark theme.  Discards every edit made in this dialog."));
        // Disable when no factory value exists (custom token added by a
        // third-party theme, etc.) so the button can't mislead.
        const ThemeGradient factory =
            ThemeManager::instance().factoryGradient(m_tokenName);
        resetBtn->setEnabled(!factory.stops.isEmpty());
    }
    root->addWidget(btnBox);

    // Initial UI population.
    syncStripAndList();

    connect(m_strip, &GradientStrip::stopMoved,
            this, &GradientEditorDialog::onStopMoved);
    connect(m_strip, &GradientStrip::stopActivated,
            this, &GradientEditorDialog::onStopActivated);
    connect(m_strip, &GradientStrip::requestNewStop,
            this, &GradientEditorDialog::onRequestNewStop);
    connect(m_strip, &GradientStrip::requestDeleteStop,
            this, &GradientEditorDialog::onRequestDeleteStop);
    connect(m_stopList, &QListWidget::itemSelectionChanged,
            this, &GradientEditorDialog::onStopListSelectionChanged);
    connect(m_stopList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onEditColorBtnClicked(); });
    connect(m_angleSpin,  qOverload<int>(&QSpinBox::valueChanged),
            this, &GradientEditorDialog::onAngleSpinChanged);
    connect(addBtn,  &QPushButton::clicked, this, &GradientEditorDialog::onAddStopBtnClicked);
    connect(delBtn,  &QPushButton::clicked, this, &GradientEditorDialog::onDeleteStopBtnClicked);
    connect(editBtn, &QPushButton::clicked, this, &GradientEditorDialog::onEditColorBtnClicked);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (resetBtn) {
        connect(resetBtn, &QPushButton::clicked,
                this, &GradientEditorDialog::onResetToDefaultClicked);
    }

    // Default selection — first stop, if any.
    if (!m_gradient.stops.isEmpty()) selectStop(0);
}

void GradientEditorDialog::rebuildStopList()
{
    QSignalBlocker block(m_stopList);
    m_stopList->clear();
    for (int i = 0; i < m_gradient.stops.size(); ++i) {
        const auto& s = m_gradient.stops[i];
        auto* item = new QListWidgetItem(stopSwatchIcon(s.color),
            QStringLiteral("%1  at %2  %3")
                .arg(i, 2, 10, QChar('0'))
                .arg(QString::number(s.at, 'f', 4), -8)
                .arg(colorToTokenHex(s.color)));
        item->setData(Qt::UserRole, i);
        m_stopList->addItem(item);
    }
}

void GradientEditorDialog::syncStripAndList()
{
    m_strip->setGradient(m_gradient);
    rebuildStopList();
}

void GradientEditorDialog::selectStop(int index)
{
    if (index < 0 || index >= m_gradient.stops.size()) {
        m_stopList->clearSelection();
        m_strip->setSelectedStop(-1);
        return;
    }
    QSignalBlocker block(m_stopList);
    m_stopList->setCurrentRow(index);
    m_strip->setSelectedStop(index);
}

int GradientEditorDialog::selectedStopIndex() const
{
    const auto sel = m_stopList->selectedItems();
    if (sel.isEmpty()) return -1;
    return sel.first()->data(Qt::UserRole).toInt();
}

void GradientEditorDialog::sortStopsByAt()
{
    // Sort stops by position so subsequent QLinearGradient rendering
    // and round-trip serialisation keep the natural left-to-right order.
    // Track the currently-selected stop's identity (by colour + at) so
    // we can re-select it after the sort.
    const int prevSel = selectedStopIndex();
    QColor prevColor;
    qreal  prevAt = -1.0;
    if (prevSel >= 0 && prevSel < m_gradient.stops.size()) {
        prevColor = m_gradient.stops[prevSel].color;
        prevAt    = m_gradient.stops[prevSel].at;
    }

    std::sort(m_gradient.stops.begin(), m_gradient.stops.end(),
              [](const ThemeGradientStop& a, const ThemeGradientStop& b) {
                  return a.at < b.at;
              });

    syncStripAndList();
    if (prevAt >= 0.0) {
        for (int i = 0; i < m_gradient.stops.size(); ++i) {
            if (qFuzzyCompare(m_gradient.stops[i].at, prevAt) &&
                m_gradient.stops[i].color == prevColor) {
                selectStop(i);
                return;
            }
        }
    }
}

void GradientEditorDialog::onStopMoved(int index, qreal newAt)
{
    if (index < 0 || index >= m_gradient.stops.size()) return;
    m_gradient.stops[index].at = newAt;
    // Don't sort mid-drag — repaint with the new position so the marker
    // can travel across neighbouring stops smoothly; sort happens on
    // mouse release (via syncStripAndList in the next user action).
    m_strip->setGradient(m_gradient);
    // Update the list row's text so the numeric position stays current.
    if (auto* item = m_stopList->item(index)) {
        const auto& s = m_gradient.stops[index];
        item->setText(QStringLiteral("%1  at %2  %3")
                          .arg(index, 2, 10, QChar('0'))
                          .arg(QString::number(s.at, 'f', 4), -8)
                          .arg(colorToTokenHex(s.color)));
    }
}

void GradientEditorDialog::onStopActivated(int index)
{
    selectStop(index);
}

void GradientEditorDialog::onRequestNewStop(qreal at)
{
    ThemeGradientStop s;
    s.at = at;
    // Sample the gradient at the click position so the new stop matches
    // the visual colour the user pointed at — feels like splitting the
    // ramp at that point rather than parachuting a random colour in.
    if (m_gradient.stops.isEmpty()) {
        s.color = QColor(0xff, 0xff, 0xff);
    } else if (m_gradient.stops.size() == 1) {
        s.color = m_gradient.stops.first().color;
    } else {
        // Find bracketing stops + lerp.
        ThemeGradient sorted = m_gradient;
        std::sort(sorted.stops.begin(), sorted.stops.end(),
                  [](const auto& a, const auto& b) { return a.at < b.at; });
        const auto& first = sorted.stops.first();
        const auto& last  = sorted.stops.last();
        if (at <= first.at)      s.color = first.color;
        else if (at >= last.at)  s.color = last.color;
        else {
            for (int i = 1; i < sorted.stops.size(); ++i) {
                const auto& a = sorted.stops[i - 1];
                const auto& b = sorted.stops[i];
                if (at >= a.at && at <= b.at) {
                    const qreal span = std::max(1e-9, b.at - a.at);
                    const qreal t = (at - a.at) / span;
                    auto blend = [t](int x, int y) {
                        return static_cast<int>(std::round((1.0 - t) * x + t * y));
                    };
                    s.color = QColor(blend(a.color.red(),   b.color.red()),
                                     blend(a.color.green(), b.color.green()),
                                     blend(a.color.blue(),  b.color.blue()),
                                     blend(a.color.alpha(), b.color.alpha()));
                    break;
                }
            }
        }
    }
    m_gradient.stops.append(s);
    sortStopsByAt();
    // Select the newly-added stop by colour+at identity.
    for (int i = 0; i < m_gradient.stops.size(); ++i) {
        if (qFuzzyCompare(m_gradient.stops[i].at, s.at) &&
            m_gradient.stops[i].color == s.color) {
            selectStop(i);
            return;
        }
    }
}

void GradientEditorDialog::onRequestDeleteStop(int index)
{
    if (index < 0 || index >= m_gradient.stops.size()) return;
    if (m_gradient.stops.size() <= 2) {
        QMessageBox::information(this, QStringLiteral("Cannot delete stop"),
            QStringLiteral("A gradient needs at least two stops.  Delete this "
                           "one would leave the gradient under-defined."));
        return;
    }
    m_gradient.stops.removeAt(index);
    syncStripAndList();
    selectStop(std::min(index, static_cast<int>(m_gradient.stops.size()) - 1));
}

void GradientEditorDialog::onStopListSelectionChanged()
{
    m_strip->setSelectedStop(selectedStopIndex());
}

void GradientEditorDialog::onAngleSpinChanged(int deg)
{
    m_gradient.angle = static_cast<qreal>(deg);
    // Angle changes don't affect the strip preview (it's always left-to-
    // right for editing legibility), but rebuilding the list isn't needed
    // either.  The angle is just stored and applied at brush() time.
}

void GradientEditorDialog::onAddStopBtnClicked()
{
    // Insert at midpoint between the last selected stop and its
    // neighbour, or at 0.5 when no selection.
    qreal at = 0.5;
    const int sel = selectedStopIndex();
    if (sel >= 0 && sel < m_gradient.stops.size() - 1) {
        at = (m_gradient.stops[sel].at + m_gradient.stops[sel + 1].at) / 2.0;
    } else if (sel == m_gradient.stops.size() - 1 && sel > 0) {
        at = (m_gradient.stops[sel - 1].at + m_gradient.stops[sel].at) / 2.0;
    }
    onRequestNewStop(at);
}

void GradientEditorDialog::onDeleteStopBtnClicked()
{
    onRequestDeleteStop(selectedStopIndex());
}

void GradientEditorDialog::onResetToDefaultClicked()
{
    const ThemeGradient factory =
        ThemeManager::instance().factoryGradient(m_tokenName);
    if (factory.stops.isEmpty()) return;  // shouldn't happen (button disabled)

    const auto reply = QMessageBox::question(this,
        QStringLiteral("Reset to default"),
        QStringLiteral("Replace the current stops with the canonical "
                       "value from the bundled Default Dark theme?\n\n"
                       "Any edits made in this dialog will be discarded."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_gradient = factory;
    if (m_angleSpin) {
        QSignalBlocker block(m_angleSpin);
        m_angleSpin->setValue(static_cast<int>(std::round(m_gradient.angle)) % 361);
    }
    syncStripAndList();
    if (!m_gradient.stops.isEmpty()) selectStop(0);
}

void GradientEditorDialog::onEditColorBtnClicked()
{
    const int idx = selectedStopIndex();
    if (idx < 0 || idx >= m_gradient.stops.size()) return;
    const QColor current = m_gradient.stops[idx].color;
    const QColor chosen = QColorDialog::getColor(current, this,
        QStringLiteral("Edit stop %1 colour").arg(idx),
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    m_gradient.stops[idx].color = chosen;
    syncStripAndList();
    selectStop(idx);
}

} // namespace AetherSDR
