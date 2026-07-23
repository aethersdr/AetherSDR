// Contribute.cpp — animated AetherSDR community credits dialog.
//
// Owns the demo-scene renderer, Open Collective supporter fetch, controls,
// and audio lifecycle. MainWindow_Menus.cpp retains only the About button and
// its one-line showOrRaisePersistent() launcher.

#include "Contribute.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QAudioOutput>
#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLinearGradient>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

struct CommunityStar {
    qreal x{0.0};
    qreal y{0.0};
    qreal radius{1.0};
    qreal phase{0.0};
    qreal drift{0.0};
};

class CommunityCreditsCanvas final : public QWidget {
public:
    explicit CommunityCreditsCanvas(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_radioImage(QStringLiteral(":/images/aurora-cutout.png"))
    {
        setMinimumSize(640, 390);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("AetherSDR community credits animation"));
        setAccessibleDescription(
            QStringLiteral("An animated thank-you to AetherSDR contributors and supporters."));

        quint32 seed = 0xa37e5d12U;
        m_stars.reserve(110);
        for (int index = 0; index < 110; ++index) {
            const auto randomUnit = [&seed]() {
                seed = seed * 1664525U + 1013904223U;
                return static_cast<qreal>(seed & 0x00ffffffU)
                    / static_cast<qreal>(0x01000000U);
            };
            m_stars.push_back({randomUnit(), randomUnit(),
                               0.45 + randomUnit() * 1.45,
                               randomUnit() * 6.283185307179586,
                               0.0005 + randomUnit() * 0.0012});
        }

        m_elapsed.start();
        m_animationTimer.setInterval(16);
        connect(&m_animationTimer, &QTimer::timeout, this,
                qOverload<>(&CommunityCreditsCanvas::update));
        // The timer is driven by showEvent/hideEvent so a backgrounded or
        // minimised credits window doesn't keep waking the CPU 60×/s.
    }

    void applySupporters(const QStringList& supporters)
    {
        if (supporters.isEmpty()) {
            return;
        }
        m_supporters = supporters;
        m_scrollText = QStringLiteral("  %1  ★  THANK YOU FOR KEEPING AETHERSDR ON THE AIR  ★  ")
                           .arg(m_supporters.join(QStringLiteral("  ★  ")));
        setAccessibleDescription(
            QStringLiteral("Animated AetherSDR community credits. Signal boosters: %1")
                .arg(m_supporters.join(QStringLiteral(", "))));
        update();
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        if (!m_animationTimer.isActive()) {
            m_animationTimer.start();
        }
    }

