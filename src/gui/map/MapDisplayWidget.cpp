#include "MapDisplayWidget.h"
#include "MapProviderNetworkAccessManager.h"
#include "CityLightsSource.h"
#include "GlobeMapView.h"
#include "WeatherRadarPlaybackTimeline.h"
#include "WeatherRadarTexture.h"
#include "core/ThemeManager.h"

#include <QCoreApplication>
#include <QAccessible>
#include <QBuffer>
#include <QImageReader>
#include <QLabel>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStackedLayout>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcWeatherRadarPlayback,
                   "aether.weather.radar.playback")
// Opt-in timing trace; do not emit per-presentation file I/O by default.
Q_LOGGING_CATEGORY(lcWeatherRadarPresentation,
                   "aether.weather.radar.presentation", QtInfoMsg)

namespace {
constexpr qint64 kMaximumTimelineBytes = 512 * 1024;
constexpr qint64 kMaximumBufferedFrameBytes = 16 * 1024 * 1024;
constexpr int kTimelineTimeoutMs = 15 * 1000;
constexpr int kMaximumBufferedFrameRequests = 4;
constexpr int kDecodedFrameLookahead = 5;
constexpr int kLoopPauseMs = 1000;
constexpr int kPlaybackPresentationIntervalMs = 16;
constexpr int kPlaybackSuspensionThresholdMs = 250;
constexpr int kPlaybackPresentationTimeoutMs = 250;
constexpr qint64 kObservedTimePerPlaybackSecondMs = 10 * 60 * 1000;
constexpr int kMinimumPlaybackSegmentMs = 450;
constexpr int kMaximumPlaybackSegmentMs = 2400;
constexpr qint64 kTimelineCacheLifetimeMs = 60 * 1000;
constexpr qint64 kFrameCacheLifetimeMs = 4 * 60 * 60 * 1000
                                       + 15 * 60 * 1000;
constexpr qint64 kMaximumFrameCacheBytes = 256 * 1024 * 1024;

}

MapDisplayWidget::MapDisplayWidget(QWidget* parent)
    : QWidget(parent)
    , m_stack(new QStackedLayout(this))
    , m_flatView(new MapView(
          this, MapView::ViewportMode::OpenGlIfAvailable))
{
    m_cityLightsSource = new CityLightsSource(this);
    connect(m_cityLightsSource, &CityLightsSource::imageChanged,
            this, &MapDisplayWidget::presentCityLights);
    connect(m_cityLightsSource, &CityLightsSource::statusChanged,
            this, &MapDisplayWidget::cityLightsStatusChanged);
    connect(m_flatView, &MapView::imageOverlayViewChanged,
            this, &MapDisplayWidget::refreshCityLightsView);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_flatView);
    connect(m_flatView, &MapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    connect(m_flatView, &MapView::weatherRadarFrameLoaded,
            this, [this](const QDateTime& frameTime) {
                if (!m_weatherRadarPlaybackClockPending
                    || !m_weatherRadarAnimating
                    || m_projectionMode != ProjectionMode::Flat) {
                    return;
                }
                startWeatherRadarPlaybackClock(frameTime);
            });
    connect(m_flatView, &MapView::weatherRadarPlaybackInvalidated,
            this, [this] {
                if (m_weatherRadarPlaybackRequested
                    && m_projectionMode == ProjectionMode::Flat) {
                    m_weatherRadarRebufferTimer->start(350);
                }
            });
    connect(m_flatView, &MapView::weatherRadarPlaybackPresented,
            this, &MapDisplayWidget::handleWeatherRadarPlaybackPresented,
            Qt::QueuedConnection);
    connect(m_flatView, &MapView::weatherRadarPlaybackFramePreloaded,
            this, [this](const QDateTime& frameTime) {
                if (!m_weatherRadarPlaybackPreloadPending
                    || !m_weatherRadarAnimating
                    || m_projectionMode != ProjectionMode::Flat
                    || m_weatherRadarFrames.size() < 2
                    || frameTime != m_weatherRadarFrames.at(1)) {
                    return;
                }
                m_weatherRadarPlaybackPreloadPending = false;
                m_weatherRadarPlaybackClock.start();
                m_weatherRadarPlaybackCadence.reset();
                m_weatherRadarPlaybackTimer->start();
                updateWeatherRadarPlayback();
            });
    m_weatherRadarTimer = new QTimer(this);
    m_weatherRadarTimer->setInterval(60 * 1000);
    connect(m_weatherRadarTimer, &QTimer::timeout, this, [this] {
        if (!m_weatherRadarVisible) {
            return;
        }
        if (m_weatherRadarAnimating) {
            // A looping movie is a rolling history, not a frozen snapshot.
            // Fetch only the small catalog while old frames keep playing.
            if (m_weatherRadarFrameIndex >= 0) {
                emit weatherRadarFrameChanged(
                    m_weatherRadarFrames.value(m_weatherRadarFrameIndex), false);
            }
            requestWeatherRadarTimeline(m_weatherRadarHistoryHours, true);
        } else if (m_weatherRadarPlaybackRequested) {
            if (m_weatherRadarTimelineReply == nullptr
                && m_weatherRadarFrames.size() < 2) {
                requestWeatherRadarTimeline(m_weatherRadarHistoryHours);
            }
        } else if (!m_weatherRadarTimelineLoading && !m_weatherRadarRebuffering) {
            applyWeatherRadarSource(
                WeatherRadarSource::currentNoaaFrame());
        }
    });
    m_weatherRadarPlaybackTimer = new QTimer(this);
    m_weatherRadarPlaybackTimer->setTimerType(Qt::PreciseTimer);
    m_weatherRadarPlaybackTimer->setInterval(
        kPlaybackPresentationIntervalMs);
    connect(m_weatherRadarPlaybackTimer, &QTimer::timeout,
            this, &MapDisplayWidget::updateWeatherRadarPlayback);
    m_weatherRadarRebufferTimer = new QTimer(this);
    // Recovery must not fire early inside the provider cooldown plus jitter.
    m_weatherRadarRebufferTimer->setTimerType(Qt::PreciseTimer);
    m_weatherRadarRebufferTimer->setSingleShot(true);
    m_weatherRadarRebufferTimer->setInterval(350);
    connect(m_weatherRadarRebufferTimer, &QTimer::timeout,
            this, &MapDisplayWidget::rebufferWeatherRadarPlayback);
    m_weatherRadarNetwork = new MapProviderNetworkAccessManager(this);
    m_weatherRadarNetwork->setTransferTimeout(kTimelineTimeoutMs);
    m_weatherRadarLoadingLabel = new QLabel(this);
    m_weatherRadarLoadingLabel->setObjectName(QStringLiteral("pskReporterWeatherRadarLoading"));
    m_weatherRadarLoadingLabel->setAccessibleName(tr("Map overlay loading status"));
    m_weatherRadarLoadingLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_weatherRadarLoadingLabel->setFocusPolicy(Qt::NoFocus);
    m_weatherRadarLoadingLabel->setAlignment(Qt::AlignCenter);
    m_weatherRadarLoadingLabel->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_weatherRadarLoadingLabel,
        "QLabel { background: {{color.background.2}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.subtle}}; border-radius: 6px; padding: 5px 10px; }");
    m_weatherRadarLoadingLabel->hide();
    m_cityLightsLoadingTimer = new QTimer(this);
    m_cityLightsLoadingTimer->setSingleShot(true);
    m_cityLightsLoadingTimer->setInterval(350);
    connect(m_cityLightsLoadingTimer, &QTimer::timeout,
            this, &MapDisplayWidget::updateOverlayLoadingStatus);
    connect(this, &MapDisplayWidget::cityLightsStatusChanged, this, [this](const QString& status) {
        m_cityLightsLoadingText = status;
        if (status.isEmpty()) {
            m_cityLightsLoadingTimer->stop();
        } else {
            // Avoid flashing a notice for quick cache hits.
            m_cityLightsLoadingTimer->start();
        }
        updateOverlayLoadingStatus();
    });
    m_weatherRadarLoadingTimer = new QTimer(this);
    m_weatherRadarLoadingTimer->setInterval(100);
    connect(m_weatherRadarLoadingTimer, &QTimer::timeout,
        this, &MapDisplayWidget::updateWeatherRadarLoadingStatus);
}


