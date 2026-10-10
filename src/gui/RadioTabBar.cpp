#include "RadioTabBar.h"

#include "core/ThemeManager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QWheelEvent>
#include <QStringList>
#include <QVariantList>
#include <QWindow>
#include <QtMath>
#include <algorithm>

namespace AetherSDR {

namespace {

// 52 px bar − 8 px above and below.  The inset is load-bearing, not taste:
// Qt's expanded Windows frame keeps a top resize border of about 8 px, so a
// taller tab would sit where a press starts a window resize instead of
// reaching the tab.  (On Linux the resizer reserves the whole bar height —
// no top-edge resize there, #4886.)
constexpr int  kTabHeight     = 36;
constexpr int  kTabRadius     = 8;
constexpr int  kTabPadX       = 14;
constexpr int  kTabPadY       = 4;
constexpr int  kDotDiameter   = 7;
constexpr int  kDotTextGap    = 8;
constexpr int  kAddButtonSize = 28;
constexpr int  kStripSpacing  = 6;
constexpr int  kStripMinWidth = 112;
constexpr qreal kNameSizePx   = 12.5;
constexpr qreal kStatusSizePx = 9.5;
// A heartbeat swells the dot's glow and lets it fall away over this long.  The
// glow is data, not decoration: one swell per discovery packet, so a stalled
// link simply stops breathing.  Long enough to read as a pulse rather than the
// 100 ms blink the old standalone lamp used.
//
// Kept BELOW the discovery beat interval (~1 s) on purpose.  At 1400 ms the
// decay never finished before the next beat re-armed it, so the ticker ran at
// 12.5 Hz for the entire connected session — a permanent repaint of the active
// tab, in a bar whose whole design point is a small idle cost.
constexpr int  kPulseDecayMs  = 850;
constexpr int  kPulseTickMs   = 80;     // 12.5 Hz — smooth enough, cheap enough
constexpr int  kAlarmBlinkMs  = 500;    // link-lost red blink, as before

// Separator between the parts of a tab's status line.  Spelled as a universal
// character name rather than as raw UTF-8 bytes: QStringLiteral builds a UTF-16
// literal, so "\xC2\xB7" lands as two code units (Â·) instead of one MIDDLE DOT.
QString middleDot()
{
    return QStringLiteral(" \u00B7 ");
}

// Qt only exposes an integral setPixelSize(), and the title-bar spec calls for
// 12.5 px and 9.5 px.  Point size *is* fractional, and pixels = points × dpi/72,
// so route the fractional size through there rather than rounding the design.
void setFractionalPixelSize(QFont& font, qreal px, const QWidget* onWidget)
{
    const qreal dpi = onWidget && onWidget->logicalDpiY() > 0
        ? qreal(onWidget->logicalDpiY())
        : 96.0;
    font.setPointSizeF(px * 72.0 / dpi);
}

QFont uiFont(const QWidget* w, qreal px, int weight)
{
    QFont f = ThemeManager::instance().font(w, QStringLiteral("font.family.ui"));
    setFractionalPixelSize(f, px, w);
    f.setWeight(QFont::Weight(weight));
    return f;
}

QString statusToken(RadioTabStatus status)
{
    switch (status) {
        case RadioTabStatus::Connected:
            return QStringLiteral("color.titlebar.status.connected");
        case RadioTabStatus::InUse:
            return QStringLiteral("color.titlebar.status.inUse");
        case RadioTabStatus::Available:
        default:
            return QStringLiteral("color.titlebar.status.available");
    }
}

} // namespace

QString radioTabStatusText(RadioTabStatus status)
{
    switch (status) {
        case RadioTabStatus::Connected: return QStringLiteral("connected");
        case RadioTabStatus::InUse:     return QStringLiteral("in use");
        case RadioTabStatus::Available:
        default:                        return QStringLiteral("available");
    }
}

QString radioTabStatusLabel(RadioTabStatus status)
{
    // Sentence case, first letter only: the tab, its tooltip and its accessible
    // name are prose an operator reads, not the bridge token above.  The two
    // deliberately differ — capitalizing radioTabStatusText() instead would
    // break every driver matching on `status`.
    switch (status) {
        case RadioTabStatus::Connected:
            return QCoreApplication::translate("RadioTab", "Connected");
        case RadioTabStatus::InUse:
            return QCoreApplication::translate("RadioTab", "In use");
        case RadioTabStatus::Available:
        default:
            return QCoreApplication::translate("RadioTab", "Available");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RadioTab
// ─────────────────────────────────────────────────────────────────────────────

RadioTab::RadioTab(const RadioTabEntry& entry, QWidget* parent)
    : QAbstractButton(parent), m_entry(entry)
{
    setObjectName(QStringLiteral("radioTab_") + entry.id);
    setAccessibleIdentifier(objectName());
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);   // keyboard-reachable per the a11y contract
    setFixedHeight(kTabHeight);
    setAttribute(Qt::WA_Hover, true);
    refreshAccessibility();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, qOverload<>(&QWidget::update));
}

QString RadioTab::statusLine() const
{
    // The name is already line one; repeating it here pushed the state — the
    // part that changes — off the end of every narrow tab.  The model is shown
    // only when an operator nickname hides it.
    QStringList parts;
    if (!m_entry.model.isEmpty() && m_entry.model != m_entry.name) {
        parts << m_entry.model;
    }
    parts << statusWord();
    if (!m_entry.detail.isEmpty()) {
        parts << m_entry.detail;
    }
    return parts.join(middleDot());
}

QString RadioTab::statusWord() const
{
    // The alarm is the one state the dot exists to show, so it must be in
    // words too (WCAG 1.4.1) — it speaks over the radio's own status.
    return m_alarm ? tr("Link lost") : radioTabStatusLabel(m_entry.status);
}

QString RadioTab::descriptionLine() const
{
    // Screen readers and the tooltip get the name too: they are not reading
    // the two lines together the way a sighted operator does.
    return m_entry.name.isEmpty() ? statusLine()
                                  : m_entry.name + middleDot() + statusLine();
}

QRect RadioTab::dotDirtyRect() const
{
    // Everything the link indicator paints — the dot and its heartbeat glow
    // (radius kDotDiameter * 1.9, see paintEvent) — plus a pixel for
    // antialiasing.  The heartbeat repaints only this, not the whole tab.
    const qreal cx = kTabPadX + kDotDiameter / 2.0;
    const qreal cy = height() / 2.0;
    const int r = int(qCeil(kDotDiameter * 1.9)) + 1;
    return QRect(int(cx) - r, int(cy) - r, 2 * r + 1, 2 * r + 1);
}

void RadioTab::refreshAccessibility()
{
    setText(m_entry.name);
    // The status is in the accessible name as well as on screen — a screen
    // reader user must not have to infer it from the dot's colour.
    setAccessibleName(QStringLiteral("Radio %1, %2").arg(m_entry.name, statusWord()));
    setAccessibleDescription(descriptionLine());
    setToolTip(descriptionLine());
}

void RadioTab::setEntry(const RadioTabEntry& entry)
{
    if (m_entry == entry) {
        return;
    }
    m_entry = entry;
    setObjectName(QStringLiteral("radioTab_") + entry.id);
    setAccessibleIdentifier(objectName());
    refreshAccessibility();
    updateGeometry();
    update();
}

void RadioTab::setPulse(qreal v)
{
    if (qFuzzyCompare(m_pulse, v)) {
        return;
    }
    m_pulse = v;
    update(dotDirtyRect());
}

void RadioTab::setLinkOverride(const QColor& overrideColor, bool alarm)
{
    if (m_overrideColor == overrideColor && m_alarm == alarm) {
        return;
    }
    const bool alarmChanged = m_alarm != alarm;
    m_overrideColor = overrideColor;
    m_alarm = alarm;
    if (!alarm) {
        m_alarmVisible = true;
    }
    if (alarmChanged) {
        // The words change with the alarm, so the whole tab (and possibly its
        // width) does; a colour-only change stays on the dot's footprint.
        refreshAccessibility();
        updateGeometry();
        update();
    } else {
        update(dotDirtyRect());
    }
}

void RadioTab::setAlarmVisible(bool on)
{
    if (m_alarmVisible == on) {
        return;
    }
    m_alarmVisible = on;
    if (m_alarm) {
        update(dotDirtyRect());
    }
}

void RadioTab::setBeatColor(const QColor& color)
{
    if (m_beatColor == color) {
        return;
    }
    m_beatColor = color;
    update(dotDirtyRect());
}

QColor RadioTab::dotColor() const
{
    // Link state speaks over the radio's own status: an operator who has lost
    // the link needs to see that before they need to see "connected".
    if (m_overrideColor.isValid()) {
        return m_overrideColor;
    }
    return ThemeManager::instance().color(this, statusToken(m_entry.status));
}

QSize RadioTab::sizeHint() const
{
    const QFontMetricsF nameFm(uiFont(this, kNameSizePx, 600));
    const QFontMetricsF statusFm(uiFont(this, kStatusSizePx, 400));
    const qreal textWidth = qMax(nameFm.horizontalAdvance(m_entry.name),
                                 statusFm.horizontalAdvance(statusLine()));
    const int w = kTabPadX + kDotDiameter + kDotTextGap
                + int(qCeil(textWidth)) + kTabPadX;
    const int h = kTabPadY + int(qCeil(nameFm.height() + statusFm.height()))
                + kTabPadY;
    return QSize(w, qMax(kTabHeight, h));
}

void RadioTab::focusInEvent(QFocusEvent* ev)
{
    m_focusVisible = ev->reason() == Qt::TabFocusReason
                  || ev->reason() == Qt::BacktabFocusReason
                  || ev->reason() == Qt::ShortcutFocusReason;
    update();
    QAbstractButton::focusInEvent(ev);
}

void RadioTab::focusOutEvent(QFocusEvent* ev)
{
    m_focusVisible = false;
    update();
    QAbstractButton::focusOutEvent(ev);
}

void RadioTab::enterEvent(QEnterEvent* ev)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(ev);
}

