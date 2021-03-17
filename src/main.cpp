/*
 * QT plug-in to allow import/export in JPEG XL image format.
 * Author: Daniel Novomesky
 */

#include "qjpegxlhandler_p.h"
#include <QImageIOPlugin>

class QJpegXLPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "jpegxl.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override;
    QImageIOHandler *create(QIODevice *device, const QByteArray &format = QByteArray()) const override;
};

QImageIOPlugin::Capabilities QJpegXLPlugin::capabilities(QIODevice *device, const QByteArray &format) const
{
    if (format == "jxl") {
        return Capabilities(CanRead | CanWrite);
    }

    if (!format.isEmpty()) {
        return {};
    }
    if (!device->isOpen()) {
        return {};
    }

    Capabilities cap;
    if (device->isReadable() && QJpegXLHandler::canRead(device)) {
        cap |= CanRead;
    }

    if (device->isWritable()) {
        cap |= CanWrite;
    }

    return cap;
}

QImageIOHandler *QJpegXLPlugin::create(QIODevice *device, const QByteArray &format) const
{
    QImageIOHandler *handler = new QJpegXLHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}

#include "main.moc"
