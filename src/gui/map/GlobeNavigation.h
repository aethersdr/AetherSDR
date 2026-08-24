#pragma once

#include <QPointF>
#include <QQuaternion>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// Projection-independent interaction state for the globe. Rebuilding the
// quaternion from explicit latitude, longitude and roll keeps ordinary drag
// gestures from accumulating unintended axial tilt.
class GlobeNavigation final {
public:
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
