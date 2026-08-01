#include "WindowGeometryRestore.h"

#include <QDataStream>
#include <QtGlobal>

namespace AetherSDR {

bool parseSavedWindowGeometry(const QByteArray& geometryBlob,
                              SavedWindowGeometry* out)
{
    if (!out) {
        return false;
    }

    QDataStream stream(geometryBlob);
    stream.setVersion(QDataStream::Qt_4_0);

    // Magic, field order and the version ceiling belong to
    // QWidget::saveGeometry(), not to us — keep them in step if Qt ever starts
    // writing a version 4.  Refusing an unknown-newer blob is what Qt does too;
    // it is the difference between declining to help and misreading a rect.
    constexpr quint32 kGeometryMagic = 0x1D9D0CB;
    constexpr quint16 kCurrentGeometryMajorVersion = 3;

    quint32 magic = 0;
    quint16 majorVersion = 0;
    quint16 minorVersion = 0;
    QRect frameGeometry;
    QRect normalGeometry;
    qint32 screenNumber = 0;
    quint8 maximized = 0;
    quint8 fullScreen = 0;

    stream >> magic >> majorVersion >> minorVersion
           >> frameGeometry >> normalGeometry >> screenNumber
           >> maximized >> fullScreen;
    Q_UNUSED(minorVersion);
    Q_UNUSED(frameGeometry);
    Q_UNUSED(screenNumber);

    if (stream.status() != QDataStream::Ok
        || magic != kGeometryMagic
        || majorVersion > kCurrentGeometryMajorVersion) {
        return false;
    }

    // v2 appended the saved screen width, v3 the client geometry().  Qt applies
    // geometry() to a non-maximized window and falls back to normalGeometry for
    // older blobs, so match that and we re-anchor to the same rect Qt aimed at.
    QRect normalRect = normalGeometry;
    if (majorVersion > 1) {
        qint32 savedScreenWidth = 0;
        stream >> savedScreenWidth;
    }
    if (majorVersion > 2) {
        QRect geometry;
        stream >> geometry;
        normalRect = geometry;
    }

    if (stream.status() != QDataStream::Ok || !normalRect.isValid()) {
        return false;
    }

    out->normalRect = normalRect;
    out->maximized = maximized != 0;
    out->fullScreen = fullScreen != 0;
    return true;
}

QRect clampFrameToWorkArea(const QRect& savedFrame, const QRect& availableGeometry)
{
    if (!savedFrame.isValid() || !availableGeometry.isValid()) {
        return savedFrame;
    }

    QRect wanted = savedFrame;

    // Shrink only to what the work area can genuinely hold.  Qt's version
    // subtracts a further 2 + PM_TitleBarHeight here, and that surplus is what
    // turns a work-area-sized window into a permanent gap along the bottom.
    wanted.setWidth(qMin(wanted.width(), availableGeometry.width()));
    wanted.setHeight(qMin(wanted.height(), availableGeometry.height()));

    // Pull the far edges in first and the near edges second, so a rect that
    // overhangs on both sides ends up flush with the top-left corner rather
    // than half off it.  No frame-height term anywhere: that is the whole
    // point — the custom frame has no caption to leave room for.
    if (wanted.right() > availableGeometry.right()) {
        wanted.moveRight(availableGeometry.right());
    }
    if (wanted.bottom() > availableGeometry.bottom()) {
        wanted.moveBottom(availableGeometry.bottom());
    }
    wanted.moveLeft(qMax(wanted.left(), availableGeometry.left()));
    wanted.moveTop(qMax(wanted.top(), availableGeometry.top()));
    return wanted;
}

}  // namespace AetherSDR
