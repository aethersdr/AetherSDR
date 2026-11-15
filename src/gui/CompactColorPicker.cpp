#include "CompactColorPicker.h"

#include <QApplication>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

QString colorToTokenHex(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb)
                            : c.name(QColor::HexArgb);
}

// Render a small eyedropper glyph for the picker button.  Drawn at
// device-pixel resolution so HiDPI displays don't blur it.
QIcon makeEyedropperIcon(const QColor& strokeColor)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int side = 16;
    QPixmap pm(QSize(side, side) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(strokeColor);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(strokeColor);

    // Bulb at the top-right (the squeezable end of the pipette).
    p.drawEllipse(QPointF(11.5, 4.5), 2.4, 2.4);
    // Stem — diagonal pipe from bulb to tip.
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(10.2, 5.6), QPointF(3.4, 12.4));
    // Drip at the tip.
    p.setBrush(strokeColor);
    p.drawEllipse(QPointF(3.0, 13.0), 1.1, 1.1);
    return QIcon(pm);
}

// Screen colour picking is implemented via grabMouse(Qt::CrossCursor) +
// grabKeyboard() on the picker widget — exactly the same approach
// QColorDialog uses internally for its "Pick Screen Color" button.
// The platform integration handles Wayland portal / X11 XGrabPointer /
// Win32 SetCapture under the hood, so this works on every platform
// where QColorDialog's eyedropper works.

// Common 2x2 checkerboard tile painter, used by the alpha slider and
// the live swatch so translucent values don't disappear into the
// surrounding dark theme.
void paintCheckerboard(QPainter& p, const QRect& r)
{
    constexpr int tile = 6;
    for (int y = r.top(); y < r.bottom(); y += tile) {
        for (int x = r.left(); x < r.right(); x += tile) {
            const bool dark = ((x / tile + y / tile) & 1) == 0;
            p.fillRect(QRect(x, y, tile, tile),
                       dark ? QColor(0x80, 0x80, 0x80) : QColor(0xc8, 0xc8, 0xc8));
        }
    }
}

} // namespace

// ───────────────────────────────────────────────────────────── SVSquare ──

SVSquare::SVSquare(QWidget* parent) : QWidget(parent)
{
    setFixedSize(200, 200);
    setMouseTracking(false);
    setCursor(Qt::CrossCursor);
}

void SVSquare::setHue(int h)
{
    h = std::clamp(h, 0, 359);
    if (h == m_h) return;
    m_h = h;
    rebuildCache();
    update();
}

void SVSquare::setSV(int s, int v)
{
    s = std::clamp(s, 0, 255);
    v = std::clamp(v, 0, 255);
    if (s == m_s && v == m_v) return;
    m_s = s;
    m_v = v;
    update();
}

void SVSquare::resizeEvent(QResizeEvent*)
{
    rebuildCache();
}

void SVSquare::rebuildCache()
{
    // Render the SV plane at the current hue into a QImage cache.
    // Two-gradient composite: horizontal white→hue and vertical
    // transparent→black, painted with Multiply / SourceOver semantics
    // via the gradient's alpha component.  Faster than per-pixel setPixel.
    const QSize sz = size().isValid() ? size() : QSize(180, 180);
    if (sz.width() < 2 || sz.height() < 2) return;
    m_cache = QImage(sz, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&m_cache);

    const QColor hueColor = QColor::fromHsv(m_h, 255, 255);
    QLinearGradient horizontal(0, 0, sz.width(), 0);
    horizontal.setColorAt(0.0, QColor(255, 255, 255));
    horizontal.setColorAt(1.0, hueColor);
    p.fillRect(m_cache.rect(), horizontal);

    QLinearGradient vertical(0, 0, 0, sz.height());
    vertical.setColorAt(0.0, QColor(0, 0, 0, 0));
    vertical.setColorAt(1.0, QColor(0, 0, 0, 255));
    p.fillRect(m_cache.rect(), vertical);
}

