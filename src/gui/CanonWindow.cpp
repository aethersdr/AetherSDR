#include "CanonWindow.h"

#include "FramelessMoveHelper.h"

#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QConicalGradient>
#include <QAccessibilityHints>
#include <QGuiApplication>
#include <QFrame>
#include <QLabel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QScreen>
#include <QShortcut>
#include <QStyleHints>
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
constexpr int kSparkIdleMs = 500;   // while the window is not exposed

const QAccessibilityHints* accessibilityHints()
{
    return QGuiApplication::styleHints()->accessibility();
}
} // namespace

CanonWindow::CanonWindow(const QString& title, QWidget* parent, Kind kind)
    : QDialog(parent)
    , m_returnClicksDefault(kind == Kind::Dialog)
{
    setWindowTitle(title);
    setWindowFlags((kind == Kind::Workspace ? Qt::Window : Qt::Dialog)
                   | Qt::FramelessWindowHint);
    // A secondary top-level window: an open workspace must not keep the app
    // running once the main window has closed.
    if (kind == Kind::Workspace) {
        setAttribute(Qt::WA_QuitOnClose, false);
    }
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

    // The ground is painted from tokens; rebuild it when the theme changes.
    connect(&tm, &ThemeManager::themeChanged, this, [this] {
        m_ground = QPixmap();
        update();
    });
}

void CanonWindow::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    m_ground = QPixmap();
    m_close->move(width() - kCloseSize - kCloseMargin, kCloseMargin);
    m_close->raise();
    // Native-window creation on first show() delivers move and resize events
    // before showEvent(); saving those would overwrite the last session's
    // geometry with the default one, so nothing is saved until placed.
    if (m_placed && !m_restoringGeometry) {
        saveGeometryToSettings();
    }
}

void CanonWindow::moveEvent(QMoveEvent* event)
{
    QDialog::moveEvent(event);
    if (m_placed && !m_restoringGeometry) {
        saveGeometryToSettings();
    }
}

void CanonWindow::keyPressEvent(QKeyEvent* event)
{
    // QDialog::keyPressEvent() turns Return and Enter into a click on the
    // default or first auto-default button. With setReturnClicksDefault(false)
    // that is skipped; a focused button still takes Return itself, so keyboard
    // activation is unchanged.
    // Same test QDialog uses: no modifier, or the keypad's Enter.
    const bool plainEnter = !event->modifiers()
        || (event->modifiers() & Qt::KeypadModifier && event->key() == Qt::Key_Enter);
    if (!m_returnClicksDefault && plainEnter
        && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        event->ignore();
        return;
    }
    QDialog::keyPressEvent(event);
}

void CanonWindow::hideEvent(QHideEvent* event)
{
    // Move and resize saves are in memory; hiding flushes them to disk. Hide,
    // not close: Escape and Ctrl+W go through QDialog::reject(), which hides
    // the window without a close event.
    if (!m_geometryKey.isEmpty() && m_placed) {
        saveGeometryToSettings();
        AppSettings::instance().save();
    }
    QDialog::hideEvent(event);
}

// Saved as "x,y,width,height" and applied with move() and resize():
// QWidget::restoreGeometry() reports success on Wayland without applying the
// size, so a window resized there came back at its constructed size. A value
// in the older saveGeometry() form (from before Network Diagnostics was a
// CanonWindow) is read once and replaced on the next save.
void CanonWindow::saveGeometryToSettings()
{
    if (m_geometryKey.isEmpty()) {
        return;
    }
    const QRect g = geometry();
    AppSettings::instance().setValue(m_geometryKey, QStringLiteral("%1,%2,%3,%4")
        .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()));
}