void MapDisplayWidget::setHomePosition(double lat, double lon,
                                       const QString& label, bool showMarker)
{
    m_hasHome = true;
    m_homeLat = lat;
    m_homeLon = lon;
    m_homeLabel = label;
    m_showHomeMarker = showMarker;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setHomePosition(lat, lon, label, showMarker);
        m_flatViewDirty = true;
    } else {
        m_flatView->setHomePosition(lat, lon, label, showMarker);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::setHomeSpanDegrees(double spanDegrees)
{
    m_homeSpanDegrees = spanDegrees;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setHomeSpanDegrees(spanDegrees);
        m_flatViewDirty = true;
    } else {
        m_flatView->setHomeSpanDegrees(spanDegrees);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::hasHomePosition() const
{
    return m_hasHome;
}

double MapDisplayWidget::homeLat() const
{
    return m_homeLat;
}

double MapDisplayWidget::homeLon() const
{
    return m_homeLon;
}

void MapDisplayWidget::setMarkers(const QVector<Marker>& markers)
{
    m_markers = markers;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setMarkers(markers);
        m_flatViewDirty = true;
    } else {
        m_flatView->setMarkers(markers);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::clearMarkers()
{
    m_markers.clear();
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->clearMarkers();
        m_flatViewDirty = true;
    } else {
        m_flatView->clearMarkers();
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::setPathsVisible(bool visible)
{
    m_pathsVisible = visible;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setPathsVisible(visible);
        m_flatViewDirty = true;
    } else {
        m_flatView->setPathsVisible(visible);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::pathsVisible() const
{
    return m_pathsVisible;
}

void MapDisplayWidget::setDayNightTerminatorVisible(bool visible)
{
    m_terminatorVisible = visible;
    m_cityLightsSource->setNightOnly(visible && m_projectionMode == ProjectionMode::Flat);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setDayNightTerminatorVisible(visible);
        m_flatViewDirty = true;
    } else {
        m_flatView->setDayNightTerminatorVisible(visible);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

bool MapDisplayWidget::dayNightTerminatorVisible() const
{
    return m_terminatorVisible;
}

void MapDisplayWidget::setCityLightsVisible(bool visible)
{
    m_cityLightsVisible = visible;
    updateOverlayLoadingStatus();
    m_cityLightsSource->setNightOnly(m_terminatorVisible && m_projectionMode == ProjectionMode::Flat);
    m_cityLightsSource->setEnabled(visible && isVisible());
    presentCityLights();
    refreshCityLightsView();
}

void MapDisplayWidget::setCityLightsWarmth(int percent)
{
    m_cityLightsWarmth = std::clamp(percent, 0, 100);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setCityLightsWarmth(m_cityLightsWarmth);
    } else {
        m_cityLightsSource->setWarmth(m_cityLightsWarmth);
    }
}

void MapDisplayWidget::setCityLightsFaintLights(int percent)
{
    m_cityLightsFaintLights = std::clamp(percent, 0, 100);
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setCityLightsFaintLights(m_cityLightsFaintLights);
    } else {
        m_cityLightsSource->setFaintLights(m_cityLightsFaintLights);
    }
}

void MapDisplayWidget::setCityLightsBrightness(int percent)
{
    m_cityLightsBrightness = std::clamp(percent, 0, 100);
    presentCityLights();
}

void MapDisplayWidget::presentCityLights()
{
    // Keep pixels with the bounds of the completed render, including while a
    // different viewport image is downloading or its twilight mask is pending.
    m_flatView->setCityLightsVisible(m_cityLightsVisible);
    m_flatView->setCityLightsBrightness(m_cityLightsBrightness);
    // The CPU render only carries flat-map parameters while the flat view is
    // current: in globe mode the source renders the plain original for the
    // GPU. Never hand that, or a render still catching up after a projection
    // switch, to the flat item; imageChanged delivers the correct one.
    if (m_projectionMode == ProjectionMode::Flat && !m_cityLightsSource->renderPending()) {
        m_flatView->setCityLightsImage(m_cityLightsSource->image(), m_cityLightsSource->bounds());
    }
    if (m_globeView != nullptr) {
        m_globeView->setCityLightsVisible(m_cityLightsVisible);
        m_globeView->setCityLightsBrightness(m_cityLightsBrightness);
        m_globeView->setCityLightsFaintLights(m_cityLightsFaintLights);
        m_globeView->setCityLightsWarmth(m_cityLightsWarmth);
        m_globeView->setCityLightsImage(m_cityLightsSource->originalImage(), m_cityLightsSource->originalBounds());
    }
}

void MapDisplayWidget::refreshCityLightsView()
{
    if (m_cityLightsVisible && isVisible()) {
        m_cityLightsSource->setView(weatherRadarCurrentView());
    }
}

void MapDisplayWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_cityLightsSource->setEnabled(m_cityLightsVisible);
    refreshCityLightsView();
}

void MapDisplayWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_cityLightsSource->setEnabled(false);
}

void MapDisplayWidget::setWeatherRadarVisible(bool visible)
{
    if (m_weatherRadarVisible == visible) {
        return;
    }
    m_weatherRadarVisible = visible;
    if (!visible) {
        resetWeatherRadarAnimation(false);
    } else {
        // Seed both renderers before enabling either one. This prevents an
        // inactive renderer from briefly requesting a stale historical frame
        // when radar is re-enabled after playback.
        m_weatherRadarSource = WeatherRadarSource::currentNoaaFrame();
        m_flatView->setWeatherRadarSource(m_weatherRadarSource);
        if (m_globeView != nullptr) {
            m_globeView->setWeatherRadarSource(m_weatherRadarSource);
        }
    }
    m_flatView->setWeatherRadarVisible(visible);
    if (m_globeView != nullptr) {
        m_globeView->setWeatherRadarVisible(visible);
    }
    if (visible) {
        m_weatherRadarTimer->start();
        m_weatherRadarLoadingTimer->start();
        emit weatherRadarFrameChanged(m_weatherRadarSource.frameTime(), true);
    } else {
        m_weatherRadarTimer->stop();
        m_weatherRadarLoadingTimer->stop();
        m_weatherRadarLoadingElapsed.invalidate();
        m_weatherRadarLoadingStatus.reset();
        m_weatherRadarLoadingText.clear();
        m_weatherRadarLoadingAnnouncement.clear();
        updateOverlayLoadingStatus();
    }
}

void MapDisplayWidget::updateWeatherRadarLoadingStatus()
{
    const bool playback = m_weatherRadarPlaybackRequested;
    const int pending = playback
        ? m_weatherRadarBufferQueue.size() + m_activeWeatherRadarFrameRequests
            + m_weatherRadarDownloadDecodePending.size()
        : m_projectionMode == ProjectionMode::Flat
            ? m_flatView->pendingWeatherRadarRequests()
            : m_globeView->pendingWeatherRadarRequests();
    const bool failed = playback
        ? m_weatherRadarTimelineFailed || !m_weatherRadarDownloadFailed.isEmpty()
        : m_projectionMode == ProjectionMode::Flat
            ? m_flatView->weatherRadarLoadFailed() : m_globeView->weatherRadarLoadFailed();
    const bool busy = pending > 0 || (playback && m_weatherRadarTimelineReply != nullptr);
    if (!m_weatherRadarVisible) {
        m_weatherRadarLoadingText.clear();
        m_weatherRadarLoadingAnnouncement.clear();
        updateOverlayLoadingStatus();
        m_weatherRadarLoadingStatus.reset();
        return;
    }
    if (!m_weatherRadarLoadingElapsed.isValid()) {
        m_weatherRadarLoadingElapsed.start();
    }
    const WeatherRadarLoadingStatus::State state = m_weatherRadarLoadingStatus.update(
        m_weatherRadarLoadingElapsed.elapsed(), busy, failed, pending,
        m_weatherRadarDownloadReady.size(), !playback);
    if (state == WeatherRadarLoadingStatus::State::Hidden) {
        m_weatherRadarLoadingText.clear();
        m_weatherRadarLoadingAnnouncement.clear();
        updateOverlayLoadingStatus();
        return;
    }
    const QString message = state == WeatherRadarLoadingStatus::State::Failed
        ? tr("Loading radar data failed") : tr("Loading radar…");
    const QString text = state == WeatherRadarLoadingStatus::State::Loading
            && playback && pending > 0 && !m_weatherRadarFrames.isEmpty()
        ? tr("%1 %2/%3").arg(message)
              .arg(m_weatherRadarDownloadReady.size()).arg(m_weatherRadarFrames.size())
        : message;
    m_weatherRadarLoadingText = text;
    m_weatherRadarLoadingAnnouncement = message;
    updateOverlayLoadingStatus();
}

void MapDisplayWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateOverlayLoadingStatus();
}

void MapDisplayWidget::updateOverlayLoadingStatus()
{
    QStringList rows;
    QStringList announcements;
    if (m_weatherRadarVisible && !m_weatherRadarLoadingText.isEmpty()) {
        rows.append(m_weatherRadarLoadingText);
        announcements.append(m_weatherRadarLoadingAnnouncement);
    }
    if (m_cityLightsVisible && !m_cityLightsLoadingTimer->isActive()
        && !m_cityLightsLoadingText.isEmpty()) {
        rows.append(m_cityLightsLoadingText);
        announcements.append(m_cityLightsLoadingText);
    }
    if (rows.isEmpty()) {
        m_weatherRadarLoadingLabel->hide();
        if (!m_overlayLoadingAnnouncement.isEmpty()) {
            m_overlayLoadingAnnouncement.clear();
            m_weatherRadarLoadingLabel->setAccessibleDescription(QString());
            QAccessibleEvent event(m_weatherRadarLoadingLabel, QAccessible::DescriptionChanged);
            QAccessible::updateAccessibility(&event);
        }
        return;
    }
    m_weatherRadarLoadingLabel->setText(rows.join(QLatin1Char('\n')));
    m_weatherRadarLoadingLabel->setMaximumWidth(std::max(1, width() - 32));
    m_weatherRadarLoadingLabel->adjustSize();
    m_weatherRadarLoadingLabel->move(
        (width() - m_weatherRadarLoadingLabel->width()) / 2,
        std::max(0, height() - m_weatherRadarLoadingLabel->height() - 36));
    m_weatherRadarLoadingLabel->show();
    m_weatherRadarLoadingLabel->raise();
    // Announce state transitions only, not every completed image/tile.
    const QString message = announcements.join(QLatin1Char('\n'));
    if (m_overlayLoadingAnnouncement != message) {
        m_overlayLoadingAnnouncement = message;
        m_weatherRadarLoadingLabel->setAccessibleDescription(message);
        QAccessibleEvent event(m_weatherRadarLoadingLabel, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void MapDisplayWidget::startWeatherRadarAnimation(int historyHours)
{
    if (!m_weatherRadarVisible) {
        emit weatherRadarAnimationError(
            tr("Enable weather radar before starting playback."));
        return;
    }
    resetWeatherRadarAnimation(false);
    m_weatherRadarHistoryHours = std::clamp(historyHours, 1, 4);
    m_weatherRadarPlaybackRequested = true;
    emit weatherRadarAnimationStateChanged(true);
    requestWeatherRadarTimeline(m_weatherRadarHistoryHours);
}

void MapDisplayWidget::stopWeatherRadarAnimation()
{
    resetWeatherRadarAnimation(true);
}

void MapDisplayWidget::retryWeatherRadarHistory()
{
    // A transient NOAA failure is not a user's Pause command. Keep the last
    // image, cached originals and playback intent; retry without a lasting alert.
    m_weatherRadarTimelineFailed = true;
    m_weatherRadarTimelineLoading = false;
    emit weatherRadarTimelineLoadingChanged(false);
    m_weatherRadarTimelineCache.clear();
    m_weatherRadarTimelineCachedAt = {};
    m_weatherRadarRebufferTimer->start(MapProviderRetryPolicy::kConsumerRetryMs);
    updateWeatherRadarLoadingStatus();
}

void MapDisplayWidget::setWeatherRadarPlaybackSpeed(int speedPercent)
{
    const int speed = weatherRadarPlaybackSpeedPercent(speedPercent);
    if (m_weatherRadarPlaybackSpeedPercent == speed) {
        return;
    }
    m_weatherRadarPlaybackSpeedPercent = speed;
    if (!m_weatherRadarAnimating) {
        return; // Used on the next start, including an in-flight history load.
    }
    const QVector<int> durations = weatherRadarPlaybackScaledDurations(
        m_weatherRadarSegmentDurationsMs.mid(0, weatherRadarPlaybackFrameCount() - 1),
        speed);
    const qint64 elapsed = weatherRadarPlaybackElapsedAtSpeed(
        m_weatherRadarPlaybackCadence.requestedElapsedMs(),
        m_weatherRadarActiveSegmentDurationsMs, durations, kLoopPauseMs);
    m_weatherRadarActiveSegmentDurationsMs = durations;
    if (m_weatherRadarPlaybackClock.isValid()) {
        // Only the clock changes. Keep decoded images, GPU uploads, frame
        // identities, download requests and the visible observation intact.
        m_weatherRadarPlaybackCadence.reset(elapsed, m_weatherRadarPlaybackClock.elapsed());
    }
}

void MapDisplayWidget::requestWeatherRadarTimeline(int historyHours, bool background)
{
    if (background && (m_weatherRadarTimelineReply != nullptr
        || !m_weatherRadarAnimating || !m_weatherRadarNetworkRequestsComplete
        || m_weatherRadarBufferFinalizationPending)) {
        return;
    }
    cancelWeatherRadarTimelineRequest();
    if (!background) {
        m_weatherRadarTimelineLoading = true;
        emit weatherRadarTimelineLoadingChanged(true);
    }

    pruneWeatherRadarCache();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!background && !m_weatherRadarTimelineCache.isEmpty()
        && m_weatherRadarTimelineCachedAt.isValid()
        && m_weatherRadarTimelineCachedAt <= now
        && m_weatherRadarTimelineCachedAt.msecsTo(now)
               <= kTimelineCacheLifetimeMs) {
        const QVector<WeatherRadarObservation> observations =
            WeatherRadarSource::parseNoaaTimeline(
                m_weatherRadarTimelineCache, historyHours);
        if (observations.size() >= 2) {
            m_weatherRadarFrames.clear();
            m_weatherRadarFrameSampleTimes.clear();
            m_weatherRadarFrameRasterIds.clear();
            m_weatherRadarFrames.reserve(observations.size());
            m_weatherRadarFrameSampleTimes.reserve(observations.size());
            m_weatherRadarFrameRasterIds.reserve(observations.size());
            for (const WeatherRadarObservation& observation
                 : observations) {
                m_weatherRadarFrames.append(observation.frameTime);
                m_weatherRadarFrameSampleTimes.append(
                    observation.sampleTime);
                m_weatherRadarFrameRasterIds.append(
                    observation.rasterIds);
            }
            bufferWeatherRadarFrames();
            return;
        }
        m_weatherRadarTimelineCache.clear();
        m_weatherRadarTimelineCachedAt = {};
    }

    QNetworkRequest request(WeatherRadarSource::noaaTimelineUrl());
    request.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("AetherSDR/%1 (https://github.com/aethersdr/AetherSDR)")
            .arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    QNetworkReply* reply = m_weatherRadarNetwork->get(request);
    m_weatherRadarTimelineReply = reply;
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
                if (received > kMaximumTimelineBytes) {
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, historyHours, background] {
                if (m_weatherRadarTimelineReply != reply) {
                    reply->deleteLater();
                    return;
                }
                m_weatherRadarTimelineReply = nullptr;
                QByteArray payload;
                if (reply->error() == QNetworkReply::NoError
                    && reply->bytesAvailable() <= kMaximumTimelineBytes) {
                    payload = reply->read(kMaximumTimelineBytes + 1);
                }
                const QString error = reply->errorString();
                reply->deleteLater();
                if (payload.isEmpty()
                    || payload.size() > kMaximumTimelineBytes) {
                    if (background) {
                        // Slow/offline NOAA must not stop or erase the movie.
                        // The one-minute timer retries; displayed age stays honest.
                        qCWarning(lcWeatherRadarPlayback)
                            << "NOAA history refresh failed; retaining loaded frames:" << error;
                        m_weatherRadarTimelineFailed = true;
                        return;
                    }
                    qCWarning(lcWeatherRadarPlayback) << "NOAA history load failed; retrying:" << error;
                    retryWeatherRadarHistory();
                    return;
                }
                if (background) {
                    const QVector<WeatherRadarObservation> observations =
                        WeatherRadarSource::parseNoaaTimeline(payload, historyHours);
                    if (!m_weatherRadarAnimating || observations.size() < 2) {
                        m_weatherRadarTimelineFailed = true;
                        return; // Preserve the last valid catalog on bad responses.
                    }
                    m_weatherRadarTimelineFailed = false;
                    m_weatherRadarTimelineCache = payload;
                    m_weatherRadarTimelineCachedAt = QDateTime::currentDateTimeUtc();
                    appendWeatherRadarObservations(observations);
                    return;
                }
                m_weatherRadarTimelineCache = payload;
                m_weatherRadarTimelineCachedAt =
                    QDateTime::currentDateTimeUtc();
                useWeatherRadarTimeline(payload, historyHours);
            });
}

void MapDisplayWidget::appendWeatherRadarObservations(
    const QVector<WeatherRadarObservation>& observations)
{
    if (m_weatherRadarFrames.isEmpty()) {
        return;
    }
    const int oldCount = m_weatherRadarFrames.size();
    const QDateTime newest = *std::max_element(
        m_weatherRadarFrames.cbegin(), m_weatherRadarFrames.cend());
    QSet<QDateTime> existing(m_weatherRadarFrames.cbegin(), m_weatherRadarFrames.cend());
    // NOAA's ImageServer catalog is a rolling window, not an append-only log.
    // A locked raster that has rolled off returns HTTP 200 with a transparent
    // PNG. Keeping it for our selected 4-hour window turned every loop restart
    // into a blank frame after a zoom fetched that expired observation again.
    // A valid full catalog retires missing observations, even with no new scan.
    QSet<QDateTime> available;
    for (const WeatherRadarObservation& observation : observations) {
        available.insert(observation.frameTime);
    }
    // A complete catalog is also authority to stop retrying rolled-off data.
    m_weatherRadarRetryFrames.intersect(available);
    bool retired = false;
    for (const QDateTime& frame : std::as_const(m_weatherRadarFrames)) {
        if (!available.contains(frame) && !m_weatherRadarExpiredFrames.contains(frame)) {
            m_weatherRadarExpiredFrames.insert(frame);
            retired = true;
        }
    }
    for (const WeatherRadarObservation& observation : observations) {
        if (existing.contains(observation.frameTime)
            || m_weatherRadarExpiredFrames.contains(observation.frameTime)
            || (observation.frameTime <= newest
                && !m_weatherRadarRetryFrames.contains(observation.frameTime))) {
            continue; // Keep existing immutable raster identities and decoded images.
        }
        const WeatherRadarSource source = WeatherRadarSource::historicalNoaaFrame(
            observation.frameTime, observation.sampleTime, observation.rasterIds);
        const QUrl url = source.imageUrl(m_weatherRadarPlaybackRequestBounds,
                                         m_weatherRadarPlaybackSize);
        if (!url.isValid()) {
            continue;
        }
        const int index = m_weatherRadarFrames.size();
        m_weatherRadarFrames.append(observation.frameTime);
        existing.insert(observation.frameTime);
        m_weatherRadarFrameSampleTimes.append(observation.sampleTime);
        m_weatherRadarFrameRasterIds.append(observation.rasterIds);
        m_weatherRadarFrameUrls.append(url);
        m_weatherRadarFrameRequestGeometry.append(m_weatherRadarRequestedView);
        m_weatherRadarFrameCacheKeys.append(QString{});
        m_weatherRadarFrameBounds.append(QRectF{});
        m_weatherRadarBufferedBytes.append(QByteArray{});
        m_weatherRadarBufferQueue.append(index);
    }
    if (m_weatherRadarFrames.size() == oldCount && !retired) {
        return;
    }
    qCInfo(lcWeatherRadarPlayback) << "NOAA history refresh appended"
        << m_weatherRadarFrames.size() - oldCount << "observations; newest"
        << *std::max_element(m_weatherRadarFrames.cbegin(), m_weatherRadarFrames.cend());
    // Do not change active indexes, dwell times, geometry, texture residency,
    // or the cadence mid-loop. The existing bounded queue fetches only new
    // images; finalization adopts/prunes them at the next loop boundary.
    m_weatherRadarNetworkRequestsComplete = false;
    if (m_weatherRadarBufferQueue.isEmpty()) {
        finalizeWeatherRadarBuffering();
    } else {
        requestNextWeatherRadarBufferedFrames();
    }
}

bool MapDisplayWidget::useWeatherRadarTimeline(
    const QByteArray& payload, int historyHours)
{
    const QVector<WeatherRadarObservation> observations =
        WeatherRadarSource::parseNoaaTimeline(payload, historyHours);
    if (observations.size() < 2) {
        retryWeatherRadarHistory();
        return false;
    }
    m_weatherRadarTimelineFailed = false;
    m_weatherRadarFrames.clear();
    m_weatherRadarFrameSampleTimes.clear();
    m_weatherRadarFrameRasterIds.clear();
    m_weatherRadarFrames.reserve(observations.size());
    m_weatherRadarFrameSampleTimes.reserve(observations.size());
    m_weatherRadarFrameRasterIds.reserve(observations.size());
    for (const WeatherRadarObservation& observation : observations) {
        m_weatherRadarFrames.append(observation.frameTime);
        m_weatherRadarFrameSampleTimes.append(observation.sampleTime);
        m_weatherRadarFrameRasterIds.append(observation.rasterIds);
    }
    bufferWeatherRadarFrames();
    return true;
}

WeatherRadarViewGeometry MapDisplayWidget::weatherRadarCurrentView() const
{
    if (m_projectionMode == ProjectionMode::Flat) {
        return {WeatherRadarSource::conventionalBoundsFromQgv(m_flatView->weatherRadarPlaybackBounds()),
                m_flatView->weatherRadarPlaybackSize()};
    }
    const QRectF bounds = m_globeView->weatherRadarPlaybackBounds();
    return {bounds, m_globeView->weatherRadarPlaybackSize(bounds)};
}

QRectF MapDisplayWidget::weatherRadarRendererBounds(const QRectF& bounds) const
{
    // Reflection in the equator is its own inverse. Cache geometry is always
    // north-positive; only QGeoView needs north-negative scene coordinates.
    return m_projectionMode == ProjectionMode::Flat
        ? WeatherRadarSource::conventionalBoundsFromQgv(bounds) : bounds;
}

void MapDisplayWidget::bufferWeatherRadarFrames(bool retainPlayback)
{
    ++m_weatherRadarBufferGeneration;
    if (!retainPlayback) {
        m_weatherRadarBufferedBytes.clear();
        m_weatherRadarFrameCacheKeys.clear();
        m_weatherRadarFrameBounds.clear();
        m_weatherRadarDecodedImages.clear();
        m_weatherRadarActiveSegmentDurationsMs.clear();
        m_weatherRadarPlayableFrameCount = 0;
        m_weatherRadarNetworkBufferComplete = false;
    }
    m_weatherRadarBufferedBytes.resize(m_weatherRadarFrames.size());
    m_weatherRadarFrameUrls.clear();
    m_weatherRadarFrameUrls.resize(m_weatherRadarFrames.size());
    m_weatherRadarFrameRequestGeometry.resize(m_weatherRadarFrames.size());
    m_weatherRadarFrameCacheKeys.resize(m_weatherRadarFrames.size());
    m_weatherRadarFrameBounds.resize(m_weatherRadarFrames.size());
    if (!retainPlayback) {
        m_weatherRadarSegmentDurationsMs.clear();
    }
    m_weatherRadarDecodePending.clear();
    m_weatherRadarDownloadDecodePending.clear();
    m_weatherRadarDownloadReady.clear();
    m_weatherRadarDownloadFailed.clear();
    m_weatherRadarDetailRefresh = retainPlayback;
    m_weatherRadarNetworkRequestsComplete = false;
    m_weatherRadarBufferFinalizationPending = false;
    m_weatherRadarBufferQueue.clear();
    if (m_weatherRadarFrameSampleTimes.size()
            != m_weatherRadarFrames.size()
        || m_weatherRadarFrameRasterIds.size()
            != m_weatherRadarFrames.size()) {
        resetWeatherRadarAnimation(true);
        emit weatherRadarAnimationError(tr(
            "The NOAA radar timeline contains invalid intervals."));
        return;
    }
    const WeatherRadarViewGeometry view = weatherRadarCurrentView();
    m_weatherRadarRequestedView = weatherRadarPaddedView(view,
        m_projectionMode == ProjectionMode::Flat ? 2048 : 4096);
    m_weatherRadarPlaybackRequestBounds = m_weatherRadarRequestedView.bounds;
    m_weatherRadarPlaybackBounds = weatherRadarRendererBounds(m_weatherRadarPlaybackRequestBounds);
    m_weatherRadarPlaybackSize = m_weatherRadarRequestedView.size;
    for (int index = 0; index < m_weatherRadarFrames.size(); ++index) {
        const WeatherRadarSource source =
            WeatherRadarSource::historicalNoaaFrame(
                m_weatherRadarFrames.at(index),
                m_weatherRadarFrameSampleTimes.at(index),
                m_weatherRadarFrameRasterIds.at(index));
        QUrl url = source.imageUrl(
            m_weatherRadarPlaybackRequestBounds,
            m_weatherRadarPlaybackSize);
        WeatherRadarViewGeometry requestGeometry = m_weatherRadarRequestedView;
        // Reuse a same-observation image that already covers this view at
        // sufficient resolution, even if its old viewport/export URL differs.
        // Never reuse another timestamp (or a recycled NOAA raster identity).
        QString reusableKey;
        qint64 reusablePixels = std::numeric_limits<qint64>::max();
        for (auto cached = m_weatherRadarFrameCache.cbegin();
             cached != m_weatherRadarFrameCache.cend(); ++cached) {
            const qint64 pixels = qint64(cached->geometry.size.width()) * cached->geometry.size.height();
            if (cached->sourceId == source.frameId() && !cached->bytes.isEmpty()
                && cached->geometry.covers(view) && pixels < reusablePixels) {
                reusableKey = cached.key();
                reusablePixels = pixels;
            }
        }
        if (!reusableKey.isEmpty()) {
            url = QUrl(reusableKey);
            requestGeometry = m_weatherRadarFrameCache.value(reusableKey).geometry;
        }
        if (!url.isValid()) {
            resetWeatherRadarAnimation(true);
            emit weatherRadarAnimationError(
                tr("The radar playback view could not be prepared."));
            return;
        }
        const QString cacheKey = url.toString(QUrl::FullyEncoded);
        qCDebug(lcWeatherRadarPlayback)
            << "radar export" << m_weatherRadarBufferGeneration << index
            << m_weatherRadarFrames.at(index) << url;
        m_weatherRadarFrameUrls[index] = url;
        m_weatherRadarFrameRequestGeometry[index] = requestGeometry;
        if (m_weatherRadarExpiredFrames.contains(m_weatherRadarFrames.at(index))) {
            continue; // Keep old pixels until boundary retirement; never re-export.
        }
        if (retainPlayback && m_weatherRadarFrameCacheKeys.at(index) == cacheKey
            && !m_weatherRadarBufferedBytes.at(index).isEmpty()) {
            m_weatherRadarDownloadReady.insert(index);
            continue;
        }
        m_weatherRadarBufferQueue.append(index);
    }
    if (!retainPlayback) {
        m_weatherRadarSegmentDurationsMs =
            weatherRadarPlaybackSegmentDurations(
                m_weatherRadarFrames, kObservedTimePerPlaybackSecondMs,
                kMinimumPlaybackSegmentMs, kMaximumPlaybackSegmentMs);
    }
    // Appended retries can precede the active prefix chronologically. Keep
    // that prefix's timing immutable during zoom/download; finalization sorts
    // all recovered observations and recomputes timing only at loop restart.
    if (!retainPlayback && m_weatherRadarSegmentDurationsMs.size()
        != m_weatherRadarFrames.size() - 1) {
        resetWeatherRadarAnimation(true);
        emit weatherRadarAnimationError(tr(
            "The NOAA radar timeline contains invalid timestamps."));
        return;
    }
    if (m_weatherRadarBufferQueue.isEmpty()) {
        finishWeatherRadarDownloadBatch();
        return;
    }
    tryStartWeatherRadarBufferedPrefix();
    requestNextWeatherRadarBufferedFrames();
}

void MapDisplayWidget::requestNextWeatherRadarBufferedFrames()
{
    while (m_activeWeatherRadarFrameRequests + m_weatherRadarDownloadDecodePending.size()
               < kMaximumBufferedFrameRequests
           && !m_weatherRadarBufferQueue.isEmpty()) {
        const int index = weatherRadarTakePriorityFrame(m_weatherRadarBufferQueue,
            m_weatherRadarFrameIndex, m_weatherRadarFrames.size());
        if (m_weatherRadarExpiredFrames.contains(m_weatherRadarFrames.at(index))) {
            continue;
        }
        const QUrl url = m_weatherRadarFrameUrls.value(index);
        if (!url.isValid()) {
            resetWeatherRadarAnimation(true);
            emit weatherRadarAnimationError(
                tr("The radar playback view could not be prepared."));
            return;
        }
        const QString key = url.toString(QUrl::FullyEncoded);
        const auto cached = m_weatherRadarFrameCache.constFind(key);
        if (cached != m_weatherRadarFrameCache.cend()) {
            const CachedWeatherRadarFrame frame = *cached;
            if (!frame.decodedImage.isNull()) {
                acceptWeatherRadarDownload(m_weatherRadarBufferGeneration, index, key, frame);
            } else {
                decodeWeatherRadarDownload(index, frame.bytes, key, frame.geometry, true);
            }
            continue;
        }
        // The selected URL may cover a different cached viewport. Even if
        // that cache entry was evicted while queued, a re-fetch still uses
        // that URL's original geometry, NEVER the current batch/camera bounds.
        const WeatherRadarViewGeometry geometry = m_weatherRadarFrameRequestGeometry.at(index);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
            QStringLiteral(
                "AetherSDR/%1 (https://github.com/aethersdr/AetherSDR)")
                .arg(QCoreApplication::applicationVersion()));
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        qCDebug(lcWeatherRadarPlayback) << "radar download" << index
            << m_weatherRadarFrames.at(index) << url;
        QNetworkReply* reply = m_weatherRadarNetwork->get(request);
        m_weatherRadarFrameReplies.insert(reply);
        ++m_activeWeatherRadarFrameRequests;
        connect(reply, &QNetworkReply::downloadProgress, reply,
                [reply](qint64 received, qint64) {
                    if (received > kMaximumBufferedFrameBytes) {
                        reply->abort();
                    }
                });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, index, key, geometry] {
                    if (!m_weatherRadarFrameReplies.remove(reply)) {
                        reply->deleteLater();
                        return;
                    }
                    m_activeWeatherRadarFrameRequests = std::max(
                        0, m_activeWeatherRadarFrameRequests - 1);
                    QByteArray bytes;
                    if (reply->error() == QNetworkReply::NoError
                        && reply->bytesAvailable()
                            <= kMaximumBufferedFrameBytes) {
                        bytes = reply->read(kMaximumBufferedFrameBytes + 1);
                    }
                    const QString error = reply->errorString();
                    reply->deleteLater();
                    const bool pngHeader = bytes.size() >= 8
                        && bytes.startsWith("\x89PNG\r\n\x1a\n");
                    if (!pngHeader
                        || bytes.size() > kMaximumBufferedFrameBytes) {
                        qCWarning(lcWeatherRadarPlayback)
                            << "Skipping unavailable NOAA radar frame"
                            << m_weatherRadarFrames.value(index) << error;
                        m_weatherRadarDownloadFailed.insert(index);
                    } else {
                        decodeWeatherRadarDownload(index, bytes, key, geometry);
                    }
                    requestNextWeatherRadarBufferedFrames();
                    finishWeatherRadarDownloadBatch();
                });
    }
    finishWeatherRadarDownloadBatch();
}

