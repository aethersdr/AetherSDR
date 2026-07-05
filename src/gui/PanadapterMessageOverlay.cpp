#include "PanadapterMessageOverlay.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QEvent>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {

constexpr int kCardMinWidth = 360;
constexpr int kCardMaxTextWidth = 620;
constexpr int kCardPaddingX = 16;
constexpr int kCardPaddingTop = 10;
constexpr int kCardPaddingBottom = 12;
constexpr int kCardSpacing = 8;
constexpr int kStackMargin = 12;
constexpr int kIconColumnWidth = 56;
constexpr int kAccentRailWidth = 4;
constexpr int kCloseSize = 24;
constexpr int kCloseInset = 8;
constexpr int kCountdownMinWidth = 34;
constexpr int kCountdownHeight = 18;
constexpr int kCountdownPaddingX = 8;
constexpr int kCountdownGap = 8;
constexpr qreal kAnimationStep = 0.24;
constexpr qreal kDoneDistance = 0.5;
constexpr qreal kDoneOpacity = 0.02;

QString buttonObjectNameForId(QString id)
{
    for (int i = 0; i < id.size(); ++i) {
        if (!id.at(i).isLetterOrNumber()) {
            id[i] = QLatin1Char('_');
        }
    }
    return QStringLiteral("panOverlayMessageClose_%1").arg(id);
}

QRectF easedRect(const QRectF& from, const QRectF& to)
{
    return QRectF(from.left() + (to.left() - from.left()) * kAnimationStep,
                  from.top() + (to.top() - from.top()) * kAnimationStep,
                  from.width() + (to.width() - from.width()) * kAnimationStep,
                  from.height() + (to.height() - from.height()) * kAnimationStep);
}

bool rectCloseEnough(const QRectF& a, const QRectF& b)
{
    return std::abs(a.left() - b.left()) < kDoneDistance
        && std::abs(a.top() - b.top()) < kDoneDistance
        && std::abs(a.width() - b.width()) < kDoneDistance
        && std::abs(a.height() - b.height()) < kDoneDistance;
}

class OverlayCloseButton final : public QToolButton {
public:
    explicit OverlayCloseButton(QWidget* parent = nullptr)
        : QToolButton(parent)
    {
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF ring = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
        const bool pressed = isDown();
        const bool hovered = underMouse();

        if (hasFocus()) {
            p.setPen(QPen(AetherSDR::theme::withAlpha("color.accent.bright", 210), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(ring.adjusted(-1.5, -1.5, 1.5, 1.5));
        }

        QColor fill(2, 7, 14, 232);
        if (hovered) {
            fill = QColor(18, 31, 46, 246);
        }
        if (pressed) {
            fill = AetherSDR::theme::withAlpha(QStringLiteral("color.accent.bright"), 238);
        }

        p.setBrush(fill);
        p.setPen(QPen(QColor(255, 255, 255, pressed ? 250 : 230), 2.0));
        p.drawEllipse(ring);

        const QColor xColor = pressed
            ? QColor(1, 9, 16, 245)
            : QColor(255, 255, 255, 248);
        QPen xPen(xColor, 2.4, Qt::SolidLine, Qt::RoundCap);
        p.setPen(xPen);
        const qreal inset = 7.5;
        p.drawLine(QPointF(inset, inset),
                   QPointF(width() - inset, height() - inset));
        p.drawLine(QPointF(width() - inset, inset),
                   QPointF(inset, height() - inset));
    }
};

QString toneName(PanadapterOverlayMessageTone tone)
{
    switch (tone) {
    case PanadapterOverlayMessageTone::Warning:
        return QStringLiteral("warning");
    case PanadapterOverlayMessageTone::Info:
        return QStringLiteral("info");
    }
    return QStringLiteral("info");
}

QColor accentForTone(PanadapterOverlayMessageTone tone, int alpha = 245)
{
    switch (tone) {
    case PanadapterOverlayMessageTone::Warning:
        return AetherSDR::theme::withAlpha(QStringLiteral("color.accent.danger"), alpha);
    case PanadapterOverlayMessageTone::Info:
        return AetherSDR::theme::withAlpha(QStringLiteral("color.accent.bright"), alpha);
    }
    return AetherSDR::theme::withAlpha(QStringLiteral("color.accent.bright"), alpha);
}

QString countdownText(qint64 expiresAtMs, bool removing, qint64 nowMs)
{
    if (removing || expiresAtMs <= 0) {
        return {};
    }
    const qint64 remainingMs = qMax<qint64>(0, expiresAtMs - nowMs);
    const qint64 seconds = qMax<qint64>(0, (remainingMs + 999) / 1000);
    return QStringLiteral("%1s").arg(seconds);
}

int countdownWidth(const QString& text, const QFontMetrics& fm)
{
    if (text.isEmpty()) {
        return 0;
    }
    return qMax(kCountdownMinWidth,
                fm.horizontalAdvance(text) + kCountdownPaddingX * 2);
}

void drawToneIcon(QPainter& p,
                  const QRectF& rect,
                  PanadapterOverlayMessageTone tone,
                  const QColor& accent)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(accent, 2));
    p.setBrush(Qt::NoBrush);

