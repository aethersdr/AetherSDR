/***************************************************************************
 * QGeoView is a Qt / C ++ widget for visualizing geographic data.
 * Copyright (C) 2018-2025 Andrey Yaroshenko.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see https://www.gnu.org/licenses.
 ****************************************************************************/

#include "QGVCamera.h"
#include "QGVMap.h"

#include <QtMath>

QGVCameraState::QGVCameraState(QGVMap* geoMap, double azimuth, double scale, const QRectF& projRect, bool animation)
    : mGeoMap(geoMap)
    , mScale(scale)
    , mAzimuth(azimuth)
    , mProjRect(projRect)
    , mAnimation(animation)
{
    Q_ASSERT(geoMap);
}

QGVCameraState::QGVCameraState(const QGVCameraState& other)
    : mGeoMap(other.mGeoMap)
    , mScale(other.mScale)
    , mAzimuth(other.mAzimuth)
    , mProjRect(other.mProjRect)
    , mAnimation(other.mAnimation)
{
}

QGVCameraState::QGVCameraState(const QGVCameraState&& other)
    : mGeoMap(std::move(other.mGeoMap))
    , mScale(std::move(other.mScale))
    , mAzimuth(std::move(other.mAzimuth))
    , mProjRect(std::move(other.mProjRect))
    , mAnimation(std::move(other.mAnimation))
{
}

QGVCameraState& QGVCameraState::operator=(const QGVCameraState& other)
{
    mGeoMap = other.mGeoMap;
    mScale = other.mScale;
    mAzimuth = other.mAzimuth;
    mProjRect = other.mProjRect;
    mAnimation = other.mAnimation;
    return *this;
}

QGVCameraState& QGVCameraState::operator=(const QGVCameraState&& other)
{
    mGeoMap = std::move(other.mGeoMap);
    mScale = std::move(other.mScale);
    mAzimuth = std::move(other.mAzimuth);
    mProjRect = std::move(other.mProjRect);
    mAnimation = std::move(other.mAnimation);
    return *this;
}

QGVMap* QGVCameraState::getMap() const
{
    return mGeoMap;
}

QGVProjection* QGVCameraState::getProjection() const
{
    return mGeoMap->getProjection();
}

double QGVCameraState::azimuth() const
{
    return mAzimuth;
}

double QGVCameraState::scale() const
{
    return mScale;
}

QRectF QGVCameraState::projRect() const
{
    return mProjRect;
}

QPointF QGVCameraState::projCenter() const
{
    return mProjRect.center();
}

bool QGVCameraState::animation() const
{
    return mAnimation;
}

bool QGVCameraState::operator==(const QGVCameraState& other) const
{
    return mGeoMap == other.mGeoMap && qFuzzyCompare(mScale, other.mScale) && qFuzzyCompare(mAzimuth, other.mAzimuth) &&
           mProjRect == other.mProjRect && mAnimation == other.mAnimation;
}

bool QGVCameraState::operator!=(const QGVCameraState& other) const
{
    return !(*this == other);
}

QGVCameraActions::QGVCameraActions(QGVMap* geoMap)
    : mOrigin(geoMap->getCamera())
{
    reset();
}

const QGVCameraState& QGVCameraActions::origin() const
{
    return mOrigin;
}

QGVCameraActions& QGVCameraActions::rebase(const QGVCameraState& origin)
{
    mOrigin = origin;
    return *this;
}

QGVCameraActions& QGVCameraActions::reset()
{
    return reset(mOrigin);
}

QGVCameraActions& QGVCameraActions::reset(const QGVCameraState& origin)
{
    mOrigin = origin;
    mScale = mOrigin.scale();
    mAzimuth = mOrigin.azimuth();
    mProjCenter = mOrigin.projCenter();
    return *this;
}

QGVCameraActions& QGVCameraActions::scaleBy(double factor)
{
    return scaleTo(mOrigin.scale() * factor);
}

QGVCameraActions& QGVCameraActions::scaleTo(double scale)
{
    mScale = scale;
    return *this;
}

QGVCameraActions& QGVCameraActions::scaleTo(const QRectF& projRect)
{
    const QRectF oldProjRect = mOrigin.projRect();
    const double scaleFactor =
            qMin(qAbs(oldProjRect.width() / projRect.width()), qAbs(oldProjRect.height() / projRect.height()));
    mScale = mOrigin.scale() * scaleFactor;
    mProjCenter = projRect.center();
    return *this;
}

QGVCameraActions& QGVCameraActions::scaleTo(const QGV::GeoRect& geoRect)
{
    const QRectF projRect = mOrigin.getMap()->getProjection()->geoToProj(geoRect);
    return scaleTo(projRect);
}

QGVCameraActions& QGVCameraActions::rotateBy(double angle)
{
    rotateTo(mOrigin.azimuth() + angle);
    return *this;
}

QGVCameraActions& QGVCameraActions::rotateTo(double azimuth)
{
    mAzimuth = azimuth;
    return *this;
}

QGVCameraActions& QGVCameraActions::moveTo(const QPointF& projPos)
{
    mProjCenter = projPos;
    return *this;
}

QGVCameraActions& QGVCameraActions::moveTo(const QGV::GeoPos& geoPos)
{
    const QPointF projPos = mOrigin.getMap()->getProjection()->geoToProj(geoPos);
    return moveTo(projPos);
}

double QGVCameraActions::scale() const
{
    return mScale;
}

double QGVCameraActions::azimuth() const
{
    return mAzimuth;
}

QPointF QGVCameraActions::projCenter() const
{
    return mProjCenter;
}