void MapDisplayWidget::decodeWeatherRadarDownload(int index, const QByteArray& bytes,
    const QString& key, const WeatherRadarViewGeometry& geometry, bool trustedCache)
{
    const int generation = m_weatherRadarBufferGeneration;
    CachedWeatherRadarFrame frame;
    frame.bytes = bytes;
    frame.frameTime = m_weatherRadarFrames.at(index);
    frame.sourceId = WeatherRadarSource::historicalNoaaFrame(frame.frameTime,
        m_weatherRadarFrameSampleTimes.at(index), m_weatherRadarFrameRasterIds.at(index)).frameId();
    frame.geometry = geometry;
    m_weatherRadarDownloadDecodePending.insert(index);
    using DecodedFrame = std::pair<QImage, bool>;
    auto* watcher = new QFutureWatcher<DecodedFrame>(this);
    connect(watcher, &QFutureWatcher<DecodedFrame>::finished, this,
        [this, watcher, generation, index, key, frame, trustedCache]() mutable {
            const DecodedFrame decoded = watcher->result();
            frame.decodedImage = decoded.first;
            watcher->deleteLater();
            if (generation != m_weatherRadarBufferGeneration) {
                return; // Zoom/stop invalidates delivery, not the visible image.
            }
            if (!frame.decodedImage.isNull() && !decoded.second && !trustedCache) {
                // Do not guess whether transparency is clear weather or an
                // expired raster. Check the exact locked IDs AFTER the export.
                // Hold this bounded work slot and the prior visible original.
                validateWeatherRadarEmptyFrame(generation, index, key, frame);
                return;
            }
            m_weatherRadarDownloadDecodePending.remove(index);
            if (frame.decodedImage.isNull() || frame.decodedImage.size() != frame.geometry.size) {
                m_weatherRadarFrameCache.remove(key);
                m_weatherRadarDownloadFailed.insert(index);
                qCWarning(lcWeatherRadarPlayback) << "Invalid radar image; keeping prior coverage" << key;
            } else {
                acceptWeatherRadarDownload(generation, index, key, frame);
            }
            requestNextWeatherRadarBufferedFrames();
        });
    watcher->setFuture(QtConcurrent::run([bytes, geometry, trustedCache]() -> DecodedFrame {
        QBuffer buffer;
        buffer.setData(bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "PNG");
        if (reader.size() != geometry.size || geometry.size.width() > 4096
            || geometry.size.height() > 4096) {
            return {}; // Reject oversized/misregistered images before allocation.
        }
        const QImage image = WeatherRadarTexture::prepareImage(reader.read());
        // Alpha scan runs off the GUI thread, alongside PNG decode. Prepared
        // RGBA8888 has alpha in byte 3 on every platform; no endian assumptions.
        if (!trustedCache) {
            for (int y = 0; y < image.height(); ++y) {
                const uchar* row = image.constScanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    if (row[x * 4 + 3] != 0) {
                        return {image, true};
                    }
                }
            }
        }
        return {image, trustedCache};
    }));
}

