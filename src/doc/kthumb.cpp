/*
Copyright (C) 2016  Jean-Baptiste Mardelle <jb@kdenlive.org>
This file is part of Kdenlive. See www.kdenlive.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License or (at your option) version 3 or any later version
accepted by the membership of KDE e.V. (or its successor approved
by the membership of KDE e.V.), which shall act as a proxy 
defined in Section 14 of version 3 of the license.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "kthumb.h"
#include "kdenlivesettings.h"

#include <mlt++/Mlt.h>

#include <QImage>
#include <QPainter>

//static
QPixmap KThumb::getImage(const QUrl &url, int width, int height)
{
    if (!url.isValid()) return QPixmap();
    return getImage(url, 0, width, height);
}

//static
QPixmap KThumb::getImage(const QUrl &url, int frame, int width, int height)
{
    Mlt::Profile profile(KdenliveSettings::current_profile().toUtf8().constData());
    if (height == -1) {
        height = width * profile.height() / profile.width();
    }
    QPixmap pix(width, height);
    if (!url.isValid()) return pix;
    Mlt::Producer *producer = new Mlt::Producer(profile, url.path().toUtf8().constData());
    pix = QPixmap::fromImage(getFrame(producer, frame, width, height));
    delete producer;
    return pix;
}

//static
QImage KThumb::getFrame(Mlt::Producer *producer, int framepos, int displayWidth, int height)
{
    if (producer == NULL || !producer->is_valid()) {
        QImage p(displayWidth, height, QImage::Format_ARGB32_Premultiplied);
        p.fill(QColor(Qt::red).rgb());
        return p;
    }
    if (producer->is_blank()) {
        QImage p(displayWidth, height, QImage::Format_ARGB32_Premultiplied);
        p.fill(QColor(Qt::black).rgb());
        return p;
    }

    producer->seek(framepos);
    Mlt::Frame *frame = producer->get_frame();
    const QImage p = getFrame(frame, displayWidth, height);
    delete frame;
    return p;
}


//static
QImage KThumb::getFrame(Mlt::Frame *frame, int width, int height)
{
    if (frame == NULL || !frame->is_valid()) {
        QImage p(width, height, QImage::Format_ARGB32_Premultiplied);
        p.fill(QColor(Qt::red).rgb());
        return p;
    }
    int ow = width;
    int oh = height;
    mlt_image_format format = mlt_image_rgb24a;
    //frame->set("progressive", "1");
    ow += ow % 2;
    const uchar* imagedata = frame->get_image(format, ow, oh);
    if (imagedata) {
        QImage image(imagedata, ow, oh, QImage::Format_RGBA8888);
        //memcpy(image.bits(), imagedata, ow * oh * 4);
        if (!image.isNull()) {
            if (ow > (2 * width)) {
                // there was a scaling problem, do it manually
                image = image.scaled(width, height);
            }
            return image;
            /*p.fill(QColor(100, 100, 100, 70));
            QPainter painter(&p);
            painter.drawImage(p.rect(), image);
            painter.end();*/
        }
    }
    QImage p(width, height, QImage::Format_ARGB32_Premultiplied);
    p.fill(QColor(Qt::red).rgb());
    return p;
}

//static
uint KThumb::imageVariance(const QImage &image )
{
    uint delta = 0;
    uint avg = 0;
    uint bytes = image.byteCount();
    uint STEPS = bytes/2;
    QVarLengthArray<uchar> pivot(STEPS);
    const uchar *bits=image.bits();
    // First pass: get pivots and taking average
    for( uint i=0; i<STEPS ; ++i ){
        pivot[i] = bits[2 * i];
        avg+=pivot.at(i);
    }
    if (STEPS)
        avg=avg/STEPS;
    // Second Step: calculate delta (average?)
    for (uint i=0; i<STEPS; ++i)
    {
        int curdelta=abs(int(avg - pivot.at(i)));
        delta+=curdelta;
    }
    if (STEPS)
        return delta/STEPS;
    else
        return 0;
}
