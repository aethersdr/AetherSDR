#include "CanonWindow.h"

#include "FramelessMoveHelper.h"

#include "core/ThemeManager.h"

#include <QConicalGradient>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace AetherSDR {

namespace {
constexpr int kCloseSize = 26;
constexpr int kCloseMargin = 12;
constexpr int kGridStep = 64;
constexpr int kSparkLapMs = 7000;
constexpr int kSparkFrameMs = 33;

// A spark frame is wasted on a window nobody can see. Qt sends no hideEvent
// when a window is minimised or covered, but the window stops being exposed.
bool sparkVisible(const QWidget* w)
{
    const QWindow* handle = w->window()->windowHandle();
    return handle && handle->isExposed();
}
} // namespace

CanonWindow::CanonWindow(const QString& title, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    // The app stylesheet gives dialogs an opaque background, which would fill
    // the square behind the rounded corners. This window paints its own ground.
    ThemeManager::instance().applyStyleSheet(
        this, "AetherSDR--CanonWindow { background: transparent; border: none; }"
              "QWidget#canonBody { background: transparent; }");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kInset, kInset, kInset, kInset);
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("canonBody"));
    m_body->setAttribute(Qt::WA_TranslucentBackground);
    outer->addWidget(m_body);

    auto& tm = ThemeManager::instance();
    m_close = new QToolButton(this);
    m_close->setText(QStringLiteral("✕"));
    m_close->setAccessibleName(QStringLiteral("Close"));
    m_close->setToolTip(QStringLiteral("Close (Esc)"));
    m_close->setFixedSize(kCloseSize, kCloseSize);
    m_close->setCursor(Qt::PointingHandCursor);
    tm.applyStyleSheet(m_close,
        "QToolButton { background: transparent; color: {{color.canon.muted}}; border: 1px solid transparent; "
        "border-radius: 13px; font-size: 12px; }"
        "QToolButton:hover { background: {{color.canon.nested}}; color: {{color.canon.ink}}; border-color: {{color.canon.lineHi}}; }"
        "QToolButton:focus { border: 2px solid {{color.canon.cyan}}; }");
    connect(m_close, &QToolButton::clicked, this, &QDialog::close);

    // QDialog handles Escape itself; a caption-less window gets no Close
    // shortcut from the platform, so ⌘W / Ctrl+W is wired here.
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::reject);

    // The ground is painted from tokens; repaint it when the theme changes.
    connect(&tm, &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void CanonWindow::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    m_close->move(width() - kCloseSize - kCloseMargin, kCloseMargin);
    m_close->raise();
}

void CanonWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_placed) {
        return;
    }
    m_placed = true;
    // Centre over the parent window (or the screen) the first time it opens.
    QRect anchor;
    if (parentWidget()) {
        anchor = parentWidget()->window()->frameGeometry();
    } else if (const QScreen* s = QGuiApplication::primaryScreen()) {
        anchor = s->availableGeometry();
    }
    if (anchor.isValid()) {
        move(anchor.center() - rect().center());
    }
}

void CanonWindow::mousePressEvent(QMouseEvent* event)
{
    // Empty areas of the window move it (the window has no title bar).
    // FramelessMoveHelper picks startSystemMove() or the #4827 manual move:
    // startSystemMove() silently fails on xcb (QTBUG-69716) and on Windows
    // with WA_TranslucentBackground (QTBUG-90628), which this window sets.
    if (FramelessMoveHelper::start(this, event)) {
        return;
    }
    QDialog::mousePressEvent(event);
}

void CanonWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (FramelessMoveHelper::move(this, event)) {
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void CanonWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (FramelessMoveHelper::finish(this, event)) {
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

void CanonWindow::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath shape;
    shape.addRoundedRect(r, kRadius, kRadius);
    p.setClipPath(shape);

    p.fillRect(r, tm.color(this, QStringLiteral("color.canon.ground")));

    auto bloom = [&](const QPointF& centre, qreal radius, const QString& token) {
        QColor c = tm.color(this, token);
        QColor clear = c;
        clear.setAlpha(0);
        QRadialGradient g(centre, radius);
        g.setColorAt(0.0, c);
        g.setColorAt(0.6, clear);
        p.fillRect(r, g);
    };
    bloom(QPointF(r.width() * 0.72, -r.height() * 0.08), r.width() * 1.1,
          QStringLiteral("color.canon.bloom.blue"));
    bloom(QPointF(r.width() * 0.12, r.height() * 0.04), r.width() * 0.9,
          QStringLiteral("color.canon.bloom.teal"));

    // Grid: full strength at the top, gone three quarters of the way down.
    const QColor grid = tm.color(this, QStringLiteral("color.canon.grid"));
    const qreal fadeTo = r.height() * 0.75;
    for (int y = kGridStep; y < fadeTo; y += kGridStep) {
        QColor c = grid;
        c.setAlphaF(grid.alphaF() * (1.0 - y / fadeTo));
        p.setPen(QPen(c, 1));
        p.drawLine(QPointF(0, y + 0.5), QPointF(r.width(), y + 0.5));
    }
    QColor clearGrid = grid;
    clearGrid.setAlpha(0);
    QLinearGradient fade(0, 0, 0, fadeTo);
    fade.setColorAt(0.0, grid);
    fade.setColorAt(1.0, clearGrid);
    p.setPen(QPen(QBrush(fade), 1));
    for (int x = kGridStep; x < r.width(); x += kGridStep) {
        p.drawLine(QPointF(x + 0.5, 0), QPointF(x + 0.5, fadeTo));
    }

    p.setClipping(false);
    p.setPen(QPen(tm.color(this, QStringLiteral("color.canon.lineHi")), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
}

SparkRing::SparkRing(const QPixmap& image, int diameter, QWidget* parent)
    : QWidget(parent), m_image(image), m_diameter(diameter)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_timer = new QTimer(this);
    m_timer->setInterval(kSparkFrameMs);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!sparkVisible(this)) {
            return;
        }
        m_angle -= 360.0 * kSparkFrameMs / kSparkLapMs;   // clockwise
        if (m_angle < 0.0) {
            m_angle += 360.0;
        }
        update();
    });
}

