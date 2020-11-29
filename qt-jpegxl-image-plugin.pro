TARGET = qjpegxl

HEADERS = src/qjpegxlhandler_p.h
SOURCES = src/qjpegxlhandler.cpp
OTHER_FILES = src/jpegxl.json

SOURCES += src/main.cpp

LIBS += -ljxl -ljxl_threads -lbrotlicommon -lbrotlienc -lbrotlidec

PLUGIN_TYPE = imageformats
PLUGIN_CLASS_NAME = QJpegXLPlugin
load(qt_plugin)

CONFIG += release skip_target_version_ext c++14 warn_on
CONFIG -= separate_debug_info debug debug_and_release force_debug_info

QMAKE_TARGET_COMPANY = "Daniel Novomesky"
QMAKE_TARGET_PRODUCT = "qt-jpegxl-image-plugin"
QMAKE_TARGET_DESCRIPTION = "Qt plug-in to allow Qt and KDE based applications to read/write JPEG XL images."
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2020 Daniel Novomesky"