    if (tone == PanadapterOverlayMessageTone::Warning) {
        const QPointF top(rect.center().x(), rect.top() + 6.0);
        const QPointF left(rect.left() + 7.0, rect.bottom() - 7.0);
        const QPointF right(rect.right() - 7.0, rect.bottom() - 7.0);
        QPolygonF tri;
        tri << top << right << left;
        p.drawPolygon(tri);

        QPen markPen(accent, 2.4, Qt::SolidLine, Qt::RoundCap);
        p.setPen(markPen);
        p.drawLine(QPointF(rect.center().x(), rect.top() + 17.0),
                   QPointF(rect.center().x(), rect.bottom() - 15.0));
        p.setBrush(accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(rect.center().x(), rect.bottom() - 10.0), 2.0, 2.0);
    } else {
        p.drawEllipse(rect.adjusted(6.0, 6.0, -6.0, -6.0));

        QFont iconFont = p.font();
        iconFont.setPointSize(24);
        iconFont.setBold(true);
        p.setFont(iconFont);
        p.setPen(accent);
        p.drawText(rect.adjusted(0.0, -1.0, 0.0, 0.0),
                   Qt::AlignCenter,
                   QStringLiteral("i"));
    }

    p.restore();
}

} // namespace

PanadapterMessageOverlay::PanadapterMessageOverlay(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("panadapterMessageOverlay"));
    setAccessibleName(tr("Panadapter message overlay"));
    setAutoFillBackground(false);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    hide();

    m_clock.start();

    m_animationTimer.setInterval(16);
    connect(&m_animationTimer, &QTimer::timeout,
            this, &PanadapterMessageOverlay::tickAnimation);

    m_expiryTimer.setSingleShot(true);
    connect(&m_expiryTimer, &QTimer::timeout,
            this, &PanadapterMessageOverlay::expireDueMessages);

    m_countdownTimer.setInterval(250);
    connect(&m_countdownTimer, &QTimer::timeout, this, [this]() {
        updateAccessibilityDescription(false);
        update();
    });
}

bool PanadapterMessageOverlay::upsertMessage(PanadapterOverlayMessage message)
{
    message.id = normalizedId(message.id);
    message.title = message.title.trimmed();
    message.detail = message.detail.trimmed();
    if (message.id.isEmpty()
        || (message.title.isEmpty() && message.detail.isEmpty())) {
        return false;
    }

    const qint64 expiry = message.timeoutMs > 0
        ? m_clock.elapsed() + qMax(1, message.timeoutMs)
        : -1;

    const int existing = indexOf(message.id);
    if (existing >= 0) {
        Item& item = m_items[existing];
        const bool wasRemoving = item.removing;
        const bool contentChanged =
            item.message.title != message.title
            || item.message.detail != message.detail
            || item.message.dismissible != message.dismissible
            || item.message.tone != message.tone;
        // Identical, untimed re-upsert of an existing card: nothing to change
        // and no countdown to refresh. Owner-managed status cards (e.g.
        // kiwi.connection) get re-asserted from ~10 event-driven sync sites,
        // so skip the full relayout() — sort + word-wrap sizing + setMask +
        // repaint — that would otherwise churn against the 60 fps FFT feed. (#3999 review)
        if (!wasRemoving && !contentChanged && expiry < 0 && item.expiresAtMs < 0) {
            return true;
        }
        item.message = std::move(message);
        item.expiresAtMs = expiry;
        item.removing = false;
        item.targetOpacity = 1.0;
        if (wasRemoving || contentChanged || expiry > 0) {
            item.sequence = m_nextSequence++;
        }
        if (item.closeButton) {
            item.closeButton->setObjectName(buttonObjectNameForId(item.message.id));
            item.closeButton->setAccessibleName(
                tr("Close panadapter message: %1")
                    .arg(item.message.title.isEmpty()
                             ? item.message.detail
                             : item.message.title));
            item.closeButton->setToolTip(item.closeButton->accessibleName());
        }
    } else {
        Item item;
        item.message = std::move(message);
        item.expiresAtMs = expiry;
        item.sequence = m_nextSequence++;
        item.opacity = 0.0;
        item.targetOpacity = 1.0;
        m_items.append(std::move(item));
    }

    relayout();
    scheduleExpiryTimer();
    updateAccessibilityDescription(true);
    return true;
}