void MapDisplayWidget::validateWeatherRadarEmptyFrame(int generation, int index,
    const QString& key, const CachedWeatherRadarFrame& frame)
{
    const QVector<qint64> rasterIds = m_weatherRadarFrameRasterIds.at(index);
    QNetworkRequest request(WeatherRadarSource::noaaRasterAvailabilityUrl(rasterIds));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(kTimelineTimeoutMs);
    QNetworkReply* reply = m_weatherRadarNetwork->get(request);
    m_weatherRadarFrameReplies.insert(reply);
    // This request occupies its existing download/decode slot; do not count
    // it twice or release it early and build an unbounded validation backlog.
    connect(reply, &QNetworkReply::downloadProgress, reply,
        [reply](qint64 received, qint64) {
            if (received > kMaximumTimelineBytes) {
                reply->abort();
            }
        });
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, index, key, frame, rasterIds] {
            if (!m_weatherRadarFrameReplies.remove(reply)) {
                reply->deleteLater();
                return;
            }
            QByteArray payload;
            if (reply->error() == QNetworkReply::NoError
                && reply->bytesAvailable() <= kMaximumTimelineBytes) {
                payload = reply->read(kMaximumTimelineBytes + 1);
            }
            reply->deleteLater();
            if (generation != m_weatherRadarBufferGeneration) {
                return;
            }
            m_weatherRadarDownloadDecodePending.remove(index);
            const std::optional<bool> available =
                WeatherRadarSource::parseNoaaRasterAvailability(payload, rasterIds);
            if (available.value_or(false)) {
                acceptWeatherRadarDownload(generation, index, key, frame);
            } else if (available.has_value()) {
                // A confirmed expired raster is normal rolling-history upkeep,
                // not a failed download. Keep its old pixels until retirement
                // at loop wrap, without a failure popup or a pointless retry.
                qCInfo(lcWeatherRadarPlayback) << "Retiring expired NOAA observation" << frame.frameTime;
                m_weatherRadarExpiredFrames.insert(frame.frameTime);
                m_weatherRadarRetryFrames.remove(frame.frameTime);
                m_weatherRadarDownloadFailed.remove(index);
                m_weatherRadarTimelineCache.clear();
            } else {
                // A failed verification is not evidence of expiration. Keep
                // the prior original and retry; never cache this ambiguous PNG.
                m_weatherRadarDownloadFailed.insert(index);
            }
            requestNextWeatherRadarBufferedFrames();
        });
}

