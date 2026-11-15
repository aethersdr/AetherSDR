TARGET = qgeoview
TEMPLATE = lib

QT += gui widgets network

DEFINES += QGV_EXPORT

HEADERS += \
    $$PWD/include/QGeoView/QGVCamera.h \
    $$PWD/include/QGeoView/QGVDrawItem.h \
    $$PWD/include/QGeoView/QGVGlobal.h \
    $$PWD/include/QGeoView/QGVUtils.h \
    $$PWD/include/QGeoView/QGVItem.h \
    $$PWD/include/QGeoView/QGVLayer.h \
    $$PWD/include/QGeoView/QGVLayerBing.h \
    $$PWD/include/QGeoView/QGVLayerGoogle.h \
    $$PWD/include/QGeoView/QGVLayerOSM.h \
    $$PWD/include/QGeoView/QGVLayerBDGEx.h \
    $$PWD/include/QGeoView/QGVLayerTiles.h \
    $$PWD/include/QGeoView/QGVLayerTilesOnline.h \
    $$PWD/include/QGeoView/QGVMap.h \
    $$PWD/include/QGeoView/QGVMapQGItem.h \
    $$PWD/include/QGeoView/QGVMapQGView.h \
    $$PWD/include/QGeoView/QGVMapRubberBand.h \
    $$PWD/include/QGeoView/QGVProjection.h \
    $$PWD/include/QGeoView/QGVProjectionEPSG3857.h \
    $$PWD/include/QGeoView/QGVWidget.h \
    $$PWD/include/QGeoView/QGVWidgetCompass.h \
    $$PWD/include/QGeoView/QGVWidgetScale.h \
    $$PWD/include/QGeoView/QGVWidgetText.h \
    $$PWD/include/QGeoView/QGVWidgetZoom.h \
    $$PWD/include/QGeoView/Raster/QGVImage.h \
    $$PWD/include/QGeoView/Raster/QGVIcon.h \

SOURCES += \
    $$PWD/src/QGVCamera.cpp \
    $$PWD/src/QGVDrawItem.cpp \
    $$PWD/src/QGVGlobal.cpp \
    $$PWD/src/QGVUtils.cpp \
    $$PWD/src/QGVItem.cpp \
    $$PWD/src/QGVLayer.cpp \
    $$PWD/src/QGVLayerBing.cpp \
    $$PWD/src/QGVLayerGoogle.cpp \
    $$PWD/src/QGVLayerOSM.cpp \
    $$PWD/src/QGVLayerBDGEx.cpp \
    $$PWD/src/QGVLayerTiles.cpp \
    $$PWD/src/QGVLayerTilesOnline.cpp \
    $$PWD/src/QGVMap.cpp \
    $$PWD/src/QGVMapQGItem.cpp \
    $$PWD/src/QGVMapQGView.cpp \
    $$PWD/src/QGVMapRubberBand.cpp \
    $$PWD/src/QGVProjection.cpp \
    $$PWD/src/QGVProjectionEPSG3857.cpp \
    $$PWD/src/QGVWidget.cpp \
    $$PWD/src/QGVWidgetCompass.cpp \
    $$PWD/src/QGVWidgetScale.cpp \
    $$PWD/src/QGVWidgetText.cpp \
    $$PWD/src/QGVWidgetZoom.cpp \
    $$PWD/src/Raster/QGVImage.cpp \
    $$PWD/src/Raster/QGVIcon.cpp

INCLUDEPATH += \
    $$PWD/include/ \
    $$PWD/include/QGeoView/

!isEmpty(QGV_DESTDIR){
    DESTDIR = $$QGV_DESTDIR
    message(DESTDIR will be changed to \"$$DESTDIR\")
}

target.path = $${DESTDIR}/lib
target.CONFIG = no_check_exist
INSTALLS += target

header_files.files = $$HEADERS
header_files.path = $${DESTDIR}/include/QGeoView
header_files.CONFIG = no_check_exist
INSTALLS += header_files
