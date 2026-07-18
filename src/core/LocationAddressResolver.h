#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkReply;

namespace AetherSDR {

// Small, provider-isolated reverse-geocoding client for operator-triggered
// location views.  It performs at most one request per second, keeps only an
// in-memory cache (precise station coordinates are never persisted), and lets
// operators replace the service without rebuilding via
// AETHER_NOMINATIM_BASE_URL.  The default is the public OSM Nominatim service;
// callers must display its OpenStreetMap attribution beside returned data.
class LocationAddressResolver : public QObject {
    Q_OBJECT

public:
    explicit LocationAddressResolver(QObject* parent = nullptr);

    void resolve(double latitude, double longitude);

    // Public for the boundary-validation unit test. Empty means the response
    // was malformed or did not contain a usable display_name.
    static QString parseDisplayName(const QByteArray& payload);

signals:
    void resolved(double latitude, double longitude, const QString& address);
    void failed(double latitude, double longitude, const QString& reason);

private:
    void startRequest(double latitude, double longitude);
    void schedulePending();
    static QString cacheKey(double latitude, double longitude);

    QNetworkAccessManager m_network;
    QHash<QString, QString> m_cache;
    QElapsedTimer m_lastRequest;
    QTimer m_rateLimitTimer;
    QNetworkReply* m_reply{nullptr};
    bool m_hasPending{false};
    double m_pendingLatitude{0.0};
    double m_pendingLongitude{0.0};
};

} // namespace AetherSDR