    void hideEvent(QHideEvent* event) override
    {
        m_animationTimer.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        const QRectF viewport(rect());
        painter.fillRect(viewport, QColor(2, 7, 18));
        const qreal seconds = static_cast<qreal>(m_elapsed.elapsed()) / 1000.0;
        paintStarfield(painter, viewport, seconds);
        paintAurora(painter, viewport, seconds);

        if (m_radioImage.isNull()) {
            return;
        }

        const qreal imageAspect = static_cast<qreal>(m_radioImage.width())
            / static_cast<qreal>(m_radioImage.height());
        QSizeF sceneSize = viewport.size();
        if (sceneSize.width() / sceneSize.height() > imageAspect) {
            sceneSize.setWidth(sceneSize.height() * imageAspect);
        } else {
            sceneSize.setHeight(sceneSize.width() / imageAspect);
        }
        const QRectF sceneRect(
            viewport.center().x() - sceneSize.width() / 2.0,
            viewport.center().y() - sceneSize.height() / 2.0,
            sceneSize.width(), sceneSize.height());

        // Drawn at full opacity: any sub-1.0 opacity forces QPainter onto a
        // destination read-back software compositing path (~4 MB/frame here).
        // The aurora is painted before the cutout and the PNG carries its own
        // alpha, so it already bleeds through the transparent regions.
        painter.drawImage(sceneRect, m_radioImage);

        // Measured from the cleaned product cutout. Keeping this in source-
        // image coordinates makes the text stay inside the physical LCD at
        // every dialog size.
        const QRectF sourceScreen(240.0, 228.0, 667.0, 419.0);
        const QRectF screenRect(
            sceneRect.left() + sourceScreen.left() / m_radioImage.width() * sceneRect.width(),
            sceneRect.top() + sourceScreen.top() / m_radioImage.height() * sceneRect.height(),
            sourceScreen.width() / m_radioImage.width() * sceneRect.width(),
            sourceScreen.height() / m_radioImage.height() * sceneRect.height());
        paintRadioScreen(painter, screenRect, seconds);

        QLinearGradient vignette(viewport.topLeft(), viewport.bottomRight());
        vignette.setColorAt(0.0, QColor(0, 0, 0, 90));
        vignette.setColorAt(0.35, QColor(0, 0, 0, 0));
        vignette.setColorAt(0.75, QColor(0, 0, 0, 0));
        vignette.setColorAt(1.0, QColor(0, 0, 0, 120));
        painter.fillRect(viewport, vignette);
    }

private:
    void paintStarfield(QPainter& painter, const QRectF& viewport, qreal seconds) const
    {
        painter.save();
        for (const CommunityStar& star : m_stars) {
            const qreal xUnit = std::fmod(star.x + seconds * star.drift, 1.0);
            const qreal pulse = 0.55 + 0.45 * std::sin(seconds * 1.4 + star.phase);
            const QPointF center(viewport.left() + xUnit * viewport.width(),
                                 viewport.top() + star.y * viewport.height());
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(175, 220, 255, static_cast<int>(60 + pulse * 150)));
            painter.drawEllipse(center, star.radius, star.radius);
        }
        painter.restore();
    }

    static void paintAuroraRibbon(QPainter& painter, const QRectF& viewport,
                                  qreal seconds, qreal yUnit, qreal amplitude,
                                  qreal phase, const QColor& color, qreal width)
    {
        QPainterPath ribbon;
        const qreal left = viewport.left() - viewport.width() * 0.1;
        const qreal right = viewport.right() + viewport.width() * 0.1;
        constexpr int kSegments = 36;
        for (int index = 0; index <= kSegments; ++index) {
            const qreal unit = static_cast<qreal>(index) / kSegments;
            const qreal x = left + unit * (right - left);
            const qreal wave = std::sin(unit * 7.0 + seconds * 0.22 + phase)
                + 0.42 * std::sin(unit * 15.0 - seconds * 0.13 + phase * 1.7);
            const qreal y = viewport.top() + viewport.height() * (yUnit + amplitude * wave);
            if (index == 0) {
                ribbon.moveTo(x, y);
            } else {
                ribbon.lineTo(x, y);
            }
        }
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(ribbon);
    }

    static void paintAurora(QPainter& painter, const QRectF& viewport, qreal seconds)
    {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        paintAuroraRibbon(painter, viewport, seconds, 0.18, 0.055, 0.0,
                          QColor(0, 188, 214, 44), viewport.height() * 0.12);
        paintAuroraRibbon(painter, viewport, seconds, 0.28, 0.075, 2.1,
                          QColor(58, 232, 161, 35), viewport.height() * 0.17);
        paintAuroraRibbon(painter, viewport, seconds, 0.10, 0.045, 4.2,
                          QColor(126, 92, 255, 34), viewport.height() * 0.09);
        painter.restore();
    }

    static qreal smoothStep(qreal value)
    {
        const qreal clamped = std::clamp(value, 0.0, 1.0);
        return clamped * clamped * (3.0 - 2.0 * clamped);
    }

    static qreal deterministicUnit(int index, qreal salt)
    {
        const qreal value = std::sin(index * 91.713 + salt * 47.239) * 43758.5453;
        return value - std::floor(value);
    }

    static void paintLcdStarTunnel(QPainter& painter, const QRectF& screenRect, qreal seconds)
    {
        const QPointF vanishingPoint(screenRect.left() + screenRect.width() * 0.51,
                                     screenRect.top() + screenRect.height() * 0.51);
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        constexpr int kStarCount = 72;
        for (int index = 0; index < kStarCount; ++index) {
            const qreal speed = 0.10 + deterministicUnit(index, 4.0) * 0.13;
            qreal depth = std::fmod(deterministicUnit(index, 1.0) - seconds * speed, 1.0);
            if (depth < 0.0) {
                depth += 1.0;
            }
            depth = 0.055 + depth * 0.945;
            const qreal sourceX = (deterministicUnit(index, 2.0) - 0.5) * 0.42;
            const qreal sourceY = (deterministicUnit(index, 3.0) - 0.5) * 0.34;
            const QPointF position(
                vanishingPoint.x() + sourceX * screenRect.width() / depth,
                vanishingPoint.y() + sourceY * screenRect.height() / depth);
            if (!screenRect.adjusted(-4.0, -4.0, 4.0, 4.0).contains(position)) {
                continue;
            }

            const qreal brightness = std::pow(1.0 - depth, 1.4);
            const QPointF direction = position - vanishingPoint;
            const QPointF tail = position - direction * (0.018 + brightness * 0.045);
            const QColor color(112 + qRound(brightness * 120),
                               190 + qRound(brightness * 60), 255,
                               35 + qRound(brightness * 210));
            painter.setPen(QPen(color, 0.5 + brightness * 1.5, Qt::SolidLine,
                                Qt::RoundCap));
            painter.drawLine(tail, position);
            if (brightness > 0.72) {
                painter.drawLine(position - QPointF(2.5, 0.0),
                                 position + QPointF(2.5, 0.0));
                painter.drawLine(position - QPointF(0.0, 2.5),
                                 position + QPointF(0.0, 2.5));
            }
        }
        painter.restore();
    }

    static void paintCopperBars(QPainter& painter, const QRectF& screenRect, qreal seconds)
    {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        for (int index = 0; index < 3; ++index) {
            const qreal travel = std::fmod(seconds * (0.055 + index * 0.013)
                                               + index * 0.31,
                                           1.25) - 0.12;
            const qreal y = screenRect.top() + travel * screenRect.height();
            const qreal halfHeight = screenRect.height() * (0.028 + index * 0.006);
            QLinearGradient bar(screenRect.left(), y - halfHeight,
                                screenRect.left(), y + halfHeight);
            const QColor core = index == 0 ? QColor(0, 238, 255, 75)
                : index == 1 ? QColor(106, 255, 177, 58)
                             : QColor(176, 82, 255, 50);
            bar.setColorAt(0.0, QColor(core.red(), core.green(), core.blue(), 0));
            bar.setColorAt(0.46, core);
            bar.setColorAt(0.54, core.lighter(150));
            bar.setColorAt(1.0, QColor(core.red(), core.green(), core.blue(), 0));
            painter.fillRect(QRectF(screenRect.left(), y - halfHeight,
                                    screenRect.width(), halfHeight * 2.0), bar);
        }
        painter.restore();
    }

    static void paintAnimatedTitle(QPainter& painter, const QRectF& screenRect,
                                   qreal seconds)
    {
        const QString title = QStringLiteral("THANK YOU");
        QFont font(QStringLiteral("Inter"));
        font.setPixelSize(std::max(12, qRound(screenRect.height() * 0.125)));
        font.setBold(true);
        font.setLetterSpacing(QFont::AbsoluteSpacing, screenRect.width() * 0.006);
        const QFontMetricsF metrics(font);
        const qreal totalWidth = metrics.horizontalAdvance(title);
        qreal x = screenRect.center().x() - totalWidth / 2.0;
        const qreal baseline = screenRect.top() + screenRect.height() * 0.38;

        painter.save();
        painter.setFont(font);
        for (int index = 0; index < title.size(); ++index) {
            const QString glyph(title.at(index));
            const qreal glyphWidth = metrics.horizontalAdvance(glyph);
            const qreal pulse = 0.5 + 0.5 * std::sin(seconds * 3.4 - index * 0.72);
            const qreal bob = std::sin(seconds * 2.1 + index * 0.86)
                * screenRect.height() * 0.017;
            const qreal angle = std::sin(seconds * 1.65 + index * 0.57) * 4.5;
            const qreal scale = 0.94 + pulse * 0.12;
            const QPointF center(x + glyphWidth / 2.0,
                                 baseline - metrics.ascent() / 2.0 + bob);

            painter.save();
            painter.translate(center);
            painter.rotate(angle);
            painter.scale(scale, scale);
            const QPointF glyphOrigin(-glyphWidth / 2.0, metrics.ascent() / 2.0);
            painter.setPen(QColor(0, 220, 255, 75 + qRound(pulse * 80)));
            painter.drawText(glyphOrigin + QPointF(0.0, 2.0), glyph);
            painter.setPen(QColor(210 + qRound(pulse * 45), 247, 255));
            painter.drawText(glyphOrigin, glyph);
            painter.restore();
            x += glyphWidth;
        }
        painter.restore();
    }

    void paintFlyingName(QPainter& painter, const QRectF& screenRect, qreal seconds) const
    {
        const QStringList fallbackNames{
            QStringLiteral("CONTRIBUTORS"), QStringLiteral("TESTERS"),
            QStringLiteral("REPORTERS"), QStringLiteral("SIGNAL BOOSTERS")};
        const QStringList& names = m_supporters.isEmpty() ? fallbackNames : m_supporters;
        constexpr qreal kNamePeriod = 3.4;
        const int nameIndex = static_cast<int>(seconds / kNamePeriod) % names.size();
        const qreal phase = std::fmod(seconds, kNamePeriod);
        const QString name = names.at(nameIndex).toUpper();

        qreal opacity = 1.0;
        qreal scale = 1.0;
        qreal zRotation = 0.0;
        if (phase < 0.62) {
            const qreal arrival = smoothStep(phase / 0.62);
            scale = 0.18 + arrival * 0.86;
            opacity = arrival;
            zRotation = (1.0 - arrival) * -18.0;
        } else if (phase > 2.72) {
            const qreal departure = smoothStep((phase - 2.72) / 0.68);
            scale = 1.0 + departure * 1.35;
            opacity = 1.0 - departure;
            zRotation = departure * 13.0;
        } else {
            scale = 1.0 + std::sin(seconds * 4.0) * 0.025;
        }

        QFont font(QStringLiteral("Inter"));
        int pixelSize = std::max(9, qRound(screenRect.height() * 0.075));
        font.setPixelSize(pixelSize);
        font.setBold(true);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        QFontMetricsF metrics(font);
        while (metrics.horizontalAdvance(name) > screenRect.width() * 0.77 && pixelSize > 8) {
            font.setPixelSize(--pixelSize);
            metrics = QFontMetricsF(font);
        }

        const QPointF center(screenRect.center().x(),
                             screenRect.top() + screenRect.height() * 0.51);
        const qreal burst = smoothStep(std::min(phase / 0.75, 1.0));
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        for (int index = 0; index < 18; ++index) {
            const qreal angle = deterministicUnit(index, nameIndex + 11.0) * 6.283185307179586;
            const qreal length = screenRect.height()
                * (0.025 + deterministicUnit(index, nameIndex + 12.0) * 0.11) * burst;
            const QPointF direction(std::cos(angle), std::sin(angle));
            const QPointF head = center + direction * length;
            painter.setPen(QPen(QColor(80, 235, 255,
                                      qRound(opacity * (100 + (1.0 - burst) * 100))),
                                0.7 + deterministicUnit(index, 13.0) * 1.2,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(head - direction * length * 0.28, head);
        }
        painter.restore();

        painter.save();
        painter.setFont(font);
        painter.setOpacity(opacity);
        painter.translate(center);
        painter.rotate(zRotation);
        painter.scale(scale, scale);
        const qreal width = metrics.horizontalAdvance(name);
        const QPointF origin(-width / 2.0, metrics.ascent() / 2.0);
        painter.setPen(QColor(0, 0, 0, 185));
        painter.drawText(origin + QPointF(2.0, 3.0), name);
        painter.setPen(QColor(36, 255, 184, 115));
        painter.drawText(origin + QPointF(0.0, 1.5), name);
        painter.setPen(QColor(229, 255, 247));
        painter.drawText(origin, name);
        painter.restore();
    }

    void paintDxycpScroller(QPainter& painter, const QRectF& screenRect, qreal seconds) const
    {
        QFont font(QStringLiteral("Inter"));
        font.setPixelSize(std::max(10, qRound(screenRect.height() * 0.075)));
        font.setBold(true);
        const QFontMetricsF metrics(font);
        const qreal textWidth = std::max<qreal>(1.0, metrics.horizontalAdvance(m_scrollText));
        const qreal gap = screenRect.width() * 0.28;
        const qreal cycleWidth = textWidth + gap;
        const qreal speed = std::max<qreal>(62.0, screenRect.width() * 0.18);
        qreal cursorX = screenRect.right() - std::fmod(seconds * speed, cycleWidth);
        int glyphSequence = 0;

        painter.save();
        painter.setFont(font);
        while (cursorX < screenRect.right()) {
            for (const QChar character : m_scrollText) {
                const QString glyph(character);
                const qreal glyphWidth = metrics.horizontalAdvance(glyph);
                if (cursorX + glyphWidth >= screenRect.left() - glyphWidth
                    && cursorX <= screenRect.right() + glyphWidth) {
                    const qreal horizontalUnit = (cursorX - screenRect.left())
                        / screenRect.width();
                    const qreal entry = smoothStep((screenRect.right() - cursorX)
                                                   / (screenRect.width() * 0.22));
                    const qreal exit = smoothStep((cursorX - screenRect.left())
                                                  / (screenRect.width() * 0.12));
                    const qreal wave = std::sin(horizontalUnit * 10.0 + seconds * 2.4)
                        + 0.42 * std::sin(horizontalUnit * 23.0 - seconds * 1.7);
                    const qreal y = screenRect.top() + screenRect.height()
                        * (0.70 + wave * 0.027);
                    const qreal scale = (0.30 + entry * 0.78) * (0.72 + exit * 0.28)
                        * (0.96 + 0.08 * std::sin(seconds * 3.0 + glyphSequence * 0.55));
                    const qreal rotation = std::cos(horizontalUnit * 10.0 + seconds * 2.4) * 7.0
                        + std::sin(seconds * 1.8 + glyphSequence * 0.31) * 2.5;
                    const qreal hue = std::fmod(0.46 + horizontalUnit * 0.16
                                                   + seconds * 0.035,
                                               1.0);
                    const QColor glyphColor = QColor::fromHsvF(hue, 0.48, 1.0, 1.0);

                    painter.save();
                    painter.translate(cursorX + glyphWidth / 2.0, y);
                    painter.rotate(rotation);
                    painter.scale(scale, scale);
                    const QPointF origin(-glyphWidth / 2.0, metrics.ascent() / 2.0);
                    painter.setPen(QColor(glyphColor.red(), glyphColor.green(),
                                          glyphColor.blue(), 70));
                    painter.drawText(origin + QPointF(0.0, 2.2), glyph);
                    painter.setPen(glyphColor.lighter(135));
                    painter.drawText(origin, glyph);
                    painter.restore();
                }
                cursorX += glyphWidth;
                ++glyphSequence;
            }
            cursorX += gap;
        }
        painter.restore();
    }

    void paintRadioScreen(QPainter& painter, const QRectF& screenRect, qreal seconds) const
    {
        painter.save();
        QPainterPath clipPath;
        clipPath.addRoundedRect(screenRect, screenRect.height() * 0.025,
                                screenRect.height() * 0.025);
        painter.setClipPath(clipPath);

        QLinearGradient displayGradient(screenRect.topLeft(), screenRect.bottomRight());
        displayGradient.setColorAt(0.0, QColor(2, 19, 35, 220));
        displayGradient.setColorAt(0.55, QColor(0, 43, 62, 205));
        displayGradient.setColorAt(1.0, QColor(4, 12, 31, 225));
        painter.fillRect(screenRect, displayGradient);

        paintLcdStarTunnel(painter, screenRect, seconds);
        paintCopperBars(painter, screenRect, seconds);

        painter.setPen(QPen(QColor(26, 201, 220, 32), 1.0));
        const qreal scanlineSpacing = std::max<qreal>(2.0, screenRect.height() * 0.018);
        for (qreal y = screenRect.top(); y < screenRect.bottom(); y += scanlineSpacing) {
            painter.drawLine(QPointF(screenRect.left(), y), QPointF(screenRect.right(), y));
        }

        QFont smallFont(QStringLiteral("Inter"));
        smallFont.setPixelSize(std::max(8, qRound(screenRect.height() * 0.053)));
        smallFont.setBold(true);
        smallFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
        painter.setFont(smallFont);
        painter.setPen(QColor(88, 228, 235, 225));
        painter.drawText(QRectF(screenRect.left(), screenRect.top() + screenRect.height() * 0.10,
                                screenRect.width(), screenRect.height() * 0.10),
                         Qt::AlignCenter, QStringLiteral("AETHERSDR COMMUNITY TRANSMISSION"));

        paintAnimatedTitle(painter, screenRect, seconds);
        paintFlyingName(painter, screenRect, seconds);
        paintDxycpScroller(painter, screenRect, seconds);

        QFont footerFont(QStringLiteral("Inter"));
        footerFont.setPixelSize(std::max(7, qRound(screenRect.height() * 0.039)));
        footerFont.setBold(true);
        footerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.75);
        painter.setFont(footerFont);
        painter.setPen(QColor(103, 206, 218, 205));
        painter.drawText(QRectF(screenRect.left() + screenRect.width() * 0.025,
                                screenRect.top() + screenRect.height() * 0.845,
                                screenRect.width() * 0.95,
                                screenRect.height() * 0.075),
                         Qt::AlignCenter,
                         QStringLiteral("DEFINING THE FRONTIER OF SOFTWARE-DEFINED RADIO"));
        painter.restore();
    }

    QImage m_radioImage;
    QVector<CommunityStar> m_stars;
    QStringList m_supporters;
    QString m_scrollText{
        QStringLiteral("  CONTRIBUTORS  ★  TESTERS  ★  REPORTERS  ★  SIGNAL BOOSTERS  ★  ")};
    QElapsedTimer m_elapsed;
    QTimer m_animationTimer;
};