CanonWindow::Restored CanonWindow::restoreGeometryFromSettings()
{
    const QString saved = AppSettings::instance().value(m_geometryKey).toString();
    if (saved.isEmpty()) {
        return Restored::Nothing;
    }
    const QStringList parts = saved.split(QLatin1Char(','));
    if (parts.size() == 4) {
        bool ok[4]{};
        const int x = parts[0].toInt(&ok[0]);
        const int y = parts[1].toInt(&ok[1]);
        const int w = parts[2].toInt(&ok[2]);
        const int h = parts[3].toInt(&ok[3]);
        if (!(ok[0] && ok[1] && ok[2] && ok[3]) || w <= 0 || h <= 0) {
            return Restored::Nothing;
        }
        resize(QSize(w, h).expandedTo(minimumSize()));
        // A position on a screen that's gone (an unplugged monitor) is not
        // restored: the window keeps its size and centres instead. Wayland
        // places top-level windows itself and ignores the move either way.
        if (const QScreen* s = QGuiApplication::screenAt(QPoint(x, y) + QPoint(w / 2, h / 2))) {
            if (s->availableGeometry().intersects(QRect(x, y, w, h))) {
                move(x, y);
                return Restored::SizeAndPosition;
            }
        }
        return Restored::SizeOnly;
    }
    return restoreGeometry(QByteArray::fromBase64(saved.toLatin1()))
        ? Restored::SizeAndPosition : Restored::Nothing;
}

void CanonWindow::setLaunchSize(const QSize& size)
{
    m_launchSize = size;
    resize(size);
}

void CanonWindow::showEvent(QShowEvent* event)
{
    // A saved geometry is applied before QDialog::showEvent(), as
    // PersistentDialog does, so the window maps at its saved size.
    Restored restored = Restored::Nothing;
    if (!m_placed && !m_geometryKey.isEmpty()) {
        m_restoringGeometry = true;
        restored = restoreGeometryFromSettings();
        m_restoringGeometry = false;
    }
    // The launch size wins over the saved one on every open, and is applied
    // before centring so the centre is the pinned window's.
    if (m_launchSize.isValid() && size() != m_launchSize) {
        resize(m_launchSize.expandedTo(minimumSize()));
    }
    if (m_placed || restored == Restored::SizeAndPosition) {
        m_placed = true;
        QDialog::showEvent(event);
        return;
    }
    QDialog::showEvent(event);
    m_placed = true;
    if (m_ownerPlaced) {
        return;
    }
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
    // The ground (fill, blooms, grid, border) only changes with size, DPR or
    // theme, but the sparks repaint the window under them 30 times a second:
    // paint it once into a pixmap and blit that.
    const qreal dpr = devicePixelRatioF();
    if (m_ground.isNull() || m_ground.size() != size() * dpr
        || !qFuzzyCompare(m_ground.devicePixelRatio(), dpr)) {
        m_ground = QPixmap(size() * dpr);
        m_ground.setDevicePixelRatio(dpr);
        m_ground.fill(Qt::transparent);
        paintGround(m_ground);
    }
    QPainter(this).drawPixmap(0, 0, m_ground);
}

void CanonWindow::paintGround(QPaintDevice& device) const
{
    auto& tm = ThemeManager::instance();
    QPainter p(&device);
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

SparkWidget::SparkWidget(int lapMs, QWidget* parent)
    : QWidget(parent), m_lapMs(lapMs)
{
    setAttribute(Qt::WA_TranslucentBackground);
    m_timer = new QTimer(this);
    m_timer->setInterval(kSparkFrameMs);
    connect(m_timer, &QTimer::timeout, this, &SparkWidget::tick);
    setMotionPreference(accessibilityHints()->motionPreference());
    connect(accessibilityHints(), &QAccessibilityHints::motionPreferenceChanged,
            this, &SparkWidget::setMotionPreference);
}

void SparkWidget::tick()
{
    // A frame is wasted on a window nobody can see, and Qt sends no hideEvent
    // when a window is minimised or covered; it only stops being exposed.
    // Tick slowly until it is exposed again.
    const QWindow* handle = window()->windowHandle();
    if (!handle || !handle->isExposed()) {
        m_timer->setInterval(kSparkIdleMs);
        return;
    }
    m_timer->setInterval(kSparkFrameMs);
    m_angle -= 360.0 * kSparkFrameMs / m_lapMs;   // clockwise
    if (m_angle < 0.0) {
        m_angle += 360.0;
    }
    update();
}

void SparkWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_motionReduced) {
        m_timer->start(kSparkFrameMs);
    }
}

void SparkWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void SparkWidget::setMotionPreference(Qt::MotionPreference preference)
{
    m_motionReduced = preference == Qt::MotionPreference::ReducedMotion;
    if (m_motionReduced) {
        m_timer->stop();
    } else if (isVisible()) {
        m_timer->start(kSparkFrameMs);
    }
    update();
}

bool SparkWidget::isAnimating() const
{
    return m_timer->isActive();
}

