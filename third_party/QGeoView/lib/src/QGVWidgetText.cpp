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

#include "QGVWidgetText.h"

#include <QHBoxLayout>

QGVWidgetText::QGVWidgetText()
{
    mLabel.reset(new QLabel());
    mLabel->setText("");
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAnchor(QPoint(10, 10), { Qt::LeftEdge, Qt::BottomEdge });
    setLayout(new QHBoxLayout(this));
    layout()->setSpacing(0);
    layout()->setSizeConstraint(QLayout::SetMinimumSize);
    layout()->setContentsMargins(0, 0, 0, 0);
    layout()->addWidget(mLabel.data());
}

QLabel* QGVWidgetText::label()
{
    return mLabel.data();
}

void QGVWidgetText::setText(const QString& text)
{
    mLabel->setText(text);
}

QString QGVWidgetText::getText() const
{
    return mLabel->text();
}