bool PanadapterMessageOverlay::removeMessage(const QString& id)
{
    const QString key = normalizedId(id);
    if (key.isEmpty()) {
        return false;
    }
    if (indexOf(key) < 0) {
        return false;
    }
    requestRemove(key, RemoveReason::Owner);
    return true;
}

void PanadapterMessageOverlay::clearMessages()
{
    for (Item& item : m_items) {
        item.removing = true;
        item.targetOpacity = 0.0;
    }
    relayout();
    updateAccessibilityDescription(true);
}

bool PanadapterMessageOverlay::hasMessages() const
{
    return !m_items.isEmpty();
}

QVariantList PanadapterMessageOverlay::messageSnapshot() const
{
    QVariantList out;
    const qint64 now = m_clock.isValid() ? m_clock.elapsed() : 0;
    for (const Item& item : m_items) {
        QVariantMap rect;
        rect[QStringLiteral("x")] = qRound(item.currentRect.x());
        rect[QStringLiteral("y")] = qRound(item.currentRect.y());
        rect[QStringLiteral("w")] = qRound(item.currentRect.width());
        rect[QStringLiteral("h")] = qRound(item.currentRect.height());

        QVariantMap m;
        m[QStringLiteral("id")] = item.message.id;
        m[QStringLiteral("title")] = item.message.title;
        m[QStringLiteral("detail")] = item.message.detail;
        m[QStringLiteral("timeoutMs")] = item.message.timeoutMs;
        m[QStringLiteral("remainingMs")] =
            item.expiresAtMs > 0 ? qMax<qint64>(0, item.expiresAtMs - now) : -1;
        m[QStringLiteral("countdown")] = countdownText(item.expiresAtMs,
                                                       item.removing,
                                                       now);
        m[QStringLiteral("dismissible")] = item.message.dismissible;
        m[QStringLiteral("tone")] = toneName(item.message.tone);
        m[QStringLiteral("removing")] = item.removing;
        m[QStringLiteral("opacity")] = item.opacity;
        m[QStringLiteral("geometry")] = rect;
        out.append(m);
    }
    return out;
}

void PanadapterMessageOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    if (m_items.isEmpty()) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qint64 now = m_clock.isValid() ? m_clock.elapsed() : 0;

    for (int i = m_items.size() - 1; i >= 0; --i) {
        const Item& item = m_items[i];
        if (item.opacity <= 0.01 || item.currentRect.isEmpty()) {
            continue;
        }

        p.save();
        p.setOpacity(item.opacity);

        const QRectF box = item.currentRect;
        const QColor accent = accentForTone(item.message.tone);

        for (int shadow = 3; shadow >= 1; --shadow) {
            const int alpha = 28 + shadow * 6;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, alpha));
            p.drawRoundedRect(box.adjusted(-shadow, shadow, shadow, shadow),
                              8 + shadow,
                              8 + shadow);
        }

        QLinearGradient fill(box.topLeft(), box.bottomLeft());
        fill.setColorAt(0.0, AetherSDR::theme::withAlpha("color.background.1", 238));
        fill.setColorAt(1.0, AetherSDR::theme::withAlpha("color.background.0", 226));

        p.setBrush(fill);
        p.setPen(QPen(AetherSDR::theme::withAlpha("color.background.3", 180), 1));
        p.drawRoundedRect(box, 7, 7);

        p.setPen(QPen(QColor(255, 255, 255, 32), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(1, 1, -1, -1), 6, 6);

        QPainterPath clip;
        clip.addRoundedRect(box, 7, 7);
        p.save();
        p.setClipPath(clip);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRect(QRectF(box.left(), box.top(),
                          kAccentRailWidth, box.height()));
        p.restore();

        const QRectF iconRect(box.left() + 9.0,
                              box.top() + (box.height() - 42.0) / 2.0,
                              42.0,
                              42.0);
        drawToneIcon(p, iconRect, item.message.tone, accent);

        QFont titleFont = p.font();
        titleFont.setPointSize(14);
        titleFont.setBold(true);
        QFont detailFont = titleFont;
        detailFont.setPointSize(10);
        detailFont.setBold(false);
        QFont countdownFont = detailFont;
        countdownFont.setPointSize(9);
        countdownFont.setBold(true);

        const QString countdown = countdownText(item.expiresAtMs,
                                                item.removing,
                                                now);
        QFontMetrics countdownFm(countdownFont);
        QRectF countdownRect;
        if (!countdown.isEmpty()) {
            const int countW = countdownWidth(countdown, countdownFm);
            const int countH = qMax(kCountdownHeight, countdownFm.height() + 4);
            countdownRect = QRectF(box.right() - kCloseInset - countW,
                                   box.bottom() - kCloseInset - countH,
                                   countW,
                                   countH);
        }

        QRectF textRect = box.adjusted(
            kIconColumnWidth + 8,
            kCardPaddingTop,
            item.message.dismissible ? -(kCloseSize + kCloseInset + 16) : -16,
            -kCardPaddingBottom);
        if (textRect.width() < 40.0) {
            textRect = box.adjusted(kCardPaddingX, 10, -kCardPaddingX, -10);
        }

        const bool hasTitle = !item.message.title.isEmpty();
        const bool hasDetail = !item.message.detail.isEmpty();
        qreal y = textRect.top();

        if (hasTitle) {
            p.setFont(titleFont);
            p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
            QFontMetrics titleFm(titleFont);
            const QRect titleBounds = titleFm.boundingRect(
                QRect(0, 0, qRound(textRect.width()), 200),
                Qt::AlignLeft | Qt::TextWordWrap,
                item.message.title);
            const QRectF titleRect(textRect.left(), y,
                                   textRect.width(), titleBounds.height() + 4);
            p.drawText(titleRect, Qt::AlignLeft | Qt::TextWordWrap,
                       item.message.title);
            y = titleRect.bottom() + (hasDetail ? 4.0 : 0.0);
        }

        if (hasDetail) {
            p.setFont(detailFont);
            p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
            const qreal detailRight = countdownRect.isValid()
                ? qMin(textRect.right(), countdownRect.left() - kCountdownGap)
                : textRect.right();
            const qreal detailWidth = qMax<qreal>(40.0, detailRight - textRect.left());
            QFontMetrics detailFm(detailFont);
            const QRect detailBounds = detailFm.boundingRect(
                QRect(0, 0, qRound(detailWidth), 240),
                Qt::AlignLeft | Qt::TextWordWrap,
                item.message.detail);
            const QRectF detailRect(textRect.left(), y,
                                    detailWidth, detailBounds.height() + 6);
            p.drawText(detailRect, Qt::AlignLeft | Qt::TextWordWrap,
                       item.message.detail);
        }

        if (!countdown.isEmpty()) {
            p.setFont(countdownFont);
            p.setPen(QPen(QColor(255, 214, 88, 220), 1));
            p.setBrush(QColor(64, 48, 0, 205));
            p.drawRoundedRect(countdownRect, 5, 5);
            p.setPen(QColor(255, 226, 110, 245));
            p.drawText(countdownRect, Qt::AlignCenter, countdown);
        }

        p.restore();
    }
}

void PanadapterMessageOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_items.isEmpty()) {
        relayout();
    }
}

bool PanadapterMessageOverlay::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (!pointInsideMessage(mouse->position().toPoint())) {
            event->ignore();
            return false;
        }
        // Inside a card: consume the event. Letting it fall through to
        // QWidget's default handlers ignore()s it, and Qt then re-delivers it
        // to the panadapter underneath — a click on a card (including the
        // "Transmit disabled" warning) would click-tune the slice at a
        // frequency the card is hiding. (#3999 review)
        event->accept();
        return true;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (pointInsideMessage(wheel->position().toPoint())) {
            event->accept();  // don't zoom/step the pan under a card
            return true;
        }
        event->ignore();
        return false;
    }
    default:
        break;
    }
    return QWidget::event(event);
}

QString PanadapterMessageOverlay::normalizedId(const QString& id) const
{
    return id.trimmed();
}