void SVSquare::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    if (m_cache.isNull()) rebuildCache();
    p.drawImage(0, 0, m_cache);

    // Border so the plane stops cleanly against the dialog background.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Crosshair marker at the current (s, v).  White ring on a black
    // dot so it stays visible against any underlying colour.
    const int x = static_cast<int>(std::round(m_s / 255.0 * (width() - 1)));
    const int y = static_cast<int>(std::round((1.0 - m_v / 255.0) * (height() - 1)));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 200), 2));
    p.drawEllipse(QPoint(x, y), 6, 6);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1));
    p.drawEllipse(QPoint(x, y), 6, 6);
}

void SVSquare::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        pickFromMouse(e->pos());
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void SVSquare::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton) {
        pickFromMouse(e->pos());
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void SVSquare::pickFromMouse(const QPoint& p)
{
    const int w = std::max(1, width()  - 1);
    const int h = std::max(1, height() - 1);
    const int x = std::clamp(p.x(), 0, w);
    const int y = std::clamp(p.y(), 0, h);
    const int s = static_cast<int>(std::round(static_cast<double>(x) / w * 255.0));
    const int v = static_cast<int>(std::round((1.0 - static_cast<double>(y) / h) * 255.0));
    setSV(s, v);
    emit svPicked(s, v);
}

// ───────────────────────────────────────────────────────────── HueStrip ──

HueStrip::HueStrip(QWidget* parent) : QWidget(parent)
{
    setFixedSize(12, 200);
    setCursor(Qt::SizeVerCursor);
}

void HueStrip::setHue(int h)
{
    h = std::clamp(h, 0, 359);
    if (h == m_h) return;
    m_h = h;
    update();
}

void HueStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // Vertical hue ramp — 7 stops from R round the wheel back to R.
    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0.0000, QColor::fromHsv(  0, 255, 255));
    g.setColorAt(0.1667, QColor::fromHsv( 60, 255, 255));
    g.setColorAt(0.3333, QColor::fromHsv(120, 255, 255));
    g.setColorAt(0.5000, QColor::fromHsv(180, 255, 255));
    g.setColorAt(0.6667, QColor::fromHsv(240, 255, 255));
    g.setColorAt(0.8333, QColor::fromHsv(300, 255, 255));
    g.setColorAt(1.0000, QColor::fromHsv(359, 255, 255));
    p.fillRect(rect(), g);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Marker: prominent square thumb at the current hue.  Full strip
    // width × ~9px tall, dark outer ring + white inner fill so it's
    // visible against any hue, plus a 1px central tick showing the
    // exact selected hue line.
    const int y = static_cast<int>(std::round(m_h / 359.0 * (height() - 1)));
    const int half = 4;
    const QRect thumbOuter(0, y - half, width(), half * 2 + 1);
    p.setBrush(QColor(0, 0, 0, 220));
    p.setPen(Qt::NoPen);
    p.drawRect(thumbOuter);
    const QRect thumbInner = thumbOuter.adjusted(1, 1, -1, -1);
    p.setBrush(Qt::white);
    p.drawRect(thumbInner);
    p.setPen(QPen(QColor(0, 0, 0, 220), 1));
    p.drawLine(1, y, width() - 2, y);
}

void HueStrip::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) { pickFromMouse(e->pos()); e->accept(); return; }
    QWidget::mousePressEvent(e);
}

void HueStrip::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton) { pickFromMouse(e->pos()); e->accept(); return; }
    QWidget::mouseMoveEvent(e);
}

void HueStrip::pickFromMouse(const QPoint& p)
{
    const int h = std::max(1, height() - 1);
    const int y = std::clamp(p.y(), 0, h);
    const int hue = static_cast<int>(std::round(static_cast<double>(y) / h * 359.0));
    setHue(hue);
    emit huePicked(hue);
}

// ────────────────────────────────────────────────────────── AlphaSlider ──

AlphaSlider::AlphaSlider(QWidget* parent) : QWidget(parent)
{
    setFixedSize(200, 12);
    setCursor(Qt::SizeHorCursor);
}

