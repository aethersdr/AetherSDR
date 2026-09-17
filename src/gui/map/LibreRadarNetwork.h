#pragma once
#include <QRectF>
#include <QSize>
#include <QVector>
#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QObject>
#include <memory>

namespace AetherSDR {
struct LibreRadarTile {
    QUrl url;
    QRectF destination;
};
// Bounded XYZ plan; preserves world wrapping and north-up pixel placement.
QVector<LibreRadarTile> libreRadarTiles(const QRectF& bounds, const QSize& size, qint64 timestamp);

// An injectable transport keeps placement, cache, cancellation and errors
// testable without sockets. The supplied network must outlive the renderer.
class LibreRadarRenderer final : public QObject {
public:
    explicit LibreRadarRenderer(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~LibreRadarRenderer() override;
    QNetworkReply* request(const QNetworkRequest& request, QObject* parent);
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
QNetworkReply* requestLibreRadar(const QNetworkRequest& request, QObject* parent);
}