QSize PanadapterMessageOverlay::cardSizeFor(
    const PanadapterOverlayMessage& message) const
{
    QFont titleFont = font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    QFont detailFont = titleFont;
    detailFont.setPointSize(10);
    detailFont.setBold(false);
    QFont countdownFont = detailFont;
    countdownFont.setPointSize(9);
    countdownFont.setBold(true);

    const int closeReserve = message.dismissible ? kCloseSize + kCloseInset : 0;
    const QString initialCountdown = message.timeoutMs > 0
        ? QStringLiteral("%1s").arg(qMax(1, (message.timeoutMs + 999) / 1000))
        : QString();
    const QFontMetrics countdownFm(countdownFont);
    const int countdownReserve = message.timeoutMs > 0
        ? countdownWidth(initialCountdown, countdownFm) + kCountdownGap
        : 0;
    const int sideReserve = kIconColumnWidth + 8 + closeReserve + kCardPaddingX;
    const int maxTextWidth = qMax(
        120,
        qMin(width() - 32 - sideReserve, kCardMaxTextWidth));
    const int detailTextWidth = qMax(120, maxTextWidth - countdownReserve);
    const QFontMetrics titleFm(titleFont);
    const QFontMetrics detailFm(detailFont);
    const QRect titleBounds = message.title.isEmpty()
        ? QRect()
        : titleFm.boundingRect(QRect(0, 0, maxTextWidth, 200),
                               Qt::AlignLeft | Qt::TextWordWrap,
                               message.title);
    const QRect detailBounds = message.detail.isEmpty()
        ? QRect()
        : detailFm.boundingRect(QRect(0, 0, detailTextWidth, 240),
                                Qt::AlignLeft | Qt::TextWordWrap,
                                message.detail);

    const int textW = qMax(titleBounds.width(),
                           detailBounds.width() + countdownReserve);
    // The 360px design floor overflows narrow overlays — PanadapterStack
    // splitter panes drag well below that in side-by-side layouts — pushing
    // the card past the right edge and the close button entirely off-widget
    // (unreadable, unclickable). Clamp the floor to the available width when
    // the overlay is smaller than the design minimum. (#3999 review)
    const int available = qMax(120, width() - 2 * kStackMargin);
    const int floorW = qMin(kCardMinWidth, available);
    const int maxBoxWidth = qMax(floorW, available);
    const int boxW = qBound(floorW,
                            textW + sideReserve,
                            maxBoxWidth);
    int boxH = kCardPaddingTop + kCardPaddingBottom;
    if (!message.title.isEmpty()) {
        boxH += titleBounds.height() + 4;
    }
    if (!message.title.isEmpty() && !message.detail.isEmpty()) {
        boxH += 4;
    }
    if (!message.detail.isEmpty()) {
        boxH += detailBounds.height() + 6;
    }
    if (message.timeoutMs > 0) {
        boxH = qMax(boxH, kCardPaddingTop + kCardPaddingBottom + kCountdownHeight);
    }
    boxH = qMax(boxH, kCloseSize + kCloseInset * 2);
    return QSize(boxW, boxH);
}

int PanadapterMessageOverlay::indexOf(const QString& id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].message.id == id) {
            return i;
        }
    }
    return -1;
}

QToolButton* PanadapterMessageOverlay::ensureCloseButton(Item& item)
{
    if (!item.closeButton) {
        item.closeButton = new OverlayCloseButton(this);
        item.closeButton->setFixedSize(kCloseSize, kCloseSize);
        item.closeButton->setCursor(Qt::PointingHandCursor);
        item.closeButton->setAutoRaise(false);
        item.closeButton->setFocusPolicy(Qt::TabFocus);
        connect(item.closeButton, &QToolButton::clicked, this, [this, button = item.closeButton]() {
            const QString id = button->property("messageId").toString();
            requestRemove(id, RemoveReason::Manual);
        });
    }

    item.closeButton->setProperty("messageId", item.message.id);
    item.closeButton->setObjectName(buttonObjectNameForId(item.message.id));
    const QString name = tr("Close panadapter message: %1")
        .arg(item.message.title.isEmpty() ? item.message.detail : item.message.title);
    item.closeButton->setAccessibleName(name);
    item.closeButton->setToolTip(name);
    return item.closeButton;
}

