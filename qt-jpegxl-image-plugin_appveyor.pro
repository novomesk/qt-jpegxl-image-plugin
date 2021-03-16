TARGET = qjpegxl

DEFINES += JXL_STATIC_DEFINE JXL_THREADS_STATIC_DEFINE

INCLUDEPATH += jpeg-xl/lib/include jpeg-xl/build/lib/include

HEADERS = src/qjpegxlhandler_p.h
SOURCES = src/qjpegxlhandler.cpp
OTHER_FILES = src/jpegxl.json

SOURCES += src/main.cpp

LIBS += jpeg-xl/build/lib/jxl-static.lib jpeg-xl/build/lib/jxl_threads-static.lib jpeg-xl/build/third_party/skcms.lib jpeg-xl/build/third_party/highway/hwy.lib jpeg-xl/build/third_party/brotli/brotlicommon-static.lib jpeg-xl/build/third_party/brotli/brotlidec-static.lib jpeg-xl/build/third_party/brotli/brotlienc-static.lib

PLUGIN_TYPE = imageformats
PLUGIN_CLASS_NAME = QJpegXLPlugin
load(qt_plugin)

CONFIG += release skip_target_version_ext c++14 warn_on
CONFIG -= separate_debug_info debug debug_and_release force_debug_info

QMAKE_TARGET_COMPANY = "Daniel Novomesky"
QMAKE_TARGET_PRODUCT = "qt-jpegxl-image-plugin"
QMAKE_TARGET_DESCRIPTION = "Qt plug-in to allow Qt and KDE based applications to read/write JPEG XL images."
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2020-2021 Daniel Novomesky"