void MapDisplayWidget::acceptWeatherRadarDownload(int generation, int index,
    const QString& key, const CachedWeatherRadarFrame& frame)
{
    if (generation != m_weatherRadarBufferGeneration || index < 0
        || index >= m_weatherRadarFrames.size() || frame.decodedImage.isNull()
        || frame.frameTime != m_weatherRadarFrames.at(index)
        || key != m_weatherRadarFrameUrls.at(index).toString(QUrl::FullyEncoded)
        || m_weatherRadarExpiredFrames.contains(frame.frameTime)) {
        return;
    }
    CachedWeatherRadarFrame cached = frame;
    cached.lastAccessMs = QDateTime::currentMSecsSinceEpoch();
    m_weatherRadarFrameCache.insert(key, cached);
    // Atomic replacement: the original pixels, their observation, and their
    // ORIGINAL geographic bounds travel together. Other frames keep their old
    // bounds until their own replacement completes. Do not use the batch's new
    // camera bounds when displaying a retained frame (the flying-radar bug).
    m_weatherRadarBufferedBytes[index] = frame.bytes;
    m_weatherRadarFrameCacheKeys[index] = key;
    m_weatherRadarFrameBounds[index] = frame.geometry.bounds;
    m_weatherRadarDecodedImages.insert(index, frame.decodedImage);
    m_weatherRadarDownloadReady.insert(index);
    m_weatherRadarDownloadFailed.remove(index);
    m_weatherRadarRetryFrames.remove(frame.frameTime);
    if (!m_weatherRadarAnimating) {
        // A complete original is useful immediately, even when the next image
        // is slow. Never try to render a partial PNG/download or invent data.
        const bool accepted = m_projectionMode == ProjectionMode::Flat
            ? m_flatView->showWeatherRadarPlaybackFrame(frame.decodedImage, frame.frameTime,
                  weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(index)))
            : m_globeView->showWeatherRadarPlaybackFrame(frame.decodedImage, frame.frameTime,
                  weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(index)));
        if (accepted) {
            emit weatherRadarFrameChanged(frame.frameTime, false);
        }
        tryStartWeatherRadarBufferedPrefix();
    } else {
        preloadNextWeatherRadarFrame();
        pruneWeatherRadarDecodedImages(m_weatherRadarFrameIndex, weatherRadarPlaybackFrameCount());
    }
    pruneWeatherRadarCache();
}

void MapDisplayWidget::finishWeatherRadarDownloadBatch()
{
    if (m_weatherRadarNetworkRequestsComplete || !m_weatherRadarBufferQueue.isEmpty()
        || m_activeWeatherRadarFrameRequests > 0 || !m_weatherRadarDownloadDecodePending.isEmpty()) {
        return;
    }
    m_weatherRadarNetworkRequestsComplete = true;
    if (m_weatherRadarDetailRefresh) {
        m_weatherRadarDetailRefresh = false;
        // No timeline/cadence reset on a zoom. Failed detail keeps that frame's
        // older image; retry only the missing upgrades after a bounded delay.
        if (!m_weatherRadarDownloadFailed.isEmpty()) {
            m_weatherRadarRebufferTimer->start(MapProviderRetryPolicy::kConsumerRetryMs);
        }
        if (m_weatherRadarPlayableFrameCount != m_weatherRadarFrames.size()
            || std::any_of(m_weatherRadarFrames.cbegin(), m_weatherRadarFrames.cend(),
                [this](const QDateTime& frame) { return m_weatherRadarExpiredFrames.contains(frame); })) {
            // A catalog append may have been in flight when the view changed.
            // Its completed originals still join the movie at loop restart.
            finalizeWeatherRadarBuffering();
        }
    } else {
        finalizeWeatherRadarBuffering();
    }
}

void MapDisplayWidget::tryStartWeatherRadarBufferedPrefix()
{
    if (m_weatherRadarNetworkBufferComplete
        || !m_weatherRadarPlaybackRequested
        || m_weatherRadarFrames.size() < 2) {
        return;
    }
    const int required = std::min(
        2,
        static_cast<int>(m_weatherRadarFrames.size()));
    for (int index = 0; index < required; ++index) {
        if (index >= m_weatherRadarBufferedBytes.size()
            || m_weatherRadarBufferedBytes.at(index).isEmpty()) {
            return;
        }
    }
    // The remaining immutable exports continue downloading in the
    // background. Queue order keeps them ahead of playback, while the exact
    // pair-residency gate freezes on the last good image if a slow connection
    // ever catches up with this buffer.
    m_weatherRadarPlayableFrameCount = required;
    m_weatherRadarNetworkBufferComplete = true;
    for (int index = 0; index < required; ++index) {
        scheduleWeatherRadarDecode(index);
    }
    tryStartWeatherRadarPlayback();
}

void MapDisplayWidget::finalizeWeatherRadarBuffering()
{
    m_weatherRadarNetworkRequestsComplete = true;
    if (m_weatherRadarAnimating) {
        const auto end = m_weatherRadarBufferedBytes.cbegin()
            + std::min(m_weatherRadarPlayableFrameCount,
                       static_cast<int>(m_weatherRadarBufferedBytes.size()));
        if (std::any_of(m_weatherRadarBufferedBytes.cbegin(), end,
                       [](const QByteArray& bytes) { return bytes.isEmpty(); })) {
            // A failed decode inside the active prefix makes its loop end
            // unreachable. Skip that failed observation now, retaining the
            // last complete texture; do not wait forever at its boundary.
            applyFinalizedWeatherRadarBuffering();
            return;
        }
        // Compact only at loop restart so indexes cannot change underneath
        // the observation currently on screen.
        m_weatherRadarBufferFinalizationPending = true;
        return;
    }
    applyFinalizedWeatherRadarBuffering();
}