void fetchOpenCollectiveSupporters(QDialog* owner, CommunityCreditsCanvas* canvas)
{
    // Opening the credits dialog issues one outbound request to Open
    // Collective's public API for the newest backers. It is user-initiated (the
    // dialog only opens from an explicit About action), non-blocking (7 s abort
    // below), and never persisted to disk. Users on metered, air-gapped, or
    // privacy-sensitive links can suppress it entirely by setting AppSettings
    // "CommunityCreditsFetchSupporters" to "False"; the built-in generic
    // scroller is shown instead.
    if (AetherSDR::AppSettings::instance()
            .value(QStringLiteral("CommunityCreditsFetchSupporters"), QStringLiteral("True"))
            .toString() != QStringLiteral("True")) {
        return;
    }

    // Backers change rarely, so the first successful result is cached for the
    // rest of the app session — reopening the dialog re-uses it instead of
    // hitting the API again.
    static QStringList s_supporterCache;
    if (!s_supporterCache.isEmpty()) {
        canvas->applySupporters(s_supporterCache);
        return;
    }

    auto* manager = new QNetworkAccessManager(owner);
    QNetworkRequest request{QUrl(QStringLiteral("https://api.opencollective.com/graphql/v2"))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", "AetherSDR-community-credits");

    const QString query = QStringLiteral(
        "query { account(slug: \"aethersdr\") { members(limit: 50, offset: 0, "
        "role: [BACKER], orderBy: { field: CREATED_AT, direction: DESC }) { nodes { "
        "account { name slug isIncognito } } } } }");
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("query"), query}})
                                   .toJson(QJsonDocument::Compact);
    QNetworkReply* reply = manager->post(request, payload);
    QTimer::singleShot(7000, reply, [reply] {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, owner, [canvas, reply] {
        const QByteArray response = reply->readAll();
        const bool succeeded = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!succeeded) {
            return;
        }

        const QJsonArray nodes = QJsonDocument::fromJson(response)
                                     .object().value(QStringLiteral("data")).toObject()
                                     .value(QStringLiteral("account")).toObject()
                                     .value(QStringLiteral("members")).toObject()
                                     .value(QStringLiteral("nodes")).toArray();
        QStringList supporters;
        for (const QJsonValue& value : nodes) {
            const QJsonObject account = value.toObject()
                                            .value(QStringLiteral("account")).toObject();
            QString name = account.value(QStringLiteral("name")).toString().trimmed();
            const bool anonymous = account.value(QStringLiteral("isIncognito")).toBool()
                || name.isEmpty() || name.compare(QStringLiteral("Guest"), Qt::CaseInsensitive) == 0;
            if (anonymous) {
                // Each anonymous backer keeps its own slot; only real names are
                // de-duplicated. Collapsing every incognito supporter into one
                // placeholder would erase most of the credit reel.
                supporters.push_back(QStringLiteral("Anonymous Signal Booster"));
                continue;
            }
            const bool duplicate = std::any_of(
                supporters.cbegin(), supporters.cend(), [&name](const QString& existing) {
                    return existing.compare(name, Qt::CaseInsensitive) == 0;
                });
            if (!duplicate) {
                supporters.push_back(name);
            }
        }
        if (!supporters.isEmpty()) {
            s_supporterCache = supporters;
        }
        canvas->applySupporters(supporters);
    });
}

} // namespace