void RadioTab::leaveEvent(QEvent* ev)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(ev);
}

void RadioTab::paintEvent(QPaintEvent* ev)
{
    Q_UNUSED(ev);
    auto& theme = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(body, kTabRadius, kTabRadius);

    if (isChecked()) {
        p.fillPath(shape,
                   theme.color(this, QStringLiteral("color.titlebar.tab.active.background")));
        p.setPen(QPen(theme.color(this, QStringLiteral("color.titlebar.tab.active.border")), 1));
        p.drawPath(shape);
        p.setPen(QPen(theme.color(this, QStringLiteral("color.border.accent")), 2));
        p.drawLine(QPointF(kTabRadius + 4, height() - 2),
                   QPointF(width() - kTabRadius - 4, height() - 2));
    } else if (m_hovered || isDown()) {
        p.fillPath(shape, theme.color(this, QStringLiteral("color.titlebar.tab.hover")));
    }

    if (m_focusVisible) {
        // Non-text UI needs 3:1; the accent border is the theme's focus cue
        // everywhere else in the app, so reuse it rather than inventing one.
        // Gated on focus-*visible*, not bare focus: a tab that happened to
        // receive the window's initial focus should not open ringed.
        p.setPen(QPen(theme.color(this, QStringLiteral("color.border.accent")), 2));
        p.drawPath(shape);
    }

    // ── Status dot / radio-link indicator ───────────────────────────────────
    const QColor dot = dotColor();
    const qreal dotX = kTabPadX;
    const qreal dotY = (height() - kDotDiameter) / 2.0;
    const QRectF dotRect(dotX, dotY, kDotDiameter, kDotDiameter);

    if (m_pulse > 0.0) {
        // Heartbeat glow, drawn under the dot so the dot itself never loses
        // contrast at the trough.  The throttle tint (if any) colours the glow
        // rather than the dot, so the radio's own status stays readable while
        // the adaptive-throttle warning is showing.
        const QColor glowBase = m_beatColor.isValid() ? m_beatColor : dot;
        const qreal glowR = kDotDiameter * 1.9;
        QRadialGradient glow(dotRect.center(), glowR);
        QColor inner = glowBase;
        inner.setAlphaF(0.45 * m_pulse);
        QColor outer = glowBase;
        outer.setAlphaF(0.0);
        glow.setColorAt(0.0, inner);
        glow.setColorAt(1.0, outer);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(dotRect.center(), glowR, glowR);
    }

    p.setPen(Qt::NoPen);
    if (m_alarm && !m_alarmVisible) {
        // Blink trough: a hollow ring rather than nothing at all, so the dot
        // never disappears entirely and the tab's layout doesn't flicker.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dot, 1));
        p.drawEllipse(dotRect.adjusted(0.5, 0.5, -0.5, -0.5));
    } else {
        p.setBrush(dot);
        p.drawEllipse(dotRect);
    }

    // ── Two-line text block ─────────────────────────────────────────────────
    const qreal textX = dotX + kDotDiameter + kDotTextGap;
    const qreal textW = width() - textX - kTabPadX;

    const QFont nameFont = uiFont(this, kNameSizePx, 600);
    const QFont statusFont = uiFont(this, kStatusSizePx, 400);
    const QFontMetricsF nameFm(nameFont);
    const QFontMetricsF statusFm(statusFont);

    const qreal blockH = nameFm.height() + statusFm.height();
    qreal y = (height() - blockH) / 2.0;

    p.setFont(nameFont);
    p.setPen(theme.color(this, QStringLiteral("color.text.primary")));
    p.drawText(QRectF(textX, y, textW, nameFm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               nameFm.elidedText(m_entry.name, Qt::ElideRight, textW));
    y += nameFm.height();

    p.setFont(statusFont);
    p.setPen(theme.color(this, QStringLiteral("color.text.secondary")));
    p.drawText(QRectF(textX, y, textW, statusFm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               statusFm.elidedText(statusLine(), Qt::ElideRight, textW));
}

// ─────────────────────────────────────────────────────────────────────────────
// Discovered-radios popover
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Panel shown by the "+" button.  Qt::Popup so it closes on outside click and
// on Esc without any bookkeeping here, and grabs the keyboard so arrow keys
// walk the rows.
class DiscoveryPopover : public QWidget {
public:
    explicit DiscoveryPopover(QWidget* parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint
                              | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("discoveredRadiosPopover"));
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAccessibleName(tr("Discovered radios"));

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);

        m_panel = new QWidget(this);
        m_panel->setObjectName(QStringLiteral("discoveredRadiosPanel"));
        ThemeManager::instance().applyStyleSheet(
            m_panel,
            QStringLiteral(
                "#discoveredRadiosPanel {"
                " background: {{color.background.1}};"
                " border: 1px solid {{color.border.strong}};"
                " border-radius: 10px; }"));
        outer->addWidget(m_panel);

        m_rows = new QVBoxLayout(m_panel);
        m_rows->setContentsMargins(6, 6, 6, 6);
        m_rows->setSpacing(2);
    }

    QVBoxLayout* rows() const { return m_rows; }
    QWidget*     panel() const { return m_panel; }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            focusNextPrevChild(event->key() == Qt::Key_Down);
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QWidget*     m_panel{nullptr};
    QVBoxLayout* m_rows{nullptr};
};