void AlphaSlider::setAlpha(int a)
{
    a = std::clamp(a, 0, 255);
    if (a == m_a) return;
    m_a = a;
    update();
}

void AlphaSlider::setBaseColor(const QColor& c)
{
    QColor opaque = c;
    opaque.setAlpha(255);
    if (opaque == m_base) return;
    m_base = opaque;
    update();
}

void AlphaSlider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    paintCheckerboard(p, rect());

    QLinearGradient g(0, 0, width(), 0);
    QColor a0 = m_base; a0.setAlpha(0);
    QColor a1 = m_base; a1.setAlpha(255);
    g.setColorAt(0.0, a0);
    g.setColorAt(1.0, a1);
    p.fillRect(rect(), g);

    // Square border + corners — matches the HueStrip's flat look.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Marker: prominent square thumb at the current alpha position.
    // ~9px wide × full strip height, dark outer ring + white inner
    // fill so it stays visible over both the checkerboard and the
    // color overlay.
    const int x = static_cast<int>(std::round(m_a / 255.0 * (width() - 1)));
    const int half = 4;
    const QRect thumbOuter(x - half, 0, half * 2 + 1, height());
    p.setBrush(QColor(0, 0, 0, 220));
    p.setPen(Qt::NoPen);
    p.drawRect(thumbOuter);
    const QRect thumbInner = thumbOuter.adjusted(1, 1, -1, -1);
    p.setBrush(Qt::white);
    p.drawRect(thumbInner);
    p.setPen(QPen(QColor(0, 0, 0, 220), 1));
    p.drawLine(x, 1, x, height() - 2);
}

void AlphaSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) { pickFromMouse(e->pos()); e->accept(); return; }
    QWidget::mousePressEvent(e);
}

void AlphaSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton) { pickFromMouse(e->pos()); e->accept(); return; }
    QWidget::mouseMoveEvent(e);
}

void AlphaSlider::pickFromMouse(const QPoint& p)
{
    const int w = std::max(1, width() - 1);
    const int x = std::clamp(p.x(), 0, w);
    const int a = static_cast<int>(std::round(static_cast<double>(x) / w * 255.0));
    setAlpha(a);
    emit alphaPicked(a);
}

// ────────────────────────────────────────────────────── CompactColorPicker ─