namespace AetherSDR {

ContributeDialog::ContributeDialog(QWidget* parent)
    : PersistentDialog(QStringLiteral("AetherSDR Community Credits"),
                       QStringLiteral("CommunityCreditsDialogGeometry"), parent)
{
    setObjectName(QStringLiteral("communityCreditsDialog"));
    setMinimumSize(760, 500);
    resize(1040, 680);
    setBodyLayoutMargins(QMargins(0, 0, 0, 0), QMargins(0, 0, 0, 0));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* canvas = new CommunityCreditsCanvas;
    root->addWidget(canvas, 1);

    auto* controls = new QWidget;
    controls->setObjectName(QStringLiteral("communityCreditsControls"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        controls,
        "QWidget#communityCreditsControls { background: {{color.background.0}}; "
        "border-top: 1px solid {{color.background.2}}; }"
        "QPushButton { background: {{color.background.1}}; color: {{color.text.primary}}; "
        "border: 1px solid {{color.background.2}}; border-radius: 4px; padding: 6px 14px; }"
        "QPushButton:hover { border-color: {{color.accent}}; }"
        "QPushButton:checked { color: {{color.accent.bright}}; border-color: {{color.accent}}; }");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(12, 9, 12, 9);

    auto* trackLabel = new QLabel(QStringLiteral("♪ Friday · c512w · Scream Tracker 3"));
    trackLabel->setAccessibleName(
        QStringLiteral("Music: Friday by c512w, Scream Tracker 3 module"));
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        trackLabel, "QLabel { color: {{color.text.secondary}}; }");
    controlsLayout->addWidget(trackLabel);
    controlsLayout->addStretch();

    auto* collectiveButton = new QPushButton(QStringLiteral("Contribute"));
    collectiveButton->setAccessibleDescription(
        QStringLiteral("Open the AetherSDR sponsor page in your web browser."));
    connect(collectiveButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.aethersdr.com/#sponsor")));
    });
    controlsLayout->addWidget(collectiveButton);