QString rowStyleTemplate()
{
    return QStringLiteral(
        "QPushButton {"
        " text-align: left;"
        " padding: 6px 10px;"
        " border: none;"
        " border-radius: 6px;"
        " background: {{color.background.1}};"
        " color: {{color.text.primary}}; }"
        "QPushButton:hover  { background: {{color.background.2}}; }"
        "QPushButton:pressed { background: {{color.background.0}}; }"
        "QPushButton:focus  { border: 1px solid {{color.border.accent}}; }");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// RadioTabBar
// ─────────────────────────────────────────────────────────────────────────────

RadioTabBar::RadioTabBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("radioTabBar"));
    setAccessibleName(QStringLiteral("Radios"));
    // Preferred: the strip takes its full width while the bar has room and
    // gives it back, down to kStripMinWidth, only when it runs out; past that
    // it scrolls.  No ceiling, so spare bar width never clips a tab.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(kStripSpacing);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("radioTabScroller"));
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setFixedHeight(kTabHeight);
    m_scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_scrollArea->viewport()->setAutoFillBackground(false);

    m_tabHost = new QWidget;
    m_tabHost->setObjectName(QStringLiteral("radioTabHost"));
    m_tabHost->setFixedHeight(kTabHeight);
    m_tabHost->setAutoFillBackground(false);
    m_tabsLayout = new QHBoxLayout(m_tabHost);
    m_tabsLayout->setContentsMargins(0, 0, 0, 0);
    m_tabsLayout->setSpacing(kStripSpacing);
    m_scrollArea->setWidget(m_tabHost);
    m_scrollArea->viewport()->installEventFilter(this);
    m_tabHost->installEventFilter(this);   // drag between tabs
    m_layout->addWidget(m_scrollArea, 1);

    m_addButton = new QToolButton(this);
    m_addButton->setObjectName(QStringLiteral("radioTabAddButton"));
    m_addButton->setText(QStringLiteral("+"));
    m_addButton->setFixedSize(kAddButtonSize, kTabHeight);
    m_addButton->setCursor(Qt::PointingHandCursor);
    m_addButton->setFocusPolicy(Qt::StrongFocus);
    m_addButton->setAccessibleName(tr("Add radio"));
    m_addButton->setAccessibleDescription(
        tr("Open the list of discovered radios"));
    m_addButton->setToolTip(tr("Discovered radios"));
    ThemeManager::instance().applyStyleSheet(
        m_addButton,
        QStringLiteral(
            "QToolButton {"
            " background: transparent;"
            " border: none;"
            " border-radius: 8px;"
            " font-size: 17px;"
            " color: {{color.text.secondary}}; }"
            "QToolButton:hover { background: {{color.titlebar.tab.hover}};"
            " color: {{color.text.primary}}; }"
            "QToolButton[focusVisible=\"true\"] { border: 1px solid {{color.border.accent}}; }"));
    // Ring on keyboard focus only, like the tabs.  A plain :focus rule lit the
    // "+" on every launch, because it is the first focusable widget in the
    // window and takes its initial focus.
    m_addButton->installEventFilter(this);
    connect(m_addButton, &QToolButton::clicked, this, [this]() {
        emit discoveryPopoverRequested();
        showDiscoveryPopover();
    });
    m_layout->addWidget(m_addButton);

    // One shared ticker rather than an animation per tab: only the active tab
    // ever glows, and this app watches its idle-repaint budget closely.  The
    // timer runs only while a beat is decaying and stops when the glow reaches
    // zero — so a bar with no beats arriving costs nothing.  While a link IS
    // beating the decay is shorter than the beat interval, so the ticker gets
    // to stop between beats instead of being re-armed mid-decay and running
    // continuously for the whole session.
    m_pulseTimer = new QTimer(this);
    m_pulseTimer->setInterval(kPulseTickMs);
    connect(m_pulseTimer, &QTimer::timeout, this, [this]() {
        m_pulseLevel -= qreal(kPulseTickMs) / qreal(kPulseDecayMs);
        if (m_pulseLevel <= 0.0) {
            m_pulseLevel = 0.0;
            m_pulseTimer->stop();
        }
        // Squared falloff: bright at the beat, then a long soft tail.
        applyLinkVisuals();
    });

    m_alarmTimer = new QTimer(this);
    m_alarmTimer->setInterval(kAlarmBlinkMs);
    connect(m_alarmTimer, &QTimer::timeout, this, [this]() {
        m_alarmVisible = !m_alarmVisible;
        applyLinkVisuals();
    });
}