void PanadapterMessageOverlay::requestRemove(const QString& id,
                                             RemoveReason reason)
{
    Q_UNUSED(reason)

    const QString key = normalizedId(id);
    const int idx = indexOf(key);
    if (idx < 0) {
        return;
    }

    Item& item = m_items[idx];
    item.removing = true;
    item.targetOpacity = 0.0;
    item.expiresAtMs = -1;
    if (item.closeButton) {
        item.closeButton->hide();
    }
    relayout();
    scheduleExpiryTimer();
    updateAccessibilityDescription(true);
}

void PanadapterMessageOverlay::relayout()
{
    if (m_items.isEmpty()) {
        hide();
        clearMask();
        m_animationTimer.stop();
        m_expiryTimer.stop();
        m_countdownTimer.stop();
        return;
    }

    std::sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) {
        return a.sequence > b.sequence;
    });

    QVector<int> liveIndexes;
    QVector<QSize> liveSizes;
    int totalHeight = 0;
    int stackWidth = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].removing) {
            continue;
        }
        const QSize size = cardSizeFor(m_items[i].message);
        liveIndexes.append(i);
        liveSizes.append(size);
        stackWidth = qMax(stackWidth, size.width());
        totalHeight += size.height();
        if (liveIndexes.size() > 1) {
            totalHeight += kCardSpacing;
        }
    }

    if (!liveIndexes.isEmpty()) {
        const int firstHeight = liveSizes.first().height();
        const int preferredTop = rect().center().y() - firstHeight / 2;
        const int maxTop = qMax(kStackMargin, height() - totalHeight - kStackMargin);
        int y = qBound(kStackMargin, preferredTop, maxTop);

        for (int i = 0; i < liveIndexes.size(); ++i) {
            Item& item = m_items[liveIndexes[i]];
            QSize size = liveSizes[i];
            size.setWidth(stackWidth);
            const int x = qMax(kStackMargin, (width() - size.width()) / 2);
            item.targetRect = QRectF(x, y, size.width(), size.height());
            item.targetOpacity = 1.0;
            if (item.currentRect.isNull()) {
                item.currentRect = item.targetRect.translated(0, -10);
                item.opacity = 0.0;
            }
            y += size.height() + kCardSpacing;
        }
    }

    for (Item& item : m_items) {
        if (!item.removing) {
            continue;
        }
        if (item.currentRect.isNull()) {
            item.currentRect = item.targetRect;
        }
        item.targetRect = item.currentRect.translated(0, -8);
        item.targetOpacity = 0.0;
    }

    show();
    raise();
    updateCloseButtons();
    updateInputMask();
    if (!m_animationTimer.isActive()) {
        m_animationTimer.start();
    }
    update();
}

void PanadapterMessageOverlay::tickAnimation()
{
    bool anyAnimating = false;

    for (Item& item : m_items) {
        item.currentRect = easedRect(item.currentRect, item.targetRect);
        item.opacity += (item.targetOpacity - item.opacity) * kAnimationStep;

        if (!rectCloseEnough(item.currentRect, item.targetRect)
            || std::abs(item.opacity - item.targetOpacity) > kDoneOpacity) {
            anyAnimating = true;
        } else {
            item.currentRect = item.targetRect;
            item.opacity = item.targetOpacity;
        }
    }

    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i].removing && m_items[i].opacity <= kDoneOpacity) {
            if (m_items[i].closeButton) {
                delete m_items[i].closeButton;
            }
            m_items.removeAt(i);
            updateAccessibilityDescription(true);
        }
    }

    updateCloseButtons();
    updateInputMask();
    update();

    if (m_items.isEmpty()) {
        hide();
        clearMask();
        m_animationTimer.stop();
        m_expiryTimer.stop();
        m_countdownTimer.stop();
        updateAccessibilityDescription(true);
        return;
    }

    if (!anyAnimating) {
        m_animationTimer.stop();
    }
}

void PanadapterMessageOverlay::expireDueMessages()
{
    const qint64 now = m_clock.elapsed();
    QStringList expired;
    for (const Item& item : m_items) {
        if (!item.removing && item.expiresAtMs > 0 && item.expiresAtMs <= now) {
            expired.append(item.message.id);
        }
    }
    for (const QString& id : expired) {
        requestRemove(id, RemoveReason::Expired);
    }
    scheduleExpiryTimer();
}