QSize SparkRing::sizeHint() const
{
    return QSize(m_diameter + 20, m_diameter + 20);
}

void SparkRing::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_timer->start();
}

void SparkRing::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void SparkRing::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QPointF c = QRectF(rect()).center();
    const qreal radius = m_diameter / 2.0;
    const QRectF imageRect(c.x() - radius, c.y() - radius, m_diameter, m_diameter);

    QPainterPath disc;
    disc.addEllipse(imageRect);
    p.save();
    p.setClipPath(disc);
    p.drawPixmap(imageRect.toRect(), m_image);
    p.restore();

    // Contrast ring just outside the image, so the dark mark separates from
    // the dark ground.
    const qreal ringR = radius + 4.0;
    const QRectF ring(c.x() - ringR, c.y() - ringR, ringR * 2, ringR * 2);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(tm.color(this, QStringLiteral("color.canon.lineHi")), 1.5));
    p.drawEllipse(ring);

    // The spark: a conical gradient whose bright end trails into nothing,
    // rotated each frame, stroked on the ring; a wider faint pass is the glow.
    const QColor cyan = tm.color(this, QStringLiteral("color.canon.cyan"));
    const QColor hot = tm.color(this, QStringLiteral("color.canon.sparkHot"));
    auto spark = [&](qreal width, qreal alpha) {
        QConicalGradient g(c, m_angle);
        QColor clear = cyan;
        clear.setAlpha(0);
        QColor faint = cyan;
        faint.setAlphaF(0.25 * alpha);
        QColor body = cyan;
        body.setAlphaF(alpha);
        QColor tip = hot;
        tip.setAlphaF(alpha);
        g.setColorAt(0.0, tip);
        g.setColorAt(0.04, body);
        g.setColorAt(0.16, faint);
        g.setColorAt(0.30, clear);
        g.setColorAt(1.0, clear);
        p.setPen(QPen(QBrush(g), width, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(ring);
    };
    spark(6.0, 0.18);
    spark(2.0, 1.0);
}

namespace {
constexpr int kSparkGap = 3;   // spark border sits this far outside the child
constexpr int kSparkBorderLapMs = 3000;   // a button's spark laps faster than the logo's
} // namespace

SparkBorder::SparkBorder(QWidget* child, int radius, QWidget* parent)
    : QWidget(parent), m_child(child), m_radius(radius)
{
    setAttribute(Qt::WA_TranslucentBackground);
    auto* lay = new QVBoxLayout(this);
    // Room outside the outline for half of the 5 px glow pen.
    lay->setContentsMargins(kSparkGap + 3, kSparkGap + 3, kSparkGap + 3, kSparkGap + 3);
    lay->addWidget(child);
    m_timer = new QTimer(this);
    m_timer->setInterval(kSparkFrameMs);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!sparkVisible(this)) {
            return;
        }
        m_angle -= 360.0 * kSparkFrameMs / kSparkBorderLapMs;   // clockwise
        if (m_angle < 0.0) {
            m_angle += 360.0;
        }
        update();
    });
}

void SparkBorder::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_timer->start();
}

void SparkBorder::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void SparkBorder::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(m_child->geometry()).adjusted(-kSparkGap, -kSparkGap, kSparkGap, kSparkGap);
    const qreal r = m_radius + kSparkGap;
    QPainterPath outline;
    outline.addRoundedRect(box, r, r);

    const QColor gold = tm.color(this, QStringLiteral("color.canon.sparkGold"));
    const QColor hot = tm.color(this, QStringLiteral("color.canon.sparkGoldHot"));
    auto spark = [&](qreal width, qreal alpha) {
        QConicalGradient g(box.center(), m_angle);
        QColor clear = gold;
        clear.setAlpha(0);
        QColor faint = gold;
        faint.setAlphaF(0.25 * alpha);
        QColor body = gold;
        body.setAlphaF(alpha);
        QColor tip = hot;
        tip.setAlphaF(alpha);
        g.setColorAt(0.0, tip);
        g.setColorAt(0.04, body);
        g.setColorAt(0.16, faint);
        g.setColorAt(0.30, clear);
        g.setColorAt(1.0, clear);
        p.setPen(QPen(QBrush(g), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(outline);
    };
    spark(5.0, 0.22);
    spark(1.5, 1.0);
}

} // namespace AetherSDR