void SparkWidget::paintSpark(QPainter& p, const QPainterPath& path, const QPointF& centre,
                             const QColor& body, const QColor& tip,
                             qreal glowWidth, qreal glowAlpha, qreal coreWidth) const
{
    // A conical gradient whose bright end trails into nothing, rotated each
    // frame and stroked on the path; the wider faint pass is the glow.
    auto stroke = [&](qreal width, qreal alpha) {
        QConicalGradient g(centre, m_angle);
        QColor clear = body;
        clear.setAlpha(0);
        QColor faint = body;
        faint.setAlphaF(0.25 * alpha);
        QColor main = body;
        main.setAlphaF(alpha);
        QColor hot = tip;
        hot.setAlphaF(alpha);
        g.setColorAt(0.0, hot);
        g.setColorAt(0.04, main);
        g.setColorAt(0.16, faint);
        g.setColorAt(0.30, clear);
        g.setColorAt(1.0, clear);
        p.setPen(QPen(QBrush(g), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    stroke(glowWidth, glowAlpha);
    stroke(coreWidth, 1.0);
}

SparkRing::SparkRing(const QPixmap& image, int diameter, QWidget* parent)
    : SparkWidget(kSparkLapMs, parent), m_source(image), m_diameter(diameter)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QSize SparkRing::sizeHint() const
{
    return QSize(m_diameter + 20, m_diameter + 20);
}

void SparkRing::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();

    // Scale for the screen this frame is painted on, once per DPR.
    const qreal dpr = devicePixelRatioF();
    if (m_scaled.isNull() || !qFuzzyCompare(m_scaledDpr, dpr)) {
        m_scaled = m_source.scaled(QSize(m_diameter, m_diameter) * dpr,
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_scaled.setDevicePixelRatio(dpr);
        m_scaledDpr = dpr;
    }

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
    p.drawPixmap(imageRect.toRect(), m_scaled);   // whole pixels: no resampling blur
    p.restore();

    // Contrast ring just outside the image, so the dark mark separates from
    // the dark ground.
    const qreal ringR = radius + 4.0;
    QPainterPath ring;
    ring.addEllipse(QRectF(c.x() - ringR, c.y() - ringR, ringR * 2, ringR * 2));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(tm.color(this, QStringLiteral("color.canon.lineHi")), 1.5));
    p.drawPath(ring);

    paintSpark(p, ring, c, tm.color(this, QStringLiteral("color.canon.cyan")),
               tm.color(this, QStringLiteral("color.canon.sparkHot")), 6.0, 0.18, 2.0);
}

namespace {
constexpr int kSparkGap = 3;   // spark border sits this far outside the child
constexpr int kSparkBorderLapMs = 3000;   // a button's spark laps faster than the logo's
} // namespace

SparkBorder::SparkBorder(QWidget* child, int radius, QWidget* parent)
    : SparkWidget(kSparkBorderLapMs, parent), m_child(child), m_radius(radius)
{
    auto* lay = new QVBoxLayout(this);
    // Room outside the outline for half of the 5 px glow pen.
    lay->setContentsMargins(kSparkGap + 3, kSparkGap + 3, kSparkGap + 3, kSparkGap + 3);
    lay->addWidget(child);
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

    paintSpark(p, outline, box.center(), tm.color(this, QStringLiteral("color.canon.sparkGold")),
               tm.color(this, QStringLiteral("color.canon.sparkGoldHot")), 5.0, 0.22, 1.5);
}

QWidget* makeCanonHeader(const QString& title)
{
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("canonHeader"));
    auto* col = new QVBoxLayout(header);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(8);

    auto* label = new QLabel(title, header);
    label->setObjectName(QStringLiteral("canonHeaderTitle"));
    label->setAccessibleName(title);
    label->setContentsMargins(0, 0, 40, 0);
    col->addWidget(label);

    auto* rule = new QFrame(header);
    rule->setObjectName(QStringLiteral("canonHeaderRule"));
    col->addWidget(rule);

    AetherSDR::ThemeManager::instance().applyStyleSheet(header,
        "QWidget#canonHeader { background: transparent; }"
        "QLabel#canonHeaderTitle { background: transparent; color: {{color.canon.ink}};"
        " font-size: 17px; font-weight: 700; }"
        "QFrame#canonHeaderRule { background: {{color.canon.line}}; border: none;"
        " min-height: 1px; max-height: 1px; }");
    return header;
}

} // namespace AetherSDR