CompactColorPicker::CompactColorPicker(QWidget* parent) : QWidget(parent)
{
    // Size policy + layout sizing both pinned — the picker is a
    // compact fixed-content block, and we don't want the parent
    // QStackedWidget page to stretch it vertically by spreading the
    // child rows apart.  SetFixedSize makes the layout report a
    // single canonical sizeHint; Fixed vertical policy stops the
    // parent from overriding it.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);
    root->setSizeConstraint(QLayout::SetMinimumSize);

    // SV / Hue / Alpha live in their own sub-VBox with explicit 4 px
    // spacing so the surrounding root spacing (6 px) doesn't bleed in
    // around them — Jeremy wants the visual gap between SV ↔ Hue and
    // SV ↔ Alpha to be exactly 4 px while preserving the wider gaps
    // between the alpha bar and the RGB / Hex rows below.
    auto* svBlock = new QVBoxLayout;
    svBlock->setSpacing(4);
    svBlock->setContentsMargins(0, 0, 0, 0);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(4);
    m_sv  = new SVSquare(this);
    m_hue = new HueStrip(this);
    topRow->addWidget(m_sv);
    topRow->addWidget(m_hue);
    topRow->addStretch(1);
    svBlock->addLayout(topRow);

    auto* alphaRow = new QHBoxLayout;
    alphaRow->setSpacing(0);
    m_alpha = new AlphaSlider(this);
    alphaRow->addWidget(m_alpha);
    alphaRow->addStretch(1);
    svBlock->addLayout(alphaRow);

    root->addLayout(svBlock);

    // Row 3: R / G / B spin boxes — alpha is handled by the bar above,
    // so this row stays compact.  Padding inside each spin box is
    // trimmed via stylesheet and the fixed width tightened so all
    // three pairs fit inside the 218 px width of the SV+hue block
    // above (200 + 6 + 12 = 218).
    auto* rgbRow = new QHBoxLayout;
    rgbRow->setSpacing(2);
    auto makeRgb = [&](const QString& label, QSpinBox*& slot) {
        rgbRow->addWidget(new QLabel(label, this));
        slot = new QSpinBox(this);
        slot->setRange(0, 255);
        slot->setFixedWidth(58);
        slot->setStyleSheet(QStringLiteral(
            "QSpinBox { padding: 1px 2px 1px 3px; }"));
        rgbRow->addWidget(slot);
    };
    makeRgb(QStringLiteral("R"), m_r);
    makeRgb(QStringLiteral("G"), m_g);
    makeRgb(QStringLiteral("B"), m_b);
    rgbRow->addStretch(1);
    root->addLayout(rgbRow);

    // Row 4: live swatch + hex line edit.
    auto* hexRow = new QHBoxLayout;
    hexRow->setSpacing(6);
    m_swatch = new QLabel(this);
    m_swatch->setFixedSize(28, 24);
    hexRow->addWidget(m_swatch);
    hexRow->addWidget(new QLabel(QStringLiteral("Hex"), this));
    m_hex = new QLineEdit(this);
    m_hex->setPlaceholderText(QStringLiteral("#rrggbb"));
    m_hex->setMaxLength(9);
    m_hex->setFixedWidth(96);
    hexRow->addWidget(m_hex);

    m_eyedropper = new QPushButton(this);
    m_eyedropper->setIcon(makeEyedropperIcon(palette().color(QPalette::WindowText)));
    m_eyedropper->setIconSize(QSize(14, 14));
    m_eyedropper->setFixedSize(24, 24);
    m_eyedropper->setToolTip(QStringLiteral(
        "Pick a colour from anywhere on the screen.\n"
        "Click to sample.  Esc or right-click cancels."));
    m_eyedropper->setAccessibleName(QStringLiteral("Pick colour from screen"));
    hexRow->addWidget(m_eyedropper);

    hexRow->addStretch(1);
    root->addLayout(hexRow);

    rebuildFromColor();

    connect(m_sv,    &SVSquare::svPicked,     this, &CompactColorPicker::onSVPicked);
    connect(m_hue,   &HueStrip::huePicked,    this, &CompactColorPicker::onHuePicked);
    connect(m_alpha, &AlphaSlider::alphaPicked, this, &CompactColorPicker::onAlphaPicked);
    connect(m_hex,   &QLineEdit::editingFinished,
            this, &CompactColorPicker::onHexEdited);
    connect(m_eyedropper, &QPushButton::clicked,
            this, &CompactColorPicker::onEyedropperClicked);
    for (QSpinBox* sb : {m_r, m_g, m_b}) {
        connect(sb, qOverload<int>(&QSpinBox::valueChanged),
                this, &CompactColorPicker::onRgbEdited);
    }
}

void CompactColorPicker::setColor(const QColor& c)
{
    if (!c.isValid()) return;
    if (c == m_color) return;
    m_color = c;
    m_updating = true;
    rebuildFromColor();
    m_updating = false;
}

void CompactColorPicker::rebuildFromColor()
{
    int h, s, v, a;
    m_color.getHsv(&h, &s, &v, &a);
    if (h < 0) h = m_hue->hue();  // achromatic colours report hue=-1
    {
        QSignalBlocker bH(m_hue), bS(m_sv), bA(m_alpha);
        QSignalBlocker bR(m_r), bG(m_g), bB(m_b), bHex(m_hex);
        m_hue->setHue(h);
        m_sv->setHue(h);
        m_sv->setSV(s, v);
        m_alpha->setAlpha(a);
        m_alpha->setBaseColor(m_color);
        m_r->setValue(m_color.red());
        m_g->setValue(m_color.green());
        m_b->setValue(m_color.blue());
        m_hex->setText(colorToTokenHex(m_color));
    }
    // Live swatch — small rounded rect over the standard checkerboard.
    QPixmap pm(m_swatch->size());
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0.5, 0.5, pm.width() - 1.0, pm.height() - 1.0), 3, 3);
    p.save();
    p.setClipPath(clip);
    paintCheckerboard(p, pm.rect());
    p.fillRect(pm.rect(), m_color);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, pm.width() - 1.0, pm.height() - 1.0), 3, 3);
    m_swatch->setPixmap(pm);
}