bool RadioTabBar::eventFilter(QObject* watched, QEvent* ev)
{
    if (watched == m_addButton
        && (ev->type() == QEvent::FocusIn || ev->type() == QEvent::FocusOut)) {
        bool visible = false;
        if (ev->type() == QEvent::FocusIn) {
            const Qt::FocusReason reason = static_cast<QFocusEvent*>(ev)->reason();
            visible = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                   || reason == Qt::ShortcutFocusReason;
        }
        if (m_addButton->property("focusVisible").toBool() != visible) {
            m_addButton->setProperty("focusVisible", visible);
            m_addButton->style()->unpolish(m_addButton);
            m_addButton->style()->polish(m_addButton);
        }
    }
    // The strip scrolls sideways with its scroll bars hidden, so a plain mouse
    // wheel — vertical only — was the one pointer that could not reach an
    // overflowing tab.  Map a mostly-vertical wheel onto the horizontal bar;
    // trackpad sideways swipes keep QScrollArea's native handling.
    if (ev->type() == QEvent::Wheel && m_scrollArea
        && watched == m_scrollArea->viewport()) {
        auto* wheel = static_cast<QWheelEvent*>(ev);
        QScrollBar* bar = m_scrollArea->horizontalScrollBar();
        const QPoint angle = wheel->angleDelta();
        if (bar->maximum() > 0 && qAbs(angle.y()) > qAbs(angle.x())) {
            const int delta = wheel->pixelDelta().y() != 0
                ? wheel->pixelDelta().y()
                : angle.y() / 2;   // 120 per notch → 60 px, about half a tab
            bar->setValue(bar->value() - delta);
            return true;
        }
    }
    if (handleStripDrag(watched, ev)) {
        return true;
    }
    return QWidget::eventFilter(watched, ev);
}

bool RadioTabBar::handleStripDrag(QObject* watched, QEvent* ev)
{
    if (!m_scrollArea) {
        return false;
    }
    const auto type = ev->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove
        && type != QEvent::MouseButtonRelease) {
        return false;
    }
    auto* tab = qobject_cast<RadioTab*>(watched);
    if (!tab && watched != m_tabHost && watched != m_scrollArea->viewport()) {
        return false;
    }
    auto* me = static_cast<QMouseEvent*>(ev);
    QScrollBar* bar = m_scrollArea->horizontalScrollBar();
    const int x = qRound(me->globalPosition().x());

    if (type == QEvent::MouseButtonPress) {
        m_dragArmed = me->button() == Qt::LeftButton && bar->maximum() > 0;
        m_dragging = false;
        m_dragPressX = x;
        m_dragStartValue = bar->value();
        m_dragPressedTab = tab;
        return false;   // the tab still sees the press: a still click selects it
    }
    if (!m_dragArmed) {
        return false;
    }
    if (type == QEvent::MouseMove) {
        if (!(me->buttons() & Qt::LeftButton)) {
            m_dragArmed = m_dragging = false;
            return false;
        }
        const int dx = x - m_dragPressX;
        if (!m_dragging && qAbs(dx) >= QApplication::startDragDistance()) {
            m_dragging = true;
            // A drag is not a click: release the tab's pressed state so the
            // swallowed release can't activate it.
            if (m_dragPressedTab) {
                m_dragPressedTab->setDown(false);
            }
            m_scrollArea->viewport()->setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            bar->setValue(m_dragStartValue - dx);
            return true;
        }
        return false;
    }
    // Release.
    const bool wasDragging = m_dragging;
    m_dragArmed = m_dragging = false;
    m_dragPressedTab = nullptr;
    if (wasDragging) {
        m_scrollArea->viewport()->unsetCursor();
        return true;
    }
    return false;
}

