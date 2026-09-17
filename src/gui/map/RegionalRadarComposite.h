#pragma once
#include "WeatherRadarSource.h"
#include <QImage>
#include <QNetworkRequest>
#include <optional>
#include <functional>
class QNetworkAccessManager;
class QNetworkReply;
class QObject;
namespace AetherSDR {
// A composite clock selects original observations at or before each time,
// with a bounded age. It never interpolates weather or uses future scans.
std::optional<WeatherRadarObservation> radarObservationAt(
    const QVector<WeatherRadarObservation>& observations, const QDateTime& time);
QImage composeRegionalRadar(const QVector<QImage>& images, const QSize& size);
QNetworkReply* requestRegionalRadar(const QNetworkRequest& request, QObject* parent);
QString regionalRadarStatus(int enabledProviders);
// Same production controller with an injected transport, owned by context.
std::function<QNetworkReply*(const QNetworkRequest&, QObject*)> regionalRadarRequester(
    QNetworkAccessManager* network, QObject* context);
}
