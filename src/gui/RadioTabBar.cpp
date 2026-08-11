#include "RadioTabBar.h"

#include "core/ThemeManager.h"

#include <QEnterEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QRadialGradient>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantList>
#include <QWindow>
#include <QtMath>

namespace AetherSDR {

namespace {

constexpr int  kTabHeight     = 40;   // 52 px bar − 6 px above/below
constexpr int  kTabRadius     = 8;
constexpr int  kTabPadX       = 14;
constexpr int  kTabPadY       = 6;
constexpr int  kDotDiameter   = 7;
constexpr int  kDotTextGap    = 8;
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

// ─────────────────────────────────────────────────────────────────────────────
// RadioTab
// ─────────────────────────────────────────────────────────────────────────────

RadioTab::RadioTab(const RadioTabEntry& entry, QWidget* parent)
    : QAbstractButton(parent), m_entry(entry)
{
    setObjectName(QStringLiteral("radioTab_") + entry.id);
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
    QString line = m_entry.name.isEmpty()
        ? radioTabStatusText(m_entry.status)
        : m_entry.name + middleDot() + radioTabStatusText(m_entry.status);
    if (!m_entry.detail.isEmpty()) {
        line += middleDot() + m_entry.detail;
    }
    return line;
}

void RadioTab::refreshAccessibility()
{
    setText(m_entry.name);
    // The status is in the accessible name as well as on screen — a screen
    // reader user must not have to infer it from the dot's colour.
    setAccessibleName(QStringLiteral("Radio %1, %2")
                          .arg(m_entry.name, radioTabStatusText(m_entry.status)));
    setAccessibleDescription(statusLine());
    setToolTip(statusLine());
}

void RadioTab::setEntry(const RadioTabEntry& entry)
{
    if (m_entry == entry) {
        return;
    }
    m_entry = entry;
    setObjectName(QStringLiteral("radioTab_") + entry.id);
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
    update();
}

void RadioTab::setLinkOverride(const QColor& overrideColor, bool alarm)
{
    if (m_overrideColor == overrideColor && m_alarm == alarm) {
        return;
    }
    m_overrideColor = overrideColor;
    m_alarm = alarm;
    if (!alarm) {
        m_alarmVisible = true;
    }
    update();
}

void RadioTab::setAlarmVisible(bool on)
{
    if (m_alarmVisible == on) {
        return;
    }
    m_alarmVisible = on;
    if (m_alarm) {
        update();
    }
}

void RadioTab::setBeatColor(const QColor& color)
{
    if (m_beatColor == color) {
        return;
    }
    m_beatColor = color;
    update();
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
        setAccessibleName(QStringLiteral("Discovered radios"));

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
        " background: transparent;"
        " color: {{color.text.primary}}; }"
        "QPushButton:hover  { background: {{color.titlebar.tab.hover}}; }"
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

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(6);

    m_addButton = new QToolButton(this);
    m_addButton->setObjectName(QStringLiteral("radioTabAddButton"));
    m_addButton->setText(QStringLiteral("+"));
    m_addButton->setFixedSize(28, kTabHeight);
    m_addButton->setCursor(Qt::PointingHandCursor);
    m_addButton->setFocusPolicy(Qt::StrongFocus);
    m_addButton->setAccessibleName(QStringLiteral("Add radio"));
    m_addButton->setAccessibleDescription(
        QStringLiteral("Open the list of discovered radios"));
    m_addButton->setToolTip(QStringLiteral("Discovered radios"));
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
            "QToolButton:focus { border: 1px solid {{color.border.accent}}; }"));
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

void RadioTabBar::applyLinkVisuals()
{
    const qreal eased = m_pulseLevel * m_pulseLevel;

    // With no active radio the link state still has something to say —
    // "searching" is reported precisely when nothing is connected, which is the
    // state the indicator matters most in, and it is the whole reason the strip
    // survives minimal mode.  Keying the carrier purely off m_activeId left it
    // rendering nowhere in exactly that case, so fall back to the first tab.
    // Once a radio is active it carries the state, as before.
    RadioTab* carrier = nullptr;
    for (RadioTab* tab : std::as_const(m_tabs)) {
        if (tab->entry().id == m_activeId) { carrier = tab; break; }
    }
    if (!carrier && !m_activeId.isEmpty()) {
        // An active id that matches no tab yet (the strip is mid-rebuild) is
        // not the disconnected case — leave the state uncarried rather than
        // painting it onto an unrelated radio.
        carrier = nullptr;
    } else if (!carrier && !m_tabs.isEmpty()) {
        carrier = m_tabs.first();
    }

    for (RadioTab* tab : std::as_const(m_tabs)) {
        // Only the carrier shows the link state — the others describe radios
        // this client is not talking to, so a heartbeat says nothing about them.
        const bool isCarrier = tab == carrier;
        tab->setLinkCarrier(isCarrier);
        tab->setLinkOverride(isCarrier ? m_linkOverride : QColor(),
                             isCarrier && m_alarm);
        tab->setAlarmVisible(m_alarmVisible);
        tab->setBeatColor(isCarrier ? m_beatColor : QColor());
        tab->setPulse(isCarrier ? eased : 0.0);
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
    m_discovered = radios;
}

void RadioTabBar::setActiveRadio(const QString& id)
{
    if (m_activeId == id) {
        return;
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
        m_layout->removeWidget(tab);
        tab->deleteLater();
    }
    while (m_tabs.size() < m_radios.size()) {
        auto* tab = new RadioTab(m_radios.at(m_tabs.size()), this);
        connect(tab, &QAbstractButton::clicked, this, [this, tab]() {
            // Deliberately does NOT claim the clicked tab as active.  MainWindow
            // treats a single click as "show me the picker", not "switch" — so
            // moving m_activeId here would leave the strip, and the bridge's
            // activeId, asserting a radio the client never connected to, with
            // the link visuals following the wrong tab.  The active tab changes
            // only when the session does, via setActiveRadio() from
            // refreshRadioTabs().
            //
            // applyActiveState() re-asserts the checked state because
            // QAbstractButton has already toggled this tab on press: the tabs
            // are checkable and belong to no exclusive group, so clicking the
            // ALREADY-active tab would otherwise leave it unchecked for good
            // (setActiveRadio() early-returns on an unchanged id, and
            // setRadios() early-returns on an unchanged list, so nothing else
            // ever restores it).
            applyActiveState();
            emit radioActivated(tab->entry().id);
        });
        // Insert before the "+" button, which always stays last.
        m_layout->insertWidget(m_tabs.size(), tab);
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
    RadioTab* shown = nullptr;
    for (RadioTab* tab : std::as_const(m_tabs)) {
        if (tab->entry().id == m_activeId) { shown = tab; break; }
    }
    if (!shown && m_activeId.isEmpty() && !m_tabs.isEmpty()) {
        shown = m_tabs.first();
    }

    for (RadioTab* tab : std::as_const(m_tabs)) {
        const bool isActive = tab->entry().id == m_activeId;
        tab->setChecked(isActive);
        tab->setVisible(!m_compact || tab == shown);
    }
    if (m_addButton) {
        m_addButton->setVisible(!m_compact);
    }
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
        m_popover->deleteLater();
        m_popover = nullptr;
    }

    auto* popover = new DiscoveryPopover(this);
    m_popover = popover;
    QVBoxLayout* rows = popover->rows();

    auto* heading = new QLabel(QStringLiteral("Discovered radios"), popover->panel());
    ThemeManager::instance().applyStyleSheet(
        heading,
        QStringLiteral("QLabel { color: {{color.text.secondary}};"
                       " padding: 4px 10px 2px 10px; font-size: 10px;"
                       " font-weight: bold; }"));
    rows->addWidget(heading);

    const QString monoTemplate = QStringLiteral(
        "QLabel { color: {{color.text.secondary}};"
        " font-family: \"{{font.family.mono}}\", \"JetBrains Mono\", monospace;"
        " font-size: 10px; }");

    if (m_discovered.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("No radios on the network"),
                                 popover->panel());
        ThemeManager::instance().applyStyleSheet(empty, monoTemplate);
        empty->setContentsMargins(10, 4, 10, 4);
        rows->addWidget(empty);
    }

    for (const RadioTabEntry& entry : std::as_const(m_discovered)) {
        auto* row = new QPushButton(popover->panel());
        row->setFlat(true);
        row->setCursor(Qt::PointingHandCursor);
        row->setFocusPolicy(Qt::StrongFocus);
        ThemeManager::instance().applyStyleSheet(row, rowStyleTemplate());

        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(10, 5, 10, 5);
        rowLayout->setSpacing(1);

        auto* name = new QLabel(entry.name, row);
        ThemeManager::instance().applyStyleSheet(
            name, QStringLiteral("QLabel { color: {{color.text.primary}};"
                                 " background: transparent; font-size: 12px;"
                                 " font-weight: bold; }"));
        rowLayout->addWidget(name);

        auto* transport = new QLabel(entry.transport, row);
        ThemeManager::instance().applyStyleSheet(transport, monoTemplate);
        rowLayout->addWidget(transport);

        // Labels would otherwise eat the click meant for the button.
        name->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        transport->setAttribute(Qt::WA_TransparentForMouseEvents, true);

        row->setAccessibleName(
            QStringLiteral("%1, %2, %3")
                .arg(entry.name, entry.transport, radioTabStatusText(entry.status)));

        const QString id = entry.id;
        connect(row, &QPushButton::clicked, this, [this, id]() {
            if (m_popover) {
                m_popover->close();
            }
            // Same rule as a tab click: choosing a row is a request, not a
            // connection.  The active tab follows the session, never the
            // gesture that asked for it.
            emit radioActivated(id);
        });
        rows->addWidget(row);
    }

    auto* separator = new QFrame(popover->panel());
    separator->setFrameShape(QFrame::HLine);
    ThemeManager::instance().applyStyleSheet(
        separator, QStringLiteral("QFrame { color: {{color.border.strong}};"
                                  " max-height: 1px; }"));
    rows->addWidget(separator);

    auto* manual = new QPushButton(QStringLiteral("Connect manually\xE2\x80\xA6"),
                                   popover->panel());
    manual->setFlat(true);
    manual->setCursor(Qt::PointingHandCursor);
    manual->setFocusPolicy(Qt::StrongFocus);
    manual->setObjectName(QStringLiteral("connectManuallyRow"));
    manual->setAccessibleName(QStringLiteral("Connect manually"));
    ThemeManager::instance().applyStyleSheet(manual, rowStyleTemplate());
    connect(manual, &QPushButton::clicked, this, [this]() {
        if (m_popover) {
            m_popover->close();
        }
        emit connectManuallyRequested();
    });
    rows->addWidget(manual);

    popover->adjustSize();

    // Anchor under the "+" button, then clamp into the screen so a radio strip
    // near the right edge doesn't push the panel off-screen.
    QPoint anchor = m_addButton->mapToGlobal(QPoint(0, m_addButton->height() + 6));
    if (QScreen* scr = (window() && window()->windowHandle())
                           ? window()->windowHandle()->screen()
                           : nullptr) {
        const QRect avail = scr->availableGeometry();
        anchor.setX(qBound(avail.left(),
                           anchor.x(),
                           avail.right() - popover->width()));
        anchor.setY(qMin(anchor.y(), avail.bottom() - popover->height()));
    }
    popover->move(anchor);
    popover->show();
    if (!m_discovered.isEmpty()) {
        popover->panel()->setFocus(Qt::TabFocusReason);
    }
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
            {QStringLiteral("statusLine"), tab->accessibleDescription()},
            {QStringLiteral("transport"), e.transport},
            {QStringLiteral("active"), tab->isChecked()},
            {QStringLiteral("linkCarrier"), tab->isLinkCarrier()},
            {QStringLiteral("accessibleName"), tab->accessibleName()},
        });
    }

    QVariantList discovered;
    for (const RadioTabEntry& e : std::as_const(m_discovered)) {
        discovered.append(QVariantMap{
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
