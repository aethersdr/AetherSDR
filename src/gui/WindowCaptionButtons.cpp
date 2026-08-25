#include "WindowCaptionButtons.h"

#include "core/ThemeManager.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QVariantList>

namespace AetherSDR {

namespace {

// Design 1e (Windows): full title-bar height, 46 px wide, glyphs at 11 px.
constexpr int kWinButtonW = 46;
constexpr int kWinButtonH = 52;
constexpr int kGlyphBox   = 11;

// Design 1b (Linux): 13 px bordered chips, 4 px radius, 8 px apart.
constexpr int kChipSize    = 13;
constexpr int kChipRadius  = 4;
constexpr int kChipSpacing = 8;
constexpr int kChipHitPad  = 5;   // click target padding — 13 px alone is too small

// macOS: the platform's own metrics — 12 px lights on a 20 px pitch, i.e. an
// 8 px gap.  Expressed as a fixed slot width with zero layout spacing rather
// than as padding plus spacing, because the pitch is the number that has to
// match: these sit beside every other Mac window the operator has open, and a
// near-miss reads as broken.  The slot is taller than the light so the click
// target stays comfortable.
constexpr int kLightSize      = 12;
constexpr int kLightSlotWidth = 20;   // centre-to-centre pitch
constexpr int kLightSlotHeight = 22;

QString semanticControlToken(CaptionButton::Role role)
{
    switch (role) {
        case CaptionButton::Role::Close:
            return QStringLiteral("color.titlebar.caption.semantic.close");
        case CaptionButton::Role::Minimize:
            return QStringLiteral("color.titlebar.caption.semantic.minimize");
        case CaptionButton::Role::MaximizeRestore:
            return QStringLiteral("color.titlebar.caption.semantic.maximize");
    }
    return QStringLiteral("color.titlebar.caption.semantic.inactive");
}

QColor semanticControlColor(const QWidget* widget, CaptionButton::Role role,
                            bool windowActive = true)
{
    const QString token = windowActive
        ? semanticControlToken(role)
        : QStringLiteral("color.titlebar.caption.semantic.inactive");
    return ThemeManager::instance().color(widget, token);
}

QString roleAccessibleName(CaptionButton::Role role, bool maximized)
{
    switch (role) {
        case CaptionButton::Role::Minimize: return QStringLiteral("Minimize window");
        case CaptionButton::Role::MaximizeRestore:
            return maximized ? QStringLiteral("Restore window")
                             : QStringLiteral("Maximize window");
        case CaptionButton::Role::Close:    return QStringLiteral("Close window");
    }
    return QString();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// CaptionButton
// ─────────────────────────────────────────────────────────────────────────────

CaptionButton::CaptionButton(Role role, CaptionStyle style, QWidget* parent)
    : QAbstractButton(parent), m_role(role), m_style(style)
{
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(roleAccessibleName(role, false));
    setToolTip(accessibleName());

    switch (style) {
        case CaptionStyle::WindowsCaption:
            setFixedSize(kWinButtonW, kWinButtonH);
            setObjectName(QStringLiteral("captionButtonWin"));
            break;
        case CaptionStyle::MacTrafficLights:
            setFixedSize(kLightSlotWidth, kLightSlotHeight);
            setObjectName(QStringLiteral("captionButtonLight"));
            break;
        case CaptionStyle::LinuxChips:
            setFixedSize(kChipSize + 2 * kChipHitPad, kChipSize + 2 * kChipHitPad);
            setObjectName(QStringLiteral("captionButtonChip"));
            break;
    }

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, qOverload<>(&QWidget::update));
}

void CaptionButton::setMaximized(bool maximized)
{
    if (m_maximized == maximized) {
        return;
    }
    m_maximized = maximized;
    if (m_role == Role::MaximizeRestore) {
        setAccessibleName(roleAccessibleName(m_role, maximized));
        setToolTip(accessibleName());
        update();
    }
}

void CaptionButton::setForcedHover(bool hovered)
{
    if (m_forcedHover == hovered) {
        return;
    }
    m_forcedHover = hovered;
    update();
}

void CaptionButton::setGroupHovered(bool hovered)
{
    if (m_groupHovered == hovered) {
        return;
    }
    m_groupHovered = hovered;
    update();
}

void CaptionButton::setWindowActive(bool active)
{
    if (m_windowActive == active) {
        return;
    }
    m_windowActive = active;
    update();
}

void CaptionButton::enterEvent(QEnterEvent* ev)
{
    m_hovered = true;
    if (auto* cluster = qobject_cast<WindowCaptionButtons*>(parentWidget())) {
        cluster->childHoverChanged(true);
    }
    update();
    QAbstractButton::enterEvent(ev);
}

void CaptionButton::leaveEvent(QEvent* ev)
{
    m_hovered = false;
    if (auto* cluster = qobject_cast<WindowCaptionButtons*>(parentWidget())) {
        cluster->childHoverChanged(false);
    }
    update();
    QAbstractButton::leaveEvent(ev);
}

void CaptionButton::focusInEvent(QFocusEvent* ev)
{
    switch (ev->reason()) {
        case Qt::TabFocusReason:
        case Qt::BacktabFocusReason:
        case Qt::ShortcutFocusReason:
            m_focusVisible = true;
            break;
        default:
            m_focusVisible = false;
            break;
    }
    update();
    QAbstractButton::focusInEvent(ev);
}

void CaptionButton::focusOutEvent(QFocusEvent* ev)
{
    m_focusVisible = false;
    update();
    QAbstractButton::focusOutEvent(ev);
}

void CaptionButton::paintTrafficLight(QPainter& p) const
{
    const QRectF light(QPointF((width() - kLightSize) / 2.0,
                               (height() - kLightSize) / 2.0),
                       QSizeF(kLightSize, kLightSize));

    QColor fill = semanticControlColor(this, m_role, m_windowActive);
    if (isDown()) {
        fill = fill.darker(120);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(light);

    if (m_focusVisible) {
        p.setPen(QPen(ThemeManager::instance().color(
                          this, QStringLiteral("color.border.accent")), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(light.adjusted(-2.5, -2.5, 2.5, 2.5));
    }

    // Glyphs appear on hovering ANY light in the cluster, never one at a time —
    // that is the platform behaviour, and showing a lone glyph would read as a
    // different control.
    if (!m_groupHovered || !m_windowActive) {
        return;
    }

    const QColor semanticGlyph = ThemeManager::instance().color(
        this, QStringLiteral("color.titlebar.caption.semantic.glyph"));
    QPen glyphPen(semanticGlyph, 1.2);
    glyphPen.setCapStyle(Qt::RoundCap);
    p.setPen(glyphPen);
    p.setBrush(Qt::NoBrush);

    const QPointF c = light.center();
    constexpr qreal kArm = 2.6;
    switch (m_role) {
        case Role::Close:
            p.drawLine(QPointF(c.x() - kArm, c.y() - kArm),
                       QPointF(c.x() + kArm, c.y() + kArm));
            p.drawLine(QPointF(c.x() + kArm, c.y() - kArm),
                       QPointF(c.x() - kArm, c.y() + kArm));
            break;
        case Role::Minimize:
            p.drawLine(QPointF(c.x() - 3.2, c.y()), QPointF(c.x() + 3.2, c.y()));
            break;
        case Role::MaximizeRestore: {
            // Two opposed filled triangles — the platform's zoom mark.  Pointing
            // outward when the window can grow, inward when it is already zoomed.
            p.setPen(Qt::NoPen);
            p.setBrush(semanticGlyph);
            QPainterPath a, b;
            if (m_maximized) {
                a.moveTo(c.x() - 3.4, c.y() - 3.4);
                a.lineTo(c.x() - 0.2, c.y() - 3.4);
                a.lineTo(c.x() - 3.4, c.y() - 0.2);
                b.moveTo(c.x() + 3.4, c.y() + 3.4);
                b.lineTo(c.x() + 0.2, c.y() + 3.4);
                b.lineTo(c.x() + 3.4, c.y() + 0.2);
            } else {
                a.moveTo(c.x() - 3.4, c.y() + 3.4);
                a.lineTo(c.x() - 3.4, c.y() + 0.2);
                a.lineTo(c.x() - 0.2, c.y() + 3.4);
                b.moveTo(c.x() + 3.4, c.y() - 3.4);
                b.lineTo(c.x() + 3.4, c.y() - 0.2);
                b.lineTo(c.x() + 0.2, c.y() - 3.4);
            }
            a.closeSubpath();
            b.closeSubpath();
            p.drawPath(a);
            p.drawPath(b);
            break;
        }
    }
}

void CaptionButton::paintGlyph(QPainter& p, const QColor& color) const
{
    const QRectF box(QPointF((width() - kGlyphBox) / 2.0,
                             (height() - kGlyphBox) / 2.0),
                     QSizeF(kGlyphBox, kGlyphBox));

    QPen pen(color, 1.0);
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Half-pixel offsets keep 1 px strokes crisp on integer device pixels.
    const qreal l = qRound(box.left()) + 0.5;
    const qreal t = qRound(box.top()) + 0.5;
    const qreal r = qRound(box.right()) - 0.5;
    const qreal b = qRound(box.bottom()) - 0.5;

    switch (m_role) {
        case Role::Minimize: {
            const qreal y = qRound(box.center().y()) + 0.5;
            p.drawLine(QPointF(l, y), QPointF(r, y));
            break;
        }
        case Role::MaximizeRestore: {
            if (m_maximized) {
                // Restore: the front square plus the back square's exposed
                // top-right corner, the standard two-window mark.
                const qreal off = 2.5;
                p.drawRect(QRectF(l, t + off, r - l - off, b - t - off));
                QPainterPath back;
                back.moveTo(l + off, t + off);
                back.lineTo(l + off, t);
                back.lineTo(r, t);
                back.lineTo(r, b - off);
                back.lineTo(r - off, b - off);
                p.drawPath(back);
            } else {
                p.drawRect(QRectF(l, t, r - l, b - t));
            }
            break;
        }
        case Role::Close: {
            p.drawLine(QPointF(l, t), QPointF(r, b));
            p.drawLine(QPointF(r, t), QPointF(l, b));
            break;
        }
    }
}

void CaptionButton::paintEvent(QPaintEvent* ev)
{
    Q_UNUSED(ev);
    auto& theme = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_style == CaptionStyle::MacTrafficLights) {
        paintTrafficLight(p);
        return;
    }

    if (m_style == CaptionStyle::WindowsCaption) {
        QColor glyph = theme.color(this, QStringLiteral("color.titlebar.caption.glyph"));
        if (isHot() || isDown()) {
            if (m_role == Role::Close) {
                p.fillRect(rect(),
                           theme.color(this, QStringLiteral("color.titlebar.caption.close.hover")));
                glyph = theme.color(this, QStringLiteral("color.titlebar.caption.close.glyph"));
            } else {
                p.fillRect(rect(),
                           theme.color(this, QStringLiteral("color.titlebar.caption.hover")));
                glyph = theme.color(this, QStringLiteral("color.titlebar.caption.glyph.hover"));
            }
        }
        if (m_focusVisible) {
            p.setPen(QPen(theme.color(this, QStringLiteral("color.border.accent")), 1));
            p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        }
        paintGlyph(p, glyph);
        return;
    }

    // ── Linux chips (design 1b) ─────────────────────────────────────────────
    const QRectF chip(QPointF((width() - kChipSize) / 2.0,
                              (height() - kChipSize) / 2.0),
                      QSizeF(kChipSize, kChipSize));
    QPainterPath shape;
    shape.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5),
                         kChipRadius, kChipRadius);

    const bool hot = isHot() || isDown();
    p.fillPath(shape, hot ? semanticControlColor(this, m_role)
                          : theme.color(this, QStringLiteral("color.background.1")));
    p.setPen(QPen(theme.color(this, m_focusVisible
                                        ? QStringLiteral("color.border.accent")
                                        : QStringLiteral("color.border.strong")),
                  1));
    p.drawPath(shape);

    if (hot) {
        // Black glyph on the filled chip — the design's hover affordance, and
        // black on all three fills clears 4.5:1.
        QPen pen(theme.color(
                     this, QStringLiteral("color.titlebar.caption.semantic.glyph")),
                 1.0);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        const qreal l = qRound(chip.left() + 3) + 0.5;
        const qreal t = qRound(chip.top() + 3) + 0.5;
        const qreal r = qRound(chip.right() - 3) - 0.5;
        const qreal b = qRound(chip.bottom() - 3) - 0.5;
        switch (m_role) {
            case Role::Minimize: {
                const qreal y = qRound(chip.center().y()) + 0.5;
                p.drawLine(QPointF(l, y), QPointF(r, y));
                break;
            }
            case Role::MaximizeRestore:
                p.drawRect(QRectF(l, t, r - l, b - t));
                break;
            case Role::Close:
                p.drawLine(QPointF(l, t), QPointF(r, b));
                p.drawLine(QPointF(r, t), QPointF(l, b));
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowCaptionButtons
// ─────────────────────────────────────────────────────────────────────────────

WindowCaptionButtons::WindowCaptionButtons(CaptionStyle style, QWidget* parent)
    : QWidget(parent), m_style(style)
{
    setObjectName(QStringLiteral("windowCaptionButtons"));
    setAccessibleName(QStringLiteral("Window controls"));

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    switch (style) {
        case CaptionStyle::WindowsCaption:   m_layout->setSpacing(0); break;
        case CaptionStyle::MacTrafficLights: m_layout->setSpacing(0); break;
        case CaptionStyle::LinuxChips:       m_layout->setSpacing(kChipSpacing); break;
    }

    m_minimize = new CaptionButton(CaptionButton::Role::Minimize, style, this);
    m_maximize = new CaptionButton(CaptionButton::Role::MaximizeRestore, style, this);
    m_close    = new CaptionButton(CaptionButton::Role::Close, style, this);

    // Close is rightmost on Windows and leftmost everywhere else — the platforms
    // put the cluster on opposite sides of the bar, and the destructive control
    // keeps its conventional outboard position on each.  macOS additionally
    // fixes the order as close / minimize / zoom, which is what the Linux chips
    // already follow.
    if (style == CaptionStyle::WindowsCaption) {
        m_layout->addWidget(m_minimize);
        m_layout->addWidget(m_maximize);
        m_layout->addWidget(m_close);
    } else {
        m_layout->addWidget(m_close);
        m_layout->addWidget(m_minimize);
        m_layout->addWidget(m_maximize);
    }

    connect(m_minimize, &QAbstractButton::clicked,
            this, &WindowCaptionButtons::minimizeRequested);
    connect(m_maximize, &QAbstractButton::clicked,
            this, &WindowCaptionButtons::maximizeRestoreRequested);
    connect(m_close, &QAbstractButton::clicked,
            this, &WindowCaptionButtons::closeRequested);
}

void WindowCaptionButtons::setMaximized(bool maximized)
{
    m_maximize->setMaximized(maximized);
}

void WindowCaptionButtons::childHoverChanged(bool entered)
{
    // Counted rather than latched: moving the pointer straight from one light
    // to the next fires the new button's enter before the old one's leave, and
    // a boolean would blink the glyphs off for a frame at every crossing.
    m_hoveredChildren = qMax(0, m_hoveredChildren + (entered ? 1 : -1));
    const bool hovered = m_hoveredChildren > 0;
    for (CaptionButton* b : {m_close, m_minimize, m_maximize}) {
        b->setGroupHovered(hovered);
    }
}

void WindowCaptionButtons::showEvent(QShowEvent* ev)
{
    QWidget::showEvent(ev);
    if (QWidget* w = window()) {
        w->installEventFilter(this);
        for (CaptionButton* b : {m_close, m_minimize, m_maximize}) {
            b->setWindowActive(w->isActiveWindow());
        }
    }
}

bool WindowCaptionButtons::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == window() && ev->type() == QEvent::ActivationChange) {
        const bool active = window()->isActiveWindow();
        for (CaptionButton* b : {m_close, m_minimize, m_maximize}) {
            b->setWindowActive(active);
        }
    }
    return QWidget::eventFilter(obj, ev);
}

QRect WindowCaptionButtons::maximizeButtonGlobalRect() const
{
    if (!m_maximize || !m_maximize->isVisible()) {
        return {};
    }
    return QRect(m_maximize->mapToGlobal(QPoint(0, 0)), m_maximize->size());
}

void WindowCaptionButtons::setMaximizeForcedHover(bool hovered)
{
    m_maximize->setForcedHover(hovered);
}

QVariantMap WindowCaptionButtons::state() const
{
    auto describe = [](const CaptionButton* b) {
        return QVariantMap{
            {QStringLiteral("accessibleName"), b->accessibleName()},
            {QStringLiteral("enabled"), b->isEnabled()},
            {QStringLiteral("visible"), b->isVisible()},
            {QStringLiteral("width"), b->width()},
            {QStringLiteral("height"), b->height()},
        };
    };
    return QVariantMap{
        {QStringLiteral("style"), [this]() -> QString {
             switch (m_style) {
                 case CaptionStyle::WindowsCaption:   return QStringLiteral("windows");
                 case CaptionStyle::MacTrafficLights: return QStringLiteral("macTrafficLights");
                 case CaptionStyle::LinuxChips:       return QStringLiteral("linuxChips");
             }
             return QString();
         }()},
        {QStringLiteral("minimize"), describe(m_minimize)},
        {QStringLiteral("maximize"), describe(m_maximize)},
        {QStringLiteral("close"), describe(m_close)},
    };
}

} // namespace AetherSDR