void CompactColorPicker::emitChange()
{
    rebuildFromColor();
    emit colorChanged(m_color);
}

void CompactColorPicker::onSVPicked(int s, int v)
{
    if (m_updating) return;
    const int h = m_hue->hue();
    QColor c = QColor::fromHsv(h, s, v);
    c.setAlpha(m_alpha->alpha());
    m_color = c;
    emitChange();
}

void CompactColorPicker::onHuePicked(int h)
{
    if (m_updating) return;
    m_sv->setHue(h);
    QColor c = QColor::fromHsv(h, m_sv->saturation(), m_sv->value());
    c.setAlpha(m_alpha->alpha());
    m_color = c;
    emitChange();
}

void CompactColorPicker::onAlphaPicked(int a)
{
    if (m_updating) return;
    m_color.setAlpha(a);
    emitChange();
}

void CompactColorPicker::onHexEdited()
{
    if (m_updating) return;
    QColor c(m_hex->text().trimmed());
    if (!c.isValid()) {
        // Snap back to current — bad input ignored.
        QSignalBlocker bHex(m_hex);
        m_hex->setText(colorToTokenHex(m_color));
        return;
    }
    m_color = c;
    emitChange();
}

void CompactColorPicker::onRgbEdited()
{
    if (m_updating) return;
    m_color = QColor(m_r->value(), m_g->value(), m_b->value(), m_alpha->alpha());
    emitChange();
}

void CompactColorPicker::onEyedropperClicked()
{
    if (m_pickingScreen) return;
    m_pickingScreen = true;
    // Same pair QColorDialog::pickScreenColor() uses internally — grabMouse
    // routes every mouse event to us regardless of which app the cursor is
    // over; grabKeyboard catches Esc-to-cancel before any other widget
    // sees it.  Qt's platform integration handles the Wayland-portal /
    // X11-XGrabPointer / Win32-SetCapture mechanics under the hood.
    grabMouse(QCursor(Qt::CrossCursor));
    grabKeyboard();
}

bool CompactColorPicker::event(QEvent* ev)
{
    if (!m_pickingScreen) return QWidget::event(ev);

    switch (ev->type()) {
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(ev);
        // Release the grabs FIRST so QScreen::grabWindow sees a clean
        // composited image (no leftover crosshair cursor in the capture).
        releaseMouse();
        releaseKeyboard();
        m_pickingScreen = false;

        if (me->button() == Qt::LeftButton) {
            const QPoint gp = me->globalPosition().toPoint();
            if (auto* screen = QGuiApplication::screenAt(gp)) {
                const QPoint local = gp - screen->geometry().topLeft();
                const QPixmap pm = screen->grabWindow(
                    0, local.x(), local.y(), 1, 1);
                if (!pm.isNull()) {
                    const QColor sampled = pm.toImage().pixelColor(0, 0);
                    if (sampled.isValid()) {
                        // Preserve the current alpha — the eyedropper
                        // reads only RGB from the screen; the alpha
                        // slider remains the source of truth.
                        QColor next = sampled;
                        next.setAlpha(m_color.alpha());
                        setColor(next);
                        emitChange();
                    }
                }
            }
        }
        ev->accept();
        return true;
    }
    case QEvent::MouseButtonPress: {
        // Eat the press so it doesn't reach the underlying widget;
        // the release handler does the actual sampling.
        ev->accept();
        return true;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) {
            releaseMouse();
            releaseKeyboard();
            m_pickingScreen = false;
            ev->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::event(ev);
}

} // namespace AetherSDR
