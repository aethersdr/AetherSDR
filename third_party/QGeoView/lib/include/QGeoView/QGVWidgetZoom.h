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

#include <QToolButton>

#include "QGVWidget.h"

class QGV_LIB_DECL QGVWidgetZoom : public QGVWidget
{
    Q_OBJECT

public:
    QGVWidgetZoom();

    void setOrientation(Qt::Orientation orientation);
    Qt::Orientation getOrientation() const;

    QToolButton* plus();
    QToolButton* minus();

private:
    QPixmap createPixmap(const QSize& size, const QString& text);
    void zoomPlus();
    void zoomMinus();

private:
    Qt::Orientation mOrientation;
    QScopedPointer<QToolButton> mButtonPlus;
    QScopedPointer<QToolButton> mButtonMinus;
};