void PanadapterMessageOverlay::scheduleExpiryTimer()
{
    qint64 next = -1;
    bool hasTimedMessage = false;
    const qint64 now = m_clock.elapsed();
    for (const Item& item : m_items) {
        if (item.removing || item.expiresAtMs <= 0) {
            continue;
        }
        hasTimedMessage = true;
        if (next < 0 || item.expiresAtMs < next) {
            next = item.expiresAtMs;
        }
    }

    if (hasTimedMessage) {
        if (!m_countdownTimer.isActive()) {
            m_countdownTimer.start();
        }
    } else {
        m_countdownTimer.stop();
    }

    if (next < 0) {
        m_expiryTimer.stop();
        return;
    }
    const qint64 delayMs = std::clamp<qint64>(
        next - now,
        1,
        static_cast<qint64>(std::numeric_limits<int>::max()));
    m_expiryTimer.start(static_cast<int>(delayMs));
}

void PanadapterMessageOverlay::updateCloseButtons()
{
    // Below a usable width the X would sit on top of the text or off the card
    // edge; suppress it there (the card still shows and, if timed, still
    // expires). (#3999 review)
    const int minWidthForClose = kIconColumnWidth + kCloseSize + 2 * kCloseInset;
    for (Item& item : m_items) {
        if (!item.message.dismissible || item.removing || item.opacity <= 0.05
            || item.currentRect.width() < minWidthForClose) {
            if (item.closeButton) {
                item.closeButton->hide();
            }
            continue;
        }

        QToolButton* button = ensureCloseButton(item);
        button->move(qRound(item.currentRect.right()) - kCloseSize - kCloseInset,
                     qRound(item.currentRect.top()) + kCloseInset);
        button->show();
        button->raise();
    }
}

void PanadapterMessageOverlay::updateInputMask()
{
    QRegion region;
    for (const Item& item : m_items) {
        if (item.opacity <= 0.01
            || (item.currentRect.isEmpty() && item.targetRect.isEmpty())) {
            continue;
        }

        if (!item.currentRect.isEmpty()) {
            region += item.currentRect.toAlignedRect().adjusted(-2, -2, 2, 2);
        }
        if (!item.targetRect.isEmpty()) {
            region += item.targetRect.toAlignedRect().adjusted(-2, -2, 2, 2);
        }
    }

    if (region.isEmpty()) {
        clearMask();
        return;
    }
    setMask(region);
}

QString PanadapterMessageOverlay::accessibleSummary() const
{
    QStringList messages;
    const qint64 now = m_clock.isValid() ? m_clock.elapsed() : 0;
    for (const Item& item : m_items) {
        if (item.removing) {
            continue;
        }

        QString message;
        const auto appendSentence = [&message](const QString& part) {
            if (part.isEmpty()) {
                return;
            }
            if (!message.isEmpty()) {
                const QChar last = message.at(message.size() - 1);
                message += (last == QLatin1Char('.') || last == QLatin1Char('!')
                            || last == QLatin1Char('?'))
                    ? QStringLiteral(" ")
                    : QStringLiteral(". ");
            }
            message += part;
        };
        appendSentence(item.message.title);
        appendSentence(item.message.detail);

        const QString countdown = countdownText(item.expiresAtMs, item.removing, now);
        if (!countdown.isEmpty()) {
            appendSentence(tr("Expires in %1").arg(countdown));
        }
        if (!message.isEmpty()) {
            messages << message;
        }
    }

    return messages.isEmpty()
        ? tr("No panadapter messages")
        : messages.join(QStringLiteral(" | "));
}

void PanadapterMessageOverlay::updateAccessibilityDescription(bool notify)
{
    const QString summary = accessibleSummary();
    if (accessibleDescription() == summary) {
        return;
    }

    setAccessibleDescription(summary);
    if (notify) {
        QAccessibleEvent event(this, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

bool PanadapterMessageOverlay::pointInsideMessage(const QPoint& point) const
{
    // Mirror updateInputMask()'s membership exactly: any card still painted
    // (opacity > 0.01) is in the input mask and receives events — including a
    // card fading out. If this diverged (e.g. by skipping `removing` items),
    // a click on a still-visible fading card would take the ignore() path in
    // event() and fall through to click-tune the panadapter beneath. (#3999 review)
    for (const Item& item : m_items) {
        if (item.opacity <= 0.01) {
            continue;
        }
        if (!item.currentRect.isEmpty() && item.currentRect.contains(point)) {
            return true;
        }
        if (!item.targetRect.isEmpty() && item.targetRect.contains(point)) {
            return true;
        }
    }
    return false;
}

} // namespace AetherSDR
