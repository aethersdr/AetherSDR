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

#include "QGVWidget.h"

class QGV_LIB_DECL QGVWidgetCompass : public QGVWidget
{
    Q_OBJECT

public:
    QGVWidgetCompass();

    void setPixmap(QPixmap pixmap);

private:
    QPixmap createPixmap();
    double mouseToAzimuth(const QPoint& pos, double offset) const;
    void onCamera(const QGVCameraState& oldState, const QGVCameraState& newState) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QPixmap mPixmap;
    bool mTracking;
    double mOffset;
    QTransform mTransfrom;
};
