#pragma once

#include <QPointF>
#include <QQuaternion>
#include <QSizeF>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace AetherSDR {

// Projection-independent interaction state for the globe. Rebuilding the
// quaternion from explicit latitude, longitude and roll keeps ordinary drag
// gestures from accumulating unintended axial tilt.
class GlobeNavigation final {
public:
    // Approximate angular scale for keyboard navigation. Pointer dragging
    // uses applyScreenDrag to account for latitude, roll and grab location.
    static float screenTrackedDegreesPerPixel(
        float cameraDistance, int viewportHeight,
        float verticalFieldOfViewDegrees)
    {
        if (viewportHeight <= 0 || cameraDistance <= 1.0F
            || verticalFieldOfViewDegrees <= 0.0F
            || verticalFieldOfViewDegrees >= 180.0F) {
            return 0.0F;
        }
        const double halfFieldOfViewRadians =
            static_cast<double>(verticalFieldOfViewDegrees)
            * std::numbers::pi / 360.0;
        const double focalLengthPixels =
            static_cast<double>(viewportHeight)
            / (2.0 * std::tan(halfFieldOfViewRadians));
        const double radiansPerPixel =
            (static_cast<double>(cameraDistance) - 1.0)
            / focalLengthPixels;
        return static_cast<float>(
            radiansPerPixel * 180.0 / std::numbers::pi);
    }

    void setCenter(double latitude, double longitude)
    {
        m_latitude = std::clamp(latitude, -89.5, 89.5);
        m_longitude = std::remainder(longitude, 360.0);
        rebuildRotation();
    }

    void reset(double latitude, double longitude)
    {
        m_rollDegrees = 0.0F;
        setCenter(latitude, longitude);
    }

    void applyDragDelta(const QPointF& delta, float degreesPerPixel)
    {
        m_longitude = std::remainder(
            m_longitude - delta.x() * degreesPerPixel, 360.0);
        m_latitude = std::clamp(
            m_latitude + delta.y() * degreesPerPixel, -89.5, 89.5);
        rebuildRotation();
    }

    bool applyScreenDrag(const QPointF& from, const QPointF& to,
                         const QSizeF& viewport, double cameraDistance,
                         double verticalFieldOfViewDegrees)
    {
        if (viewport.width() <= 0.0 || viewport.height() <= 0.0
            || cameraDistance <= 1.0 || verticalFieldOfViewDegrees <= 0.0
            || verticalFieldOfViewDegrees >= 180.0) {
            return false;
        }
        const double focal = viewport.height()
            / (2.0 * std::tan(verticalFieldOfViewDegrees * std::numbers::pi / 360.0));
        const auto surface = [&](const QPointF& pixel) {
            double x = (pixel.x() - viewport.width() / 2.0) / focal;
            double y = (viewport.height() / 2.0 - pixel.y()) / focal;
            // Project off-globe drags to the silhouette instead of inventing
            // a ray intersection behind the sphere.
            const double radius = std::hypot(x, y);
            const double limb = 1.0 / std::sqrt(cameraDistance * cameraDistance - 1.0);
            if (radius > limb) {
                x *= limb / radius;
                y *= limb / radius;
            }
            const double a = 1.0 + x * x + y * y;
            const double root = std::sqrt(std::max(0.0,
                cameraDistance * cameraDistance - a * (cameraDistance * cameraDistance - 1.0)));
            const double t = (cameraDistance * cameraDistance - 1.0)
                / (cameraDistance + root);
            return QVector3D(static_cast<float>(t * x), static_cast<float>(t * y),
                             static_cast<float>(cameraDistance - t));
        };
        const QVector3D grabbed = m_rotation.conjugated().rotatedVector(surface(from));
        const QVector3D target = QQuaternion::fromAxisAndAngle(
            {0.0F, 0.0F, 1.0F}, -m_rollDegrees).rotatedVector(surface(to));
        const double horizontal = std::hypot(grabbed.x(), grabbed.z());
        if (horizontal < 1e-8) {
            return false;
        }
        const double angle = std::asin(std::clamp(target.x() / horizontal, -1.0, 1.0));
        const double grabbedLongitude = std::atan2(grabbed.x(), grabbed.z());
        double bestCost = std::numeric_limits<double>::infinity();
        double latitude = m_latitude;
        double longitude = m_longitude;
        // Both trigonometric branches are possible. Keep the nearest valid
        // orientation, preserving the user's explicit roll and pole limits.
        for (double azimuth : {angle, std::numbers::pi - angle}) {
            const double candidateLongitude = std::remainder(
                (grabbedLongitude - azimuth) * 180.0 / std::numbers::pi, 360.0);
            const double candidateLatitude = std::remainder(
                (std::atan2(grabbed.y(), horizontal * std::cos(azimuth))
                 - std::atan2(target.y(), target.z())) * 180.0 / std::numbers::pi, 360.0);
            if (std::abs(candidateLatitude) > 89.5) {
                continue;
            }
            const double cost = std::abs(candidateLatitude - m_latitude)
                + std::abs(std::remainder(candidateLongitude - m_longitude, 360.0));
            if (cost < bestCost) {
                bestCost = cost;
                latitude = candidateLatitude;
                longitude = candidateLongitude;
            }
        }
        setCenter(latitude, longitude);
        return std::isfinite(bestCost);
    }

    void applyRollDelta(float degrees)
    {
        m_rollDegrees = std::remainder(m_rollDegrees + degrees, 360.0F);
        rebuildRotation();
    }

    double latitude() const { return m_latitude; }
    double longitude() const { return m_longitude; }
    float rollDegrees() const { return m_rollDegrees; }
    const QQuaternion& rotation() const { return m_rotation; }

private:
    void rebuildRotation()
    {
        const QQuaternion longitude = QQuaternion::fromAxisAndAngle(
            { 0.0F, 1.0F, 0.0F }, static_cast<float>(-m_longitude));
        const QQuaternion latitude = QQuaternion::fromAxisAndAngle(
            { 1.0F, 0.0F, 0.0F }, static_cast<float>(m_latitude));
        const QQuaternion roll = QQuaternion::fromAxisAndAngle(
            { 0.0F, 0.0F, 1.0F }, m_rollDegrees);
        m_rotation = roll * latitude * longitude;
        m_rotation.normalize();
    }

    QQuaternion m_rotation;
    double m_latitude{0.0};
    double m_longitude{0.0};
    float m_rollDegrees{0.0F};
};

} // namespace AetherSDR