QSize RadioTabBar::sizeHint() const
{
    const int tabsWidth = m_tabsLayout ? m_tabsLayout->sizeHint().width() : 0;
    const int addWidth = m_addButton && m_addButton->isVisible()
        ? kStripSpacing + kAddButtonSize : 0;
    return QSize(tabsWidth + addWidth, kTabHeight);
}

QSize RadioTabBar::minimumSizeHint() const
{
    return QSize(kStripMinWidth, kTabHeight);
}

void RadioTabBar::setLinkIndicator(const QColor& overrideColor, bool alarm)
{
    m_linkOverride = overrideColor;
    if (m_alarm != alarm) {
        m_alarm = alarm;
        m_alarmVisible = true;
        // Blink only when the operator has blinking on.  When they don't, the
        // alarm holds solid red rather than falling back to a quiet dot: losing
        // the radio is the one thing that must stay visible either way, and
        // contest operators who disable blinking still need to see it.
        if (alarm && m_pulseEnabled) {
            m_alarmTimer->start();
        } else {
            m_alarmTimer->stop();
        }
    }
    applyLinkVisuals();
}

void RadioTabBar::pulseLink(const QColor& beatColor)
{
    m_beatColor = beatColor;
    if (!m_pulseEnabled) {
        // Blink disabled: the dot still carries the link state through colour,
        // it just doesn't animate.
        m_pulseLevel = 0.0;
        applyLinkVisuals();
        return;
    }
    m_pulseLevel = 1.0;
    if (!m_pulseTimer->isActive()) {
        m_pulseTimer->start();
    }
    applyLinkVisuals();
}

QString RadioTabBar::linkRadioId() const
{
    return m_activeId.isEmpty() ? m_carrierId : m_activeId;
}

void RadioTabBar::applyLinkVisuals()
{
    const qreal eased = m_pulseLevel * m_pulseLevel;
    RadioTab* carrier = linkCarrierTab();
    // "Link lost" names a radio.  Only the tab of the radio whose link this
    // is may show it; a fallback carrier shows its own status colour instead.
    const bool ownsLink = carrier && carrier->entry().id == linkRadioId();
    const bool alarm = m_alarm && ownsLink;
    const QColor linkOverride = (m_alarm && !ownsLink) ? QColor() : m_linkOverride;

    for (RadioTab* tab : std::as_const(m_tabs)) {
        // Only the carrier shows the link state — the others describe radios
        // this client is not talking to, so a heartbeat says nothing about them.
        const bool isCarrier = tab == carrier;
        tab->setLinkCarrier(isCarrier);
        tab->setLinkOverride(isCarrier ? linkOverride : QColor(),
                             isCarrier && alarm);
        tab->setAlarmVisible(m_alarmVisible);
        tab->setBeatColor(isCarrier ? m_beatColor : QColor());
        tab->setPulse(isCarrier ? eased : 0.0);
    }
}

RadioTab* RadioTabBar::linkCarrierTab() const
{
    const QString& id = linkRadioId();
    for (RadioTab* tab : m_tabs) {
        if (!id.isEmpty() && tab->entry().id == id
            && (tab->entry().visibleInTabs || id == m_activeId)) {
            return tab;
        }
    }
    // No session yet, or the dropped radio's tab is hidden: "searching" still
    // needs a tab to render on, so it rides the first visible one.  That tab
    // is not the link's radio, so applyLinkVisuals() never paints the alarm
    // on it.
    if (m_activeId.isEmpty()) {
        for (RadioTab* tab : m_tabs) {
            if (tab->entry().visibleInTabs) {
                return tab;
            }
        }
    }
    return nullptr;
}

void RadioTabBar::updateTabViewport(RadioTab* ensureVisible)
{
    if (!m_tabHost || !m_tabsLayout || !m_scrollArea) {
        return;
    }
    m_tabHost->setFixedWidth(qMax(0, m_tabsLayout->sizeHint().width()));
    updateGeometry();
    if (ensureVisible) {
        const QPointer<RadioTab> target(ensureVisible);
        QTimer::singleShot(0, m_scrollArea, [this, target]() {
            if (target && m_tabs.contains(target)) {
                m_scrollArea->ensureWidgetVisible(target, kStripSpacing, 0);
            }
        });
    }
}

void RadioTabBar::setPulseEnabled(bool on)
{
    if (m_pulseEnabled == on) {
        return;
    }
    m_pulseEnabled = on;
    if (!on) {
        m_pulseTimer->stop();
        m_alarmTimer->stop();
        m_pulseLevel = 0.0;
        // Freeze the alarm ON rather than wherever the blink happened to be —
        // see setLinkIndicator() for why a lost link stays visible regardless.
        m_alarmVisible = true;
    } else if (m_alarm) {
        m_alarmTimer->start();
    }
    applyLinkVisuals();
}

void RadioTabBar::setRadios(const QList<RadioTabEntry>& radios)
{
    if (m_radios == radios) {
        return;   // discovery re-announces every 5 s; don't churn the widgets
    }
    m_radios = radios;
    rebuild();
}

void RadioTabBar::setDiscoveredRadios(const QList<RadioTabEntry>& radios)
{
    if (m_discovered == radios) {
        return;
    }
    m_discovered = radios;
    refreshOpenPopover();
}