void MapDisplayWidget::applyFinalizedWeatherRadarBuffering()
{
    QVector<QDateTime> usableFrames;
    QVector<QDateTime> usableSampleTimes;
    QVector<QVector<qint64>> usableRasterIds;
    QVector<QByteArray> usableBytes;
    QVector<QUrl> usableUrls;
    QVector<QString> usableCacheKeys;
    QVector<QRectF> usableBounds;
    QVector<WeatherRadarViewGeometry> usableRequestGeometry;
    QVector<int> usableOldIndexes;
    usableFrames.reserve(m_weatherRadarFrames.size());
    usableSampleTimes.reserve(m_weatherRadarFrames.size());
    usableRasterIds.reserve(m_weatherRadarFrames.size());
    usableBytes.reserve(m_weatherRadarFrames.size());
    usableUrls.reserve(m_weatherRadarFrames.size());
    usableCacheKeys.reserve(m_weatherRadarFrames.size());
    usableOldIndexes.reserve(m_weatherRadarFrames.size());
    // Select the cutoff from a successfully downloaded observation, so a
    // failed future image cannot discard the still-useful older movie.
    QDateTime newestUsable;
    for (int index = 0; index < m_weatherRadarFrames.size(); ++index) {
        if (!m_weatherRadarBufferedBytes.value(index).isEmpty()) {
            newestUsable = std::max(newestUsable, m_weatherRadarFrames.at(index));
        }
    }
    const QDateTime oldestKept = newestUsable.addSecs(-m_weatherRadarHistoryHours * 3600);
    QVector<int> orderedIndexes;
    orderedIndexes.reserve(m_weatherRadarFrames.size());
    for (int index = 0; index < m_weatherRadarFrames.size(); ++index) {
        orderedIndexes.append(index);
    }
    std::sort(orderedIndexes.begin(), orderedIndexes.end(), [this](int a, int b) {
        return m_weatherRadarFrames.at(a) < m_weatherRadarFrames.at(b);
    });
    for (int index : std::as_const(orderedIndexes)) {
        const QDateTime frame = m_weatherRadarFrames.at(index);
        if (!m_weatherRadarExpiredFrames.contains(frame) && frame >= oldestKept
            && m_weatherRadarBufferedBytes.value(index).isEmpty()) {
            // A network/decode failure is not expiration. Preserve its identity
            // outside the playable list so a fresh catalog can retry it later.
            m_weatherRadarRetryFrames.insert(frame);
        }
        if (index >= m_weatherRadarBufferedBytes.size()
            || m_weatherRadarBufferedBytes.at(index).isEmpty()
            || m_weatherRadarExpiredFrames.contains(m_weatherRadarFrames.at(index))
            || m_weatherRadarFrames.at(index) < oldestKept) {
            continue;
        }
        usableFrames.append(m_weatherRadarFrames.at(index));
        usableSampleTimes.append(
            m_weatherRadarFrameSampleTimes.at(index));
        usableRasterIds.append(
            m_weatherRadarFrameRasterIds.at(index));
        usableBytes.append(m_weatherRadarBufferedBytes.at(index));
        usableUrls.append(m_weatherRadarFrameUrls.at(index));
        usableRequestGeometry.append(m_weatherRadarFrameRequestGeometry.at(index));
        usableCacheKeys.append(m_weatherRadarFrameCacheKeys.at(index));
        usableBounds.append(m_weatherRadarFrameBounds.at(index));
        usableOldIndexes.append(static_cast<int>(index));
    }
    if (usableFrames.size() < 2) {
        // Retain successful originals for the next attempt, including a
        // complete preview; never clear the overlay because NOAA timed out.
        m_weatherRadarAnimating = false;
        m_weatherRadarPlaybackTimer->stop();
        retryWeatherRadarHistory();
        return;
    }
    const bool layoutChanged = usableFrames != m_weatherRadarFrames;
    if (layoutChanged) {
        QHash<int, QImage> remappedImages;
        for (int newIndex = 0; newIndex < usableOldIndexes.size(); ++newIndex) {
            const int oldIndex = usableOldIndexes.at(newIndex);
            if (m_weatherRadarDecodedImages.contains(oldIndex)) {
                remappedImages.insert(
                    newIndex, m_weatherRadarDecodedImages.value(oldIndex));
            }
        }
        ++m_weatherRadarBufferGeneration;
        m_weatherRadarDecodedImages = std::move(remappedImages);
        m_weatherRadarDecodePending.clear();
    }
    m_weatherRadarFrames = std::move(usableFrames);
    m_weatherRadarFrameSampleTimes = std::move(usableSampleTimes);
    m_weatherRadarFrameRasterIds = std::move(usableRasterIds);
    m_weatherRadarBufferedBytes = std::move(usableBytes);
    m_weatherRadarFrameUrls = std::move(usableUrls);
    m_weatherRadarFrameRequestGeometry = std::move(usableRequestGeometry);
    m_weatherRadarFrameCacheKeys = std::move(usableCacheKeys);
    m_weatherRadarFrameBounds = std::move(usableBounds);
    m_weatherRadarDownloadReady.clear();
    m_weatherRadarDownloadFailed.clear();
    for (int newIndex = 0; newIndex < usableOldIndexes.size(); ++newIndex) {
        if (m_weatherRadarFrameCacheKeys.at(newIndex)
            == m_weatherRadarFrameUrls.at(newIndex).toString(QUrl::FullyEncoded)) {
            m_weatherRadarDownloadReady.insert(newIndex);
        } else {
            m_weatherRadarDownloadFailed.insert(newIndex);
        }
    }
    m_weatherRadarBufferFinalizationPending = false;
    if (!m_weatherRadarRetryFrames.isEmpty()) {
        m_weatherRadarRebufferTimer->start(MapProviderRetryPolicy::kConsumerRetryMs);
    }
    if (m_weatherRadarFrames.size() < 2) {
        resetWeatherRadarAnimation(true);
        emit weatherRadarAnimationError(tr(
            "NOAA did not return enough usable radar frames for playback."));
        return;
    }
    m_weatherRadarSegmentDurationsMs =
        weatherRadarPlaybackSegmentDurations(
            m_weatherRadarFrames, kObservedTimePerPlaybackSecondMs,
            kMinimumPlaybackSegmentMs, kMaximumPlaybackSegmentMs);
    if (m_weatherRadarSegmentDurationsMs.size()
        != m_weatherRadarFrames.size() - 1) {
        resetWeatherRadarAnimation(true);
        emit weatherRadarAnimationError(tr(
            "The NOAA radar timeline contains invalid timestamps."));
        return;
    }
    // Adopt the compacted original-observation list only at loop restart.
    m_weatherRadarPlayableFrameCount = m_weatherRadarFrames.size();
    m_weatherRadarActiveSegmentDurationsMs = weatherRadarPlaybackScaledDurations(
        m_weatherRadarSegmentDurationsMs, m_weatherRadarPlaybackSpeedPercent);
    if (m_weatherRadarAnimating) {
        m_weatherRadarPlaybackCadence.rebaseElapsed(0);
        m_weatherRadarFrameIndex = -1;
        m_weatherRadarPresentedFromIndex = -1;
        m_weatherRadarPresentedToIndex = -1;
        m_weatherRadarPresentedImageKey = 0;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (int index = 0; index < m_weatherRadarFrameCacheKeys.size(); ++index) {
        auto cached = m_weatherRadarFrameCache.find(
            m_weatherRadarFrameCacheKeys.at(index));
        if (cached == m_weatherRadarFrameCache.end()) {
            continue;
        }
        cached->lastAccessMs = nowMs;
        if (!cached->decodedImage.isNull()) {
            m_weatherRadarDecodedImages.insert(index, cached->decodedImage);
        }
    }
    m_weatherRadarNetworkBufferComplete = true;
    const int decodeCount = std::min(
        kDecodedFrameLookahead, static_cast<int>(m_weatherRadarFrames.size()));
    for (int index = 0; index < decodeCount; ++index) {
        scheduleWeatherRadarDecode(index);
    }
    if (!m_weatherRadarAnimating) {
        tryStartWeatherRadarPlayback();
    }
}

void MapDisplayWidget::scheduleWeatherRadarDecode(int index)
{
    if (index < 0 || index >= m_weatherRadarBufferedBytes.size()
        || m_weatherRadarBufferedBytes.at(index).isEmpty()
        || m_weatherRadarDecodedImages.contains(index)
        || m_weatherRadarDecodePending.contains(index)) {
        return;
    }
    const auto cached = m_weatherRadarFrameCache.constFind(
        m_weatherRadarFrameCacheKeys.value(index));
    if (cached != m_weatherRadarFrameCache.cend() && !cached->decodedImage.isNull()) {
        m_weatherRadarDecodedImages.insert(index, cached->decodedImage);
        return;
    }
    const int generation = m_weatherRadarBufferGeneration;
    const QString cacheKey = m_weatherRadarFrameCacheKeys.value(index);
    const QByteArray bytes = m_weatherRadarBufferedBytes.at(index);
    m_weatherRadarDecodePending.insert(index);
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, generation, index, cacheKey] {
                const QImage image = watcher->result();
                watcher->deleteLater();
                if (generation != m_weatherRadarBufferGeneration) {
                    return;
                }
                m_weatherRadarDecodePending.remove(index);
                if (cacheKey != m_weatherRadarFrameCacheKeys.value(index)) {
                    return; // A sharper same-time image superseded this decode.
                }
                if (image.isNull()) {
                    handleWeatherRadarDecodeFailure(index);
                    return;
                }
                m_weatherRadarDecodedImages.insert(index, image);
                if (index < m_weatherRadarFrameCacheKeys.size()) {
                    auto cached = m_weatherRadarFrameCache.find(
                        m_weatherRadarFrameCacheKeys.at(index));
                    if (cached != m_weatherRadarFrameCache.end()) {
                        cached->decodedImage = image;
                        cached->lastAccessMs =
                            QDateTime::currentMSecsSinceEpoch();
                    }
                    pruneWeatherRadarCache();
                }
                preloadNextWeatherRadarFrame();
                tryStartWeatherRadarPlayback();
            });
    watcher->setFuture(QtConcurrent::run([bytes] {
        QImage image;
        image.loadFromData(bytes, "PNG");
        // Perform the upload-friendly pixel conversion off the GUI thread.
        // Both QPixmap and QOpenGLTexture otherwise pay this cost at the
        // exact moment a new animation frame is presented.
        return WeatherRadarTexture::prepareImage(image);
    }));
}

void MapDisplayWidget::handleWeatherRadarDecodeFailure(int index)
{
    if (index < 0 || index >= m_weatherRadarFrames.size()) {
        return;
    }
    qCWarning(lcWeatherRadarPlayback)
        << "Skipping undecodable NOAA radar frame"
        << m_weatherRadarFrames.at(index);
    if (index < m_weatherRadarFrameCacheKeys.size()) {
        m_weatherRadarFrameCache.remove(
            m_weatherRadarFrameCacheKeys.at(index));
    }
    if (index < m_weatherRadarBufferedBytes.size()) {
        m_weatherRadarBufferedBytes[index].clear();
    }
    // A corrupt image cannot ever be presented. Once downloads have settled,
    // compact it immediately rather than waiting for a loop boundary playback
    // cannot reach. The current renderer image remains visible throughout.
    if (!m_weatherRadarBufferQueue.isEmpty()
        || m_activeWeatherRadarFrameRequests > 0) {
        return;
    }
    m_weatherRadarNetworkRequestsComplete = true;
    applyFinalizedWeatherRadarBuffering();
}

int MapDisplayWidget::weatherRadarPlaybackFrameCount() const
{
    if (m_weatherRadarPlayableFrameCount >= 2) {
        return std::min(
            m_weatherRadarPlayableFrameCount,
            static_cast<int>(m_weatherRadarFrames.size()));
    }
    return m_weatherRadarFrames.size();
}

void MapDisplayWidget::ensureWeatherRadarDecodeAhead(int index)
{
    const int frameCount = weatherRadarPlaybackFrameCount();
    if (frameCount < 2) {
        return;
    }
    for (int offset = 0; offset < kDecodedFrameLookahead; ++offset) {
        scheduleWeatherRadarDecode(
            (index + offset) % frameCount);
    }
    pruneWeatherRadarDecodedImages(index, frameCount);
}

void MapDisplayWidget::pruneWeatherRadarDecodedImages(
    int index, int frameCount)
{
    if (frameCount < 2) {
        return;
    }
    QSet<int> keep = weatherRadarPlaybackDecodedWindow(
        index, frameCount, kDecodedFrameLookahead);
    const QList<int> decoded = m_weatherRadarDecodedImages.keys();
    for (const int decodedIndex : decoded) {
        if (!keep.contains(decodedIndex)) {
            m_weatherRadarDecodedImages.remove(decodedIndex);
        }
    }
}


void MapDisplayWidget::tryStartWeatherRadarPlayback()
{
    if (!m_weatherRadarNetworkBufferComplete
        || !m_weatherRadarPlaybackRequested || m_weatherRadarAnimating
        || m_weatherRadarFrames.size() < 2) {
        return;
    }
    if (m_weatherRadarPlayableFrameCount < 2) {
        m_weatherRadarPlayableFrameCount = std::min(
            kDecodedFrameLookahead,
            static_cast<int>(m_weatherRadarFrames.size()));
    }
    const int decodedRequired = std::min(
        2, m_weatherRadarPlayableFrameCount);
    for (int index = 0; index < decodedRequired; ++index) {
        if (!m_weatherRadarDecodedImages.contains(index)) {
            scheduleWeatherRadarDecode(index);
            if (!m_weatherRadarDecodedImages.contains(index)) {
                return;
            }
        }
    }

    // No analysis prerequisite: freeze only the timing of the loaded prefix.
    m_weatherRadarActiveSegmentDurationsMs = weatherRadarPlaybackScaledDurations(
        m_weatherRadarSegmentDurationsMs.mid(0, m_weatherRadarPlayableFrameCount - 1),
        m_weatherRadarPlaybackSpeedPercent);
    m_weatherRadarTimelineLoading = false;
    emit weatherRadarTimelineLoadingChanged(false);
    m_weatherRadarAnimating = true;
    m_weatherRadarFrameIndex = -1;
    m_weatherRadarPresentedFromIndex = -1;
    m_weatherRadarPresentedToIndex = -1;
    m_weatherRadarPresentedImageKey = 0;
    m_weatherRadarPlaybackClockPending = false;
    m_weatherRadarPlaybackPreloadPending = false;
    m_weatherRadarTimer->start(); // Keep the catalog fresh throughout playback.
    m_weatherRadarRebuffering = false;
    // A preview may already be visible. Do not clear it to start the clock;
    // normal presentation acknowledgements hold it until frame zero is ready.
    m_weatherRadarPlaybackClock.start();
    m_weatherRadarPlaybackCadence.reset();
    m_weatherRadarPlaybackTimer->start();
    updateWeatherRadarPlayback();
    ensureWeatherRadarDecodeAhead(1);
}

void MapDisplayWidget::rebufferWeatherRadarPlayback()
{
    if (!m_weatherRadarPlaybackRequested) {
        return;
    }
    if (!m_weatherRadarAnimating && (m_weatherRadarTimelineFailed
        || m_weatherRadarFrames.size() < 2)) {
        requestWeatherRadarTimeline(m_weatherRadarHistoryHours);
        return;
    }
    const WeatherRadarViewGeometry view = weatherRadarCurrentView();
    if (m_weatherRadarDownloadFailed.isEmpty() && m_weatherRadarRequestedView.covers(view)) {
        if (m_weatherRadarAnimating && !m_weatherRadarRetryFrames.isEmpty()) {
            requestWeatherRadarTimeline(m_weatherRadarHistoryHours, true);
        }
        return; // Margin/resolution already covers the camera; keep useful work.
    }
    cancelWeatherRadarTimelineRequest();
    cancelWeatherRadarFrameRequests();
    m_weatherRadarRebufferTimer->stop();
    bufferWeatherRadarFrames(m_weatherRadarAnimating);
}

