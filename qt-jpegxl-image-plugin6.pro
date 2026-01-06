TARGET = qjpegxl6

HEADERS = src/qjpegxlhandler_p.h src/util_p.h
SOURCES = src/qjpegxlhandler.cpp
OTHER_FILES = src/jpegxl.json

SOURCES += src/main.cpp

LIBS += -ljxl -ljxl_threads -ljxl_cms -lbrotlicommon -lbrotlienc -lbrotlidec

TEMPLATE = lib

CONFIG += release skip_target_version_ext c++14 warn_on plugin
CONFIG -= separate_debug_info debug debug_and_release force_debug_info

QMAKE_TARGET_COMPANY = "Daniel Novomesky"
QMAKE_TARGET_PRODUCT = "qt-jpegxl-image-plugin"
QMAKE_TARGET_DESCRIPTION = "Qt plug-in to allow Qt and KDE based applications to read/write JPEG XL images."
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2020-2026 Daniel Novomesky"