void RadioTabBar::refreshOpenPopover()
{
    if (!isDiscoveryPopoverVisible()) {
        return;
    }
    // A row's Actions menu is open: rebuilding would delete it under the
    // cursor.  Rebuild once it closes, from whatever the list is by then.
    auto* openMenu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    for (QWidget* w = openMenu ? openMenu->parentWidget() : nullptr; w; w = w->parentWidget()) {
        if (w != m_popover) {
            continue;
        }
        if (!m_popoverRefreshPending) {
            m_popoverRefreshPending = true;
            connect(openMenu, &QMenu::aboutToHide, this, [this]() {
                QTimer::singleShot(0, this, [this]() {
                    m_popoverRefreshPending = false;
                    refreshOpenPopover();
                });
            }, Qt::SingleShotConnection);
        }
        return;
    }
    const QLineEdit* search = m_popover->findChild<QLineEdit*>(QStringLiteral("radioSwitcherSearch"));
    const QString query = search ? search->text() : QString();
    showDiscoveryPopover();
    m_popover->findChild<QLineEdit*>(QStringLiteral("radioSwitcherSearch"))->setText(query);
}

void RadioTabBar::setActiveRadio(const QString& id)
{
    if (m_activeId == id) {
        return;
    }
    if (!id.isEmpty()) {
        // The link the heartbeat describes.  Kept through a drop — when the
        // active id goes empty — so a lost link is shown on the radio that
        // lost it, not on whichever tab happens to come first.
        m_carrierId = id;
    }
    m_activeId = id;
    applyActiveState();
}

void RadioTabBar::rebuild()
{
    // Reuse existing RadioTab widgets where the count allows, so a status-only
    // change (available → connected) never destroys the widget that currently
    // holds keyboard focus.
    while (m_tabs.size() > m_radios.size()) {
        RadioTab* tab = m_tabs.takeLast();
        m_tabsLayout->removeWidget(tab);
        tab->deleteLater();
    }
    while (m_tabs.size() < m_radios.size()) {
        auto* tab = new RadioTab(m_radios.at(m_tabs.size()), this);
        tab->installEventFilter(this);   // drag-to-scroll (handleStripDrag)
        connect(tab, &QAbstractButton::clicked, this, [this, tab]() {
            // A click opens the picker; it does not switch radios, so the
            // active tab changes only when the session does (setActiveRadio).
            // Re-assert the checked state: this checkable, ungrouped tab has
            // already toggled itself on press, and nothing else would restore
            // it for an unchanged active id.
            applyActiveState();
            emit radioActivated(tab->entry().id);
        });
        m_tabsLayout->addWidget(tab);
        m_tabs.append(tab);
    }
    for (int i = 0; i < m_radios.size(); ++i) {
        m_tabs[i]->setEntry(m_radios.at(i));
    }
    applyActiveState();
}

void RadioTabBar::applyActiveState()
{
    // In compact (minimal) mode only one tab is shown.  It must be the same tab
    // applyLinkVisuals() puts the link state on, or minimal mode hides the
    // carrier and takes the radio-link indicator with it — the exact loss the
    // strip is kept in minimal mode to prevent.  With no active radio that is
    // the first tab, not nothing.
    RadioTab* shown = linkCarrierTab();

    for (RadioTab* tab : std::as_const(m_tabs)) {
        const bool isActive = tab->entry().id == m_activeId;
        tab->setChecked(isActive);
        tab->setVisible(m_compact ? tab == shown : tab->entry().visibleInTabs);
    }
    if (m_addButton) {
        m_addButton->setVisible(!m_compact);
    }
    updateTabViewport(shown);
    applyLinkVisuals();
}

void RadioTabBar::setCompactMode(bool on)
{
    if (m_compact == on) {
        return;
    }
    m_compact = on;
    if (on && isDiscoveryPopoverVisible()) {
        m_popover->close();   // the "+" it belongs to is about to disappear
    }
    applyActiveState();
}

bool RadioTabBar::isDiscoveryPopoverVisible() const
{
    return m_popover && m_popover->isVisible();
}