QGVCameraAnimation::QGVCameraAnimation(const QGVCameraActions& actions, QObject* parent)
    : QAbstractAnimation(parent)
    , mDuration(1000)
    , mActions(actions)
{
}

void QGVCameraAnimation::setDuration(int msecs)
{
    mDuration = msecs;
}

int QGVCameraAnimation::duration() const
{
    return mDuration;
}

QGVCameraActions& QGVCameraAnimation::actions()
{
    return mActions;
}

void QGVCameraAnimation::onStart()
{
}

void QGVCameraAnimation::onStop()
{
}

double QGVCameraAnimation::interpolateScale(double from, double to, double progress)
{
    if (qFuzzyCompare(from, to)) {
        return from;
    }
    const double expFrom = qLn(from) * M_LOG2E;
    const double expTo = qLn(to) * M_LOG2E;
    const double delta = expTo - expFrom;
    return qPow(2, expFrom + delta * progress);
}

double QGVCameraAnimation::interpolateAzimuth(double from, double to, double progress)
{
    if (qFuzzyCompare(from, to)) {
        return from;
    }
    const double delta = to - from;
    return from + delta * progress;
}

QPointF QGVCameraAnimation::interpolatePos(QPointF from, QPointF to, double progress)
{
    if (from == to) {
        return from;
    }
    const QPointF delta = to - from;
    return from + delta * progress;
}

void QGVCameraAnimation::updateState(QAbstractAnimation::State newState, QAbstractAnimation::State oldState)
{
    auto geoMap = mActions.origin().getMap();
    if (newState == QAbstractAnimation::Running && oldState == QAbstractAnimation::Stopped) {
        mActions.rebase(geoMap->getCamera());
        connect(geoMap, &QGVMap::stateChanged, this, &QGVCameraAnimation::onStateChanged);
        onStart();
    }
    if (newState == QAbstractAnimation::Stopped && oldState != QAbstractAnimation::Stopped) {
        disconnect(geoMap, nullptr, this, nullptr);
        geoMap->cameraTo(QGVCameraActions(geoMap), false);
        onStop();
    }
}

void QGVCameraAnimation::updateCurrentTime(int currentTime)
{
    double progress = static_cast<double>(currentTime) / duration();
    if (direction() == Direction::Backward) {
        progress = 1.0 - progress;
    }
    QGVCameraActions target(mActions);
    target.reset();
    onProgress(progress, target);
    mActions.origin().getMap()->cameraTo(target, true);
}

void QGVCameraAnimation::onStateChanged(QGV::MapState state)
{
    if (state != QGV::MapState::Animation) {
        stop();
    }
}

QGVCameraSimpleAnimation::QGVCameraSimpleAnimation(const QGVCameraActions& actions, QObject* parent)
    : QGVCameraAnimation(actions, parent)
    , mEasing(QEasingCurve::Linear)
{
}

void QGVCameraSimpleAnimation::setEasingCurve(const QEasingCurve& easing)
{
    mEasing = easing;
}

void QGVCameraSimpleAnimation::onProgress(double progress, QGVCameraActions& target)
{
    progress = mEasing.valueForProgress(progress);
    target.scaleTo(interpolateScale(actions().origin().scale(), actions().scale(), progress));
    target.rotateTo(interpolateAzimuth(actions().origin().azimuth(), actions().azimuth(), progress));
    target.moveTo(interpolatePos(actions().origin().projCenter(), actions().projCenter(), progress));
}

QGVCameraFlyAnimation::QGVCameraFlyAnimation(const QGVCameraActions& actions, QObject* parent)
    : QGVCameraAnimation(actions, parent)
{
    setDuration(3000);
}

void QGVCameraFlyAnimation::onStart()
{
    const QLineF line(actions().projCenter(), actions().origin().projCenter());
    const double scaledDistance0 = line.length();
    const double scaledDistance1 = scaledDistance0 * actions().origin().scale();
    const double scaledDistance2 = scaledDistance0 * actions().scale();
    const double projSpeed0 = scaledDistance0 / (duration() / 1000);
    const double projSpeed1 = scaledDistance1 / (duration() / 1000);
    const double projSpeed2 = scaledDistance2 / (duration() / 1000);
    const double expectedSpeed = 300;
    if (scaledDistance0 == 0) {
        setDuration(1000);
    } else if (projSpeed1 < expectedSpeed && projSpeed2 < expectedSpeed) {
        setDuration(static_cast<int>(1000.0 * qMax(scaledDistance1, scaledDistance2) / expectedSpeed));
    }
    mFlyScale = qMin(actions().scale(), expectedSpeed / projSpeed0);
    mFlyAnchor = interpolatePos(actions().origin().projCenter(), actions().projCenter(), 0.5);
}

void QGVCameraFlyAnimation::onProgress(double progress, QGVCameraActions& target)
{
    const auto moveCurve = QEasingCurve(QEasingCurve::InQuint);
    const double switchThr = 0.5;
    if (progress <= switchThr) {
        const double flyInter = progress / switchThr;
        target.scaleTo(interpolateScale(actions().origin().scale(), mFlyScale, flyInter));
        const double moveInter = moveCurve.valueForProgress(flyInter);
        target.moveTo(interpolatePos(actions().origin().projCenter(), mFlyAnchor, moveInter));
    } else {
        const double flyInter = (progress - switchThr) / (1.0 - switchThr);
        target.scaleTo(interpolateScale(mFlyScale, actions().scale(), flyInter));
        const double moveInter = 1.0 - moveCurve.valueForProgress(1.0 - flyInter);
        target.moveTo(interpolatePos(mFlyAnchor, actions().projCenter(), moveInter));
    }

    target.rotateTo(interpolateAzimuth(actions().origin().azimuth(), actions().azimuth(), progress));
}