    auto* muteButton = new QPushButton(QStringLiteral("Mute Music"));
    muteButton->setCheckable(true);
    muteButton->setAccessibleDescription(
        QStringLiteral("Mute or unmute the community credits music."));
    controlsLayout->addWidget(muteButton);

    auto* closeButton = new QPushButton(QStringLiteral("Close"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    controlsLayout->addWidget(closeButton);
    root->addWidget(controls);

    // QMediaPlayer (not QSoundEffect) for the looping music bed: the track is a
    // ~1.6 MB WAV, well past the small-buffer range QSoundEffect is documented
    // for, and QMediaPlayer streams it rather than decoding the whole file into
    // a single backend buffer. The source is raw PCM, so no compressed-audio
    // codec is required.
    auto* audioOutput = new QAudioOutput(this);
    audioOutput->setVolume(0.08f);
    auto* music = new QMediaPlayer(this);
    music->setAudioOutput(audioOutput);
    music->setSource(QUrl(QStringLiteral("qrc:/sounds/community-credits.wav")));
    music->setLoops(QMediaPlayer::Infinite);
    connect(muteButton, &QPushButton::toggled, this,
            [audioOutput, muteButton](bool muted) {
                audioOutput->setMuted(muted);
                muteButton->setText(muted ? QStringLiteral("Unmute Music")
                                          : QStringLiteral("Mute Music"));
            });

    fetchOpenCollectiveSupporters(this, canvas);
    music->play();
}

} // namespace AetherSDR