void RadioTabBar::showDiscoveryPopover()
{
    if (m_popover) {
        m_popover->hide();
        m_popover->deleteLater();
        m_popover = nullptr;
    }

    auto* popover = new DiscoveryPopover(this);
    m_popover = popover;
    QVBoxLayout* rows = popover->rows();

    auto* heading = new QLabel(tr("Discovered radios"), popover->panel());
    heading->setObjectName(QStringLiteral("discoveredRadiosHeading"));
    ThemeManager::instance().applyStyleSheet(
        heading,
        QStringLiteral("QLabel { color: {{color.text.secondary}};"
                       " background: {{color.background.1}};"
                       " padding: 4px 10px 2px 10px; font-size: 10px;"
                       " font-weight: bold; }"));
    rows->addWidget(heading);

    auto* search = new QLineEdit(popover->panel());
    search->setObjectName(QStringLiteral("radioSwitcherSearch"));
    search->setAccessibleName(tr("Search radios"));
    search->setPlaceholderText(tr("Search name, address, or status"));
    search->setClearButtonEnabled(true);
    ThemeManager::instance().applyStyleSheet(search, QStringLiteral(
        "QLineEdit { background: {{color.background.0}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.strong}}; border-radius: 4px; padding: 7px; }"
        "QLineEdit:focus { border-color: {{color.border.accent}}; }"));
    rows->addWidget(search);

    auto* scroller = new QScrollArea(popover->panel());
    scroller->setObjectName(QStringLiteral("radioSwitcherScroller"));
    scroller->setWidgetResizable(true);
    scroller->setFrameShape(QFrame::NoFrame);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* list = new QWidget(scroller);
    ThemeManager::instance().applyStyleSheet(list, QStringLiteral(
        "QWidget { background: {{color.background.1}}; }"));
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(4);
    scroller->setWidget(list);
    rows->addWidget(scroller);

    auto* empty = new QLabel(tr("No matching radios"), list);
    empty->setObjectName(QStringLiteral("radioSwitcherEmpty"));
    ThemeManager::instance().applyStyleSheet(empty, QStringLiteral(
        "QLabel { background: {{color.background.1}}; color: {{color.text.secondary}}; padding: 10px; }"));
    listLayout->addWidget(empty);

    QList<RadioTabEntry> entries = m_discovered;
    std::stable_sort(entries.begin(), entries.end(), [this](const RadioTabEntry& first, const RadioTabEntry& second) {
        if ((first.id == m_activeId) != (second.id == m_activeId)) {
            return first.id == m_activeId;
        }
        return first.name.localeAwareCompare(second.name) < 0;
    });
    QList<QWidget*> radioRows;
    for (const RadioTabEntry& entry : entries) {
        auto* container = new QWidget(list);
        container->setProperty("radioSearchText", entry.name + ' ' + entry.model
            + ' ' + entry.transport
            + ' ' + entry.id + ' ' + radioTabStatusText(entry.status)
            // Both spellings: the filter is case-insensitive, so the token
            // already covers a typed "Connected" in English, but a translated
            // label is a different word and has to be searchable too.
            + ' ' + radioTabStatusLabel(entry.status));
        auto* rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);
        auto* row = new QPushButton(container);
        row->setObjectName(QStringLiteral("radioSwitcherRow_") + entry.id);
        row->setAccessibleIdentifier(row->objectName());
        row->setFocusPolicy(Qt::StrongFocus);
        row->setMinimumHeight(52);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont rowFont = ThemeManager::instance().font(row, QStringLiteral("font.family.ui"));
        rowFont.setPixelSize(12);
        row->setFont(rowFont);
        const QString status = entry.transport + middleDot() + radioTabStatusLabel(entry.status);
        const QFontMetrics metrics(rowFont);
        row->setText(metrics.elidedText(entry.name, Qt::ElideRight, 240)
            + '\n' + metrics.elidedText(status, Qt::ElideRight, 240));
        row->setToolTip(entry.name + '\n' + status);
        row->setAccessibleName(entry.name + QStringLiteral(", ") + status);
        ThemeManager::instance().applyStyleSheet(row, rowStyleTemplate());
        rowLayout->addWidget(row, 1);
        const QString id = entry.id;
        connect(row, &QPushButton::clicked, this, [this, id]() {
            if (m_popover) {
                m_popover->close();
            }
            emit radioActionRequested(id, QStringLiteral("restore"));
            emit radioActivated(id);
        });

        auto* actions = new QToolButton(container);
        actions->setObjectName(QStringLiteral("radioSwitcherActions_") + id);
        actions->setAccessibleIdentifier(actions->objectName());
        actions->setText(tr("Actions"));
        actions->setAccessibleName(tr("Actions for %1").arg(entry.name));
        actions->setFocusPolicy(Qt::StrongFocus);
        actions->setPopupMode(QToolButton::InstantPopup);
        actions->setFixedSize(72, 36);
        ThemeManager::instance().applyStyleSheet(actions, QStringLiteral(
            "QToolButton { background: {{color.background.1}}; color: {{color.text.primary}};"
            " border: 1px solid {{color.border.strong}}; border-radius: 4px; padding: 4px; }"
            "QToolButton:hover { background: {{color.background.2}}; }"
            "QToolButton:focus { border-color: {{color.border.accent}}; }"));
        auto* menu = new QMenu(actions);
        menu->setObjectName(QStringLiteral("radioSwitcherMenu_") + id);
        ThemeManager::instance().applyStyleSheet(menu, QStringLiteral(
            "QMenu { background: {{color.background.1}}; color: {{color.text.primary}};"
            " border: 1px solid {{color.border.strong}}; padding: 4px; }"
            "QMenu::item { padding: 8px 18px; }"
            "QMenu::item:selected { background: {{color.background.2}}; }"
            "QMenu::item:disabled { color: {{color.text.disabled}}; }"
            "QMenu::separator { height: 1px; background: {{color.border.strong}}; margin: 4px; }"));
        auto addAction = [this, menu, id](const QString& label, const QString& action, bool enabled) {
            QAction* item = menu->addAction(label);
            item->setObjectName(QStringLiteral("radioSwitcher_") + action + '_' + id);
            item->setEnabled(enabled);
            if (action == QStringLiteral("remove")) {
                item->setToolTip(tr("Hide this tab without deleting radio settings. Find it here again to restore it."));
            }
            connect(item, &QAction::triggered, this, [this, id, action]() {
                if (m_popover) {
                    m_popover->close();
                }
                emit radioActionRequested(id, action);
            });
        };
        const bool connected = entry.status == RadioTabStatus::Connected;
        addAction(tr("Disconnect"), QStringLiteral("disconnect"), connected);
        addAction(tr("Rename\u2026"), QStringLiteral("rename"), entry.canRename);
        addAction(tr("Radio setup\u2026"), QStringLiteral("setup"), connected);
        menu->addSeparator();
        addAction(entry.visibleInTabs ? tr("Remove from tabs") : tr("Add to tabs"),
                  entry.visibleInTabs ? QStringLiteral("remove") : QStringLiteral("restore"),
                  !connected);
        actions->setMenu(menu);
        rowLayout->addWidget(actions);
        listLayout->addWidget(container);
        radioRows.append(container);
    }
    listLayout->addStretch();
    auto filterRows = [radioRows, empty, this](const QString& query) {
        bool found = false;
        for (QWidget* row : radioRows) {
            const bool matches = row->property("radioSearchText").toString()
                .contains(query.trimmed(), Qt::CaseInsensitive);
            row->setVisible(matches);
            found = found || matches;
        }
        // "No matching" only makes sense once there is something to match.
        empty->setText(radioRows.isEmpty() ? tr("No radios found on the network yet")
                                           : tr("No matching radios"));
        empty->setVisible(!found);
    };
    connect(search, &QLineEdit::textChanged, popover, filterRows);
    filterRows(QString());
    scroller->setFixedHeight(qBound(64, entries.size() * 56, 336));
    popover->setFixedWidth(380);

    auto* separator = new QFrame(popover->panel());
    separator->setFrameShape(QFrame::HLine);
    ThemeManager::instance().applyStyleSheet(
        separator, QStringLiteral("QFrame { color: {{color.border.strong}};"
                                  " max-height: 1px; }"));
    rows->addWidget(separator);

    auto* manual = new QPushButton(tr("Connect manually\u2026"),
                                   popover->panel());
    manual->setFlat(true);
    manual->setCursor(Qt::PointingHandCursor);
    manual->setFocusPolicy(Qt::StrongFocus);
    manual->setObjectName(QStringLiteral("connectManuallyRow"));
    manual->setAccessibleName(tr("Connect manually"));
    ThemeManager::instance().applyStyleSheet(manual, rowStyleTemplate());
    connect(manual, &QPushButton::clicked, this, [this]() {
        if (m_popover) {
            m_popover->close();
        }
        emit connectManuallyRequested();
    });
    rows->addWidget(manual);

    auto* refresh = new QPushButton(tr("Rescan radios"), popover->panel());
    refresh->setObjectName(QStringLiteral("radioSwitcherRescan"));
    refresh->setAccessibleName(tr("Rescan radios"));
    ThemeManager::instance().applyStyleSheet(refresh, rowStyleTemplate());
    connect(refresh, &QPushButton::clicked, this, [this]() { emit rescanRequested(); });
    rows->addWidget(refresh);
    popover->adjustSize();

    // Anchor under the "+" button, then clamp into the screen so a radio strip
    // near the right edge doesn't push the panel off-screen.
    QPoint anchor = m_addButton->mapToGlobal(QPoint(0, m_addButton->height() + 6));
    if (QScreen* scr = (window() && window()->windowHandle())
                           ? window()->windowHandle()->screen()
                           : nullptr) {
        const QRect avail = scr->availableGeometry();
        popover->setFixedWidth(qMin(380, avail.width()));
        scroller->setFixedHeight(qMin(scroller->height(), qMax(48, avail.height() - 190)));
        popover->adjustSize();
        anchor.setX(qBound(avail.left(),
                           anchor.x(),
                           qMax(avail.left(), avail.right() - popover->width() + 1)));
        anchor.setY(qBound(avail.top(), anchor.y(), qMax(avail.top(), avail.bottom() - popover->height() + 1)));
    }
    popover->move(anchor);
    popover->show();
    search->setFocus(Qt::PopupFocusReason);
}

