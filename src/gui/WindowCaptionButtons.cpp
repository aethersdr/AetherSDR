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

constexpr int kGlyphBox = 11;

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

CaptionButton::CaptionButton(Role role, QWidget* parent)
    : QAbstractButton(parent), m_role(role)
{
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(roleAccessibleName(role, false));
    setToolTip(accessibleName());

    setFixedSize(36, 36);
    setObjectName(QStringLiteral("captionButton"));
    setAccessibleIdentifier(accessibleName());

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

void CaptionButton::enterEvent(QEnterEvent* ev)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(ev);
}

void CaptionButton::leaveEvent(QEvent* ev)
{
    m_hovered = false;
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
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowCaptionButtons
// ─────────────────────────────────────────────────────────────────────────────

WindowCaptionButtons::WindowCaptionButtons(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("windowCaptionButtons"));
    setAccessibleName(QStringLiteral("Window controls"));

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);

    m_minimize = new CaptionButton(CaptionButton::Role::Minimize, this);
    m_maximize = new CaptionButton(CaptionButton::Role::MaximizeRestore, this);
    m_close    = new CaptionButton(CaptionButton::Role::Close, this);

    m_layout->addWidget(m_minimize);
    m_layout->addWidget(m_maximize);
    m_layout->addWidget(m_close);

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
        {QStringLiteral("style"), QStringLiteral("shared")},
        {QStringLiteral("minimize"), describe(m_minimize)},
        {QStringLiteral("maximize"), describe(m_maximize)},
        {QStringLiteral("close"), describe(m_close)},
    };
}

} // namespace AetherSDR
