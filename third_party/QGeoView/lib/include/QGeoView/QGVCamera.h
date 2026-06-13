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

#pragma once

#include "QGVGlobal.h"

#include <QAbstractAnimation>
#include <QEasingCurve>

class QGVMap;
class QGVProjection;

class QGV_LIB_DECL QGVCameraState
{
public:
    explicit QGVCameraState(QGVMap* geoMap, double azimuth, double scale, const QRectF& projRect, bool animation);
    QGVCameraState(const QGVCameraState& other);
    QGVCameraState(const QGVCameraState&& other);
    QGVCameraState& operator=(const QGVCameraState& other);
    QGVCameraState& operator=(const QGVCameraState&& other);

    QGVMap* getMap() const;
    QGVProjection* getProjection() const;

    double scale() const;
    double azimuth() const;
    QRectF projRect() const;
    QPointF projCenter() const;
    bool animation() const;

    bool operator==(const QGVCameraState& other) const;
    bool operator!=(const QGVCameraState& other) const;

private:
    QGVMap* mGeoMap;
    double mScale;
    double mAzimuth;
    QRectF mProjRect;
    bool mAnimation;
};

class QGV_LIB_DECL QGVCameraActions
{
public:
    explicit QGVCameraActions(QGVMap* geoMap);

    const QGVCameraState& origin() const;

    QGVCameraActions& rebase(const QGVCameraState& origin);
    QGVCameraActions& reset();
    QGVCameraActions& reset(const QGVCameraState& origin);
    QGVCameraActions& scaleBy(double factor);
    QGVCameraActions& scaleTo(double scale);
    QGVCameraActions& scaleTo(const QRectF& projRect);
    QGVCameraActions& scaleTo(const QGV::GeoRect& geoRect);
    QGVCameraActions& rotateBy(double angle);
    QGVCameraActions& rotateTo(double azimuth);
    QGVCameraActions& moveTo(const QPointF& projPos);
    QGVCameraActions& moveTo(const QGV::GeoPos& geoPos);

    double scale() const;
    double azimuth() const;
    QPointF projCenter() const;

private:
    QGVCameraState mOrigin;
    double mScale;
    double mAzimuth;
    QPointF mProjCenter;
};

class QGV_LIB_DECL QGVCameraAnimation : public QAbstractAnimation
{
public:
    explicit QGVCameraAnimation(const QGVCameraActions& actions, QObject* parent = nullptr);

    void setDuration(int msecs);
    int duration() const override;
    QGVCameraActions& actions();

protected:
    virtual void onStart();
    virtual void onStop();
    virtual void onProgress(double progress, QGVCameraActions& target) = 0;

    static double interpolateScale(double from, double to, double progress);
    static double interpolateAzimuth(double from, double to, double progress);
    static QPointF interpolatePos(QPointF from, QPointF to, double progress);

private:
    void updateState(QAbstractAnimation::State newState, QAbstractAnimation::State oldState) override;
    void updateCurrentTime(int currentTime) override;
    void onStateChanged(QGV::MapState state);

private:
    int mDuration;
    QGVCameraActions mActions;
};

class QGV_LIB_DECL QGVCameraSimpleAnimation : public QGVCameraAnimation
{
public:
    explicit QGVCameraSimpleAnimation(const QGVCameraActions& actions, QObject* parent = nullptr);

    void setEasingCurve(const QEasingCurve& easing);

private:
    void onProgress(double progress, QGVCameraActions& target) override;

private:
    QEasingCurve mEasing;
};

class QGV_LIB_DECL QGVCameraFlyAnimation : public QGVCameraAnimation
{
public:
    explicit QGVCameraFlyAnimation(const QGVCameraActions& actions, QObject* parent = nullptr);

private:
    void onStart() override;
    void onProgress(double progress, QGVCameraActions& target) override;

private:
    double mFlyScale;
    QPointF mFlyAnchor;
};