void MapDisplayWidget::cancelWeatherRadarFrameRequests()
{
    const QSet<QNetworkReply*> replies = m_weatherRadarFrameReplies;
    m_weatherRadarFrameReplies.clear();
    m_activeWeatherRadarFrameRequests = 0;
    m_weatherRadarBufferQueue.clear();
    for (QNetworkReply* reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void MapDisplayWidget::startWeatherRadarPlaybackClock(
    const QDateTime& frameTime)
{
    if (!m_weatherRadarPlaybackClockPending || !m_weatherRadarAnimating
        || m_weatherRadarFrames.isEmpty()
        || frameTime != m_weatherRadarFrames.first()) {
        return;
    }
    m_weatherRadarPlaybackClockPending = false;
    m_weatherRadarPlaybackPreloadPending = true;
    preloadNextWeatherRadarFrame();
}

void MapDisplayWidget::updateWeatherRadarPlayback()
{
    const int frameCount = weatherRadarPlaybackFrameCount();
    if (!m_weatherRadarAnimating || !m_weatherRadarPlaybackClock.isValid()
        || frameCount < 2) {
        return;
    }
    const std::optional<qint64> candidateElapsedMs =
        m_weatherRadarPlaybackCadence.timerTick(
            m_weatherRadarPlaybackClock.elapsed(),
            kPlaybackSuspensionThresholdMs,
            kPlaybackPresentationIntervalMs,
            kPlaybackPresentationTimeoutMs);
    if (!candidateElapsedMs.has_value()) {
        return;
    }
    tryPresentWeatherRadarElapsed(*candidateElapsedMs);
}

bool MapDisplayWidget::tryPresentWeatherRadarElapsed(
    qint64 candidateElapsedMs)
{
    int frameCount = weatherRadarPlaybackFrameCount();
    if (!m_weatherRadarAnimating || !m_weatherRadarPlaybackClock.isValid()
        || frameCount < 2) {
        return false;
    }
    candidateElapsedMs = weatherRadarObservationClampToBoundary(
        m_weatherRadarPlaybackCadence.presentedElapsedMs(),
        candidateElapsedMs,
        m_weatherRadarActiveSegmentDurationsMs,
        kLoopPauseMs);
    WeatherRadarPlaybackPosition position =
        weatherRadarObservationPlaybackPosition(
            candidateElapsedMs, frameCount,
            m_weatherRadarActiveSegmentDurationsMs,
            kLoopPauseMs);
    if (!position.valid) {
        return false;
    }
    const bool loopRestart = position.fromIndex == 0
        && position.toIndex == 0
        && m_weatherRadarPresentedFromIndex == frameCount - 1
        && m_weatherRadarPresentedToIndex == frameCount - 1;
    if (m_weatherRadarBufferFinalizationPending && loopRestart) {
        applyFinalizedWeatherRadarBuffering();
        frameCount = weatherRadarPlaybackFrameCount();
        if (frameCount < 2 || !m_weatherRadarAnimating
            || !m_weatherRadarPlaybackClock.isValid()) {
            return false;
        }
    }
    if (loopRestart) {
        // Never interpolate backward from newest to oldest.
        m_weatherRadarPlaybackCadence.rebaseElapsed(0);
        candidateElapsedMs = 0;
        position = weatherRadarObservationPlaybackPosition(
            0, frameCount, m_weatherRadarActiveSegmentDurationsMs, kLoopPauseMs);
    }
    if (position.fromIndex == position.toIndex
        && position.fromIndex == m_weatherRadarPresentedFromIndex
        && position.toIndex == m_weatherRadarPresentedToIndex
        && m_weatherRadarDecodedImages.value(position.fromIndex).cacheKey()
            == m_weatherRadarPresentedImageKey) {
        // Every observation remains unchanged during its dwell (one second
        // for the final image). There is no new state to paint, and some
        // graphics-scene backends coalesce identical repaint requests. Advance
        // the cadence immediately instead of waiting for an acknowledgement
        // that an unchanged image may never generate. Actual frame changes
        // remain presentation-gated.
        const quint64 holdSequence =
            m_weatherRadarPlaybackCadence.beginPresentation(
                candidateElapsedMs);
        if (holdSequence == 0) {
            return false;
        }
        m_weatherRadarPlaybackCadence.completePresentation(holdSequence);
        return true;
    }
    const quint64 presentationSequence =
        m_weatherRadarPlaybackCadence.beginPresentation(
            candidateElapsedMs);
    if (presentationSequence == 0) {
        return false;
    }
    if (!presentWeatherRadarFrame(position.fromIndex, presentationSequence)) {
        m_weatherRadarPlaybackCadence.rejectPresentation(
            presentationSequence);
        qCDebug(lcWeatherRadarPresentation)
            << "radar buffering" << presentationSequence
            << "wallMs" << m_weatherRadarPlaybackClock.elapsed()
            << "pair" << position.fromIndex << position.toIndex;
        return false;
    }
    qCDebug(lcWeatherRadarPresentation)
        << "radar request" << presentationSequence
        << "wallMs" << m_weatherRadarPlaybackClock.elapsed()
        << "movieMs" << candidateElapsedMs
        << "pair" << position.fromIndex << position.toIndex
        << "phase" << position.progress
        << "frame" << m_weatherRadarFrames.value(position.fromIndex)
        << "image" << m_weatherRadarPresentedImageKey
        << "size" << m_weatherRadarDecodedImages.value(position.fromIndex).size()
        << "globe" << (m_projectionMode == ProjectionMode::Globe);
    return true;
}

void MapDisplayWidget::handleWeatherRadarPlaybackPresented(
    quint64 presentationSequence)
{
    // Acknowledgements control ownership of the in-flight image, not movie
    // speed. Drain the latest target time without throwing away elapsed ticks.
    const std::optional<qint64> deferredElapsedMs =
        m_weatherRadarPlaybackCadence.completePresentation(
            presentationSequence);
    qCDebug(lcWeatherRadarPresentation)
        << "radar ack" << presentationSequence
        << "wallMs" << m_weatherRadarPlaybackClock.elapsed()
        << "movieMs" << m_weatherRadarPlaybackCadence.presentedElapsedMs();
    if (deferredElapsedMs.has_value()) {
        tryPresentWeatherRadarElapsed(*deferredElapsedMs);
    }
}

bool MapDisplayWidget::presentWeatherRadarFrame(
    int index, quint64 presentationSequence)
{
    if (index < 0 || index >= weatherRadarPlaybackFrameCount()) {
        return false;
    }
    if (!m_weatherRadarDecodedImages.contains(index)) {
        ensureWeatherRadarDecodeAhead(index);
        return false;
    }
    const QDateTime frameTime = m_weatherRadarFrames.at(index);
    const QImage image = m_weatherRadarDecodedImages.value(index);
    // ONE NOAA observation, never a target image or an interpolated fraction.
    const bool accepted = m_projectionMode == ProjectionMode::Flat
        ? m_flatView->showWeatherRadarPlaybackFrame(
              image, frameTime, weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(index)))
        : m_globeView->showWeatherRadarPlaybackFrame(
              image, frameTime, weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(index)));
    if (!accepted) {
        return false; // Keep the preceding original visible while uploading.
    }
    m_weatherRadarSource = WeatherRadarSource::historicalNoaaFrame(
        frameTime, m_weatherRadarFrameSampleTimes.at(index),
        m_weatherRadarFrameRasterIds.at(index));
    auto cached = m_weatherRadarFrameCache.find(m_weatherRadarFrameCacheKeys.at(index));
    if (cached != m_weatherRadarFrameCache.end()) {
        cached->lastAccessMs = QDateTime::currentMSecsSinceEpoch();
    }
    m_weatherRadarFrameIndex = index;
    m_weatherRadarPresentedFromIndex = index;
    m_weatherRadarPresentedToIndex = index;
    m_weatherRadarPresentedImageKey = image.cacheKey();
    if (m_projectionMode == ProjectionMode::Flat) {
        m_flatView->acknowledgeWeatherRadarPlaybackFrame(presentationSequence);
    } else {
        m_globeView->acknowledgeWeatherRadarPlaybackFrame(presentationSequence);
    }
    emit weatherRadarFrameChanged(frameTime, false);
    ensureWeatherRadarDecodeAhead(index);
    preloadNextWeatherRadarFrame();
    return true;
}

void MapDisplayWidget::preloadNextWeatherRadarFrame()
{
    const int frameCount = weatherRadarPlaybackFrameCount();
    if (!m_weatherRadarAnimating || frameCount < 2) {
        return;
    }
    const int nextIndex =
        (m_weatherRadarFrameIndex + 1) % frameCount;
    if (!m_weatherRadarDecodedImages.contains(nextIndex)) {
        return;
    }
    if (m_projectionMode == ProjectionMode::Flat) {
        m_flatView->preloadWeatherRadarPlaybackFrame(
            m_weatherRadarDecodedImages.value(nextIndex),
            m_weatherRadarFrames.at(nextIndex),
            weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(nextIndex)));
    } else if (m_globeView != nullptr) {
        m_globeView->preloadWeatherRadarPlaybackFrame(
            m_weatherRadarDecodedImages.value(nextIndex),
            m_weatherRadarFrames.at(nextIndex),
            weatherRadarRendererBounds(m_weatherRadarFrameBounds.at(nextIndex)));
    }
}