QVariantMap RadioTabBar::state() const
{
    QVariantList tabs;
    for (RadioTab* tab : std::as_const(m_tabs)) {
        const RadioTabEntry& e = tab->entry();
        // Screen rect so a driver (or a human debugging chrome) can aim a real
        // click at the control instead of guessing from a screenshot.
        const QRect screen(tab->mapToGlobal(QPoint(0, 0)), tab->size());
        tabs.append(QVariantMap{
            {QStringLiteral("id"), e.id},
            {QStringLiteral("screenRect"),
             QVariantList{screen.x(), screen.y(), screen.width(), screen.height()}},
            {QStringLiteral("name"), e.name},
            {QStringLiteral("status"), radioTabStatusText(e.status)},
            {QStringLiteral("model"), e.model},
            {QStringLiteral("statusLine"), tab->statusLine()},
            {QStringLiteral("transport"), e.transport},
            {QStringLiteral("active"), tab->isChecked()},
            {QStringLiteral("visible"), tab->isVisible()},
            {QStringLiteral("visibleInTabs"), e.visibleInTabs},
            {QStringLiteral("linkCarrier"), tab->isLinkCarrier()},
            {QStringLiteral("accessibleName"), tab->accessibleName()},
        });
    }

    QVariantList discovered;
    for (const RadioTabEntry& e : std::as_const(m_discovered)) {
        discovered.append(QVariantMap{
            {QStringLiteral("visibleInTabs"), e.visibleInTabs},
            {QStringLiteral("canRename"), e.canRename},
            {QStringLiteral("id"), e.id},
            {QStringLiteral("name"), e.name},
            {QStringLiteral("transport"), e.transport},
            {QStringLiteral("status"), radioTabStatusText(e.status)},
        });
    }

    return QVariantMap{
        {QStringLiteral("tabs"), tabs},
        {QStringLiteral("discovered"), discovered},
        {QStringLiteral("activeId"), m_activeId},
        {QStringLiteral("width"), width()},
        {QStringLiteral("maximumWidth"), maximumWidth()},
        {QStringLiteral("contentWidth"),
         m_tabsLayout ? m_tabsLayout->sizeHint().width() : 0},
        {QStringLiteral("overflowing"),
         m_tabHost && m_scrollArea
             ? m_tabHost->width() > m_scrollArea->viewport()->width()
             : false},
        {QStringLiteral("popoverVisible"), isDiscoveryPopoverVisible()},
        {QStringLiteral("pulseEnabled"), m_pulseEnabled},
        // The discovery heartbeat, which the active tab's dot now carries.
        // Exposed as a live level rather than a "beating" boolean so a caller
        // can sample it twice and see it move — a glow is not assertable from a
        // screenshot, and this indicator replaced one that was.
        {QStringLiteral("linkPulse"), m_pulseLevel},
        {QStringLiteral("linkAlarm"), m_alarm},
        {QStringLiteral("linkOverrideColor"),
         m_linkOverride.isValid() ? m_linkOverride.name() : QString()},
    };
}

} // namespace AetherSDR