void MapDisplayWidget::applyWeatherRadarSource(
    const WeatherRadarSource& source)
{
    m_weatherRadarSource = source;
    if (m_projectionMode == ProjectionMode::Globe) {
        ensureGlobeView();
        m_globeView->setWeatherRadarSource(source);
        m_flatViewDirty = true;
    } else {
        m_flatView->setWeatherRadarSource(source);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::cancelWeatherRadarTimelineRequest()
{
    if (m_weatherRadarTimelineReply == nullptr) {
        return;
    }
    disconnect(m_weatherRadarTimelineReply, nullptr, this, nullptr);
    m_weatherRadarTimelineReply->abort();
    m_weatherRadarTimelineReply->deleteLater();
    m_weatherRadarTimelineReply = nullptr;
}

void MapDisplayWidget::resetWeatherRadarAnimation(bool returnToLive)
{
    const bool wasActive = m_weatherRadarPlaybackRequested
        || m_weatherRadarAnimating
        || m_weatherRadarTimelineLoading;
    cancelWeatherRadarTimelineRequest();
    cancelWeatherRadarFrameRequests();
    ++m_weatherRadarBufferGeneration;
    m_weatherRadarPlaybackTimer->stop();
    m_weatherRadarRebufferTimer->stop();
    m_weatherRadarFrames.clear();
    m_weatherRadarFrameSampleTimes.clear();
    m_weatherRadarFrameRasterIds.clear();
    m_weatherRadarBufferedBytes.clear();
    m_weatherRadarFrameBounds.clear();
    m_weatherRadarDownloadDecodePending.clear();
    m_weatherRadarDownloadReady.clear();
    m_weatherRadarDownloadFailed.clear();
    m_weatherRadarRequestedView = {};
    m_weatherRadarExpiredFrames.clear();
    m_weatherRadarRetryFrames.clear();
    m_weatherRadarDetailRefresh = false;
    m_weatherRadarTimelineFailed = false;
    m_weatherRadarLoadingStatus.reset();
    m_weatherRadarPresentedImageKey = 0;
    m_weatherRadarFrameUrls.clear();
    m_weatherRadarFrameCacheKeys.clear();
    m_weatherRadarFrameRequestGeometry.clear();
    m_weatherRadarSegmentDurationsMs.clear();
    m_weatherRadarDecodedImages.clear();
    m_weatherRadarDecodePending.clear();
    m_weatherRadarActiveSegmentDurationsMs.clear();
    m_weatherRadarPlayableFrameCount = 0;
    m_weatherRadarNetworkBufferComplete = false;
    m_weatherRadarNetworkRequestsComplete = false;
    m_weatherRadarBufferFinalizationPending = false;
    m_weatherRadarFrameIndex = -1;
    m_weatherRadarPresentedFromIndex = -1;
    m_weatherRadarPresentedToIndex = -1;
    m_weatherRadarPlaybackClock.invalidate();
    m_weatherRadarPlaybackCadence.reset();
    m_weatherRadarPlaybackClockPending = false;
    m_weatherRadarPlaybackPreloadPending = false;
    m_weatherRadarPlaybackRequested = false;
    m_weatherRadarAnimating = false;
    m_weatherRadarRebuffering = false;
    m_flatView->clearWeatherRadarPlayback();
    if (m_globeView != nullptr) {
        m_globeView->clearWeatherRadarPlayback();
    }
    if (m_weatherRadarTimelineLoading) {
        m_weatherRadarTimelineLoading = false;
        emit weatherRadarTimelineLoadingChanged(false);
    }
    if (wasActive) {
        emit weatherRadarAnimationStateChanged(false);
    }
    if (returnToLive && m_weatherRadarVisible) {
        const WeatherRadarSource live =
            WeatherRadarSource::currentNoaaFrame();
        applyWeatherRadarSource(live);
        m_weatherRadarTimer->start();
        emit weatherRadarFrameChanged(live.frameTime(), true);
    }
}

void MapDisplayWidget::pruneWeatherRadarCache()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 oldestFrameMs = nowMs - kFrameCacheLifetimeMs;
    qint64 compressedBytes = 0;
    qint64 decodedBytes = 0;
    QVector<QString> keys;
    keys.reserve(m_weatherRadarFrameCache.size());
    for (auto iterator = m_weatherRadarFrameCache.begin();
         iterator != m_weatherRadarFrameCache.end();) {
        if (!iterator->frameTime.isValid()
            || iterator->frameTime.toMSecsSinceEpoch() < oldestFrameMs) {
            iterator = m_weatherRadarFrameCache.erase(iterator);
            continue;
        }
        compressedBytes += iterator->bytes.size();
        decodedBytes += static_cast<qint64>(
            iterator->decodedImage.sizeInBytes());
        keys.append(iterator.key());
        ++iterator;
    }
    std::sort(keys.begin(), keys.end(), [this](const QString& lhs,
                                                const QString& rhs) {
        return m_weatherRadarFrameCache.value(lhs).lastAccessMs
             < m_weatherRadarFrameCache.value(rhs).lastAccessMs;
    });
    // Decoded RGBA images are the expensive tier. Drop only those first so a
    // replay can cheaply decode the still-cached NOAA PNG instead of going
    // back to the network. Immutable URL keys include the source observation
    // and export geometry, preventing stale reuse after a zoom.
    qint64 totalBytes = compressedBytes + decodedBytes;
    for (const QString& key : std::as_const(keys)) {
        if (totalBytes <= kMaximumFrameCacheBytes) {
            break;
        }
        auto found = m_weatherRadarFrameCache.find(key);
        if (found == m_weatherRadarFrameCache.end()) {
            continue;
        }
        const qint64 imageBytes = static_cast<qint64>(
            found->decodedImage.sizeInBytes());
        totalBytes -= imageBytes;
        found->decodedImage = {};
    }
    for (const QString& key : std::as_const(keys)) {
        if (totalBytes <= kMaximumFrameCacheBytes) {
            break;
        }
        const auto found = m_weatherRadarFrameCache.find(key);
        if (found == m_weatherRadarFrameCache.end()) {
            continue;
        }
        const qint64 entryBytes = found->bytes.size()
            + static_cast<qint64>(found->decodedImage.sizeInBytes());
        totalBytes -= entryBytes;
        m_weatherRadarFrameCache.erase(found);
    }
}

void MapDisplayWidget::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    m_legendEntries = entries;
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->setLegend(entries);
        m_flatViewDirty = true;
    } else {
        m_flatView->setLegend(entries);
        m_globeViewDirty = m_globeView != nullptr;
    }
}

void MapDisplayWidget::ensureGlobeView()
{
    if (m_globeView != nullptr) {
        return;
    }
    m_globeView = new GlobeMapView(this);
    m_stack->addWidget(m_globeView);
    connect(m_globeView, &GlobeMapView::markerClicked,
            this, &MapDisplayWidget::markerClicked);
    connect(m_globeView, &GlobeMapView::rendererUnavailable,
            this, &MapDisplayWidget::handleGlobeUnavailable);
    connect(m_globeView, &GlobeMapView::imageOverlayViewChanged,
            this, &MapDisplayWidget::refreshCityLightsView);
    connect(m_globeView, &GlobeMapView::weatherRadarFrameLoaded,
            this, [this](const QDateTime& frameTime) {
                if (!m_weatherRadarPlaybackClockPending
                    || !m_weatherRadarAnimating
                    || m_projectionMode != ProjectionMode::Globe) {
                    return;
                }
                startWeatherRadarPlaybackClock(frameTime);
            });
    connect(m_globeView,
            &GlobeMapView::weatherRadarPlaybackFramePreloaded,
            this, [this](const QDateTime& frameTime) {
                if (!m_weatherRadarPlaybackPreloadPending
                    || !m_weatherRadarAnimating
                    || m_weatherRadarFrames.size() < 2
                    || frameTime != m_weatherRadarFrames.at(1)) {
                    return;
                }
                m_weatherRadarPlaybackPreloadPending = false;
                m_weatherRadarPlaybackClock.start();
                m_weatherRadarPlaybackCadence.reset();
                m_weatherRadarPlaybackTimer->start();
                updateWeatherRadarPlayback();
            });
    connect(m_globeView, &GlobeMapView::weatherRadarPlaybackViewChanged,
            this, [this] {
                if (m_weatherRadarPlaybackRequested
                    && m_projectionMode == ProjectionMode::Globe) {
                    m_weatherRadarRebufferTimer->start(350);
                }
            });
    connect(m_globeView, &GlobeMapView::weatherRadarPlaybackPresented,
            this, &MapDisplayWidget::handleWeatherRadarPlaybackPresented,
            Qt::QueuedConnection);
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_globeView->setWeatherRadarSource(m_weatherRadarSource);
    m_globeView->setWeatherRadarVisible(m_weatherRadarVisible);
    m_globeView->setLegend(m_legendEntries);
    m_globeView->setMarkers(m_markers);
    m_globeViewDirty = false;
}

void MapDisplayWidget::synchronizeFlatView()
{
    if (!m_flatViewDirty) {
        return;
    }
    // Synchronization happens while this renderer is hidden. Disable paths
    // first so applying home and marker changes cannot rebuild an obsolete
    // path batch before the final visibility state is restored.
    m_flatView->setPathsVisible(false);
    m_flatView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_flatView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                    m_showHomeMarker);
    }
    m_flatView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_flatView->setWeatherRadarSource(m_weatherRadarSource);
    m_flatView->setWeatherRadarVisible(m_weatherRadarVisible);
    m_flatView->setLegend(m_legendEntries);
    m_flatView->setMarkers(m_markers);
    m_flatView->setPathsVisible(m_pathsVisible);
    m_flatViewDirty = false;
}

void MapDisplayWidget::synchronizeGlobeView()
{
    ensureGlobeView();
    if (!m_globeViewDirty) {
        return;
    }
    m_globeView->setHomeSpanDegrees(m_homeSpanDegrees);
    if (m_hasHome) {
        m_globeView->setHomePosition(m_homeLat, m_homeLon, m_homeLabel,
                                     m_showHomeMarker);
    }
    m_globeView->setPathsVisible(m_pathsVisible);
    m_globeView->setDayNightTerminatorVisible(m_terminatorVisible);
    m_globeView->setWeatherRadarSource(m_weatherRadarSource);
    m_globeView->setWeatherRadarVisible(m_weatherRadarVisible);
    m_globeView->setLegend(m_legendEntries);
    m_globeView->setMarkers(m_markers);
    m_globeViewDirty = false;
}

void MapDisplayWidget::setProjectionMode(ProjectionMode mode)
{
    if (mode == ProjectionMode::Globe && !m_globeAvailable) {
        return;
    }
    if (m_projectionMode == mode) {
        return;
    }
    if (mode == ProjectionMode::Globe) {
        synchronizeGlobeView();
    } else {
        synchronizeFlatView();
    }
    m_projectionMode = mode;
    m_stack->setCurrentWidget(mode == ProjectionMode::Globe
                                  ? static_cast<QWidget*>(m_globeView)
                                  : static_cast<QWidget*>(m_flatView));
    if (m_weatherRadarAnimating) {
        // The movie belongs to this facade, not to either renderer. Retain
        // frames, speed, history, and movie position across projection changes.
        // Retire any queued acknowledgement from the hidden renderer, then
        // force the same original through the new renderer's residency gate.
        // Bounds remain canonical north-positive until the draw call: reusing
        // QGeoView's reflected bounds on the globe relocates all the weather.
        const qint64 elapsed = m_weatherRadarPlaybackCadence.requestedElapsedMs();
        m_weatherRadarPlaybackCadence.reset(elapsed, m_weatherRadarPlaybackClock.elapsed());
        m_weatherRadarPresentedImageKey = 0;
        tryPresentWeatherRadarElapsed(elapsed);
    }
    if (m_weatherRadarPlaybackRequested && m_weatherRadarFrames.size() >= 2) {
        m_weatherRadarRebufferTimer->start(350);
    }
    // Flat view uses CPU processing; the globe shades the original on the GPU.
    m_cityLightsSource->setNightOnly(m_terminatorVisible && mode == ProjectionMode::Flat);
    m_cityLightsSource->setFaintLights(mode == ProjectionMode::Flat ? m_cityLightsFaintLights : 0);
    m_cityLightsSource->setWarmth(mode == ProjectionMode::Flat ? m_cityLightsWarmth : 0);
    presentCityLights();
    refreshCityLightsView();
    emit projectionModeChanged(mode);
}

bool MapDisplayWidget::globeAvailable() const
{
    return m_globeAvailable;
}

void MapDisplayWidget::handleGlobeUnavailable(const QString& reason)
{
    if (!m_globeAvailable) {
        return;
    }
    m_globeAvailable = false;
    m_globeUnavailableReason = reason;
    if (m_projectionMode == ProjectionMode::Globe) {
        setProjectionMode(ProjectionMode::Flat);
    }
    emit globeAvailabilityChanged(false, reason);
}

void MapDisplayWidget::resetToHome()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->resetToHome();
    } else {
        m_flatView->resetToHome();
    }
}

void MapDisplayWidget::zoomIn()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->zoomIn();
    } else {
        m_flatView->zoomIn();
    }
}

void MapDisplayWidget::zoomOut()
{
    if (m_projectionMode == ProjectionMode::Globe) {
        m_globeView->zoomOut();
    } else {
        m_flatView->zoomOut();
    }
}

} // namespace AetherSDR
