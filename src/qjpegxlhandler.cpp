/*
 * QT plug-in to allow import/export in JPEG XL image format.
 * Author: Daniel Novomesky
 */

/*
*/

#include <QtGlobal>
#include <QThread>

#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
#include <QColorSpace>
#endif

#include "qjpegxlhandler_p.h"
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

QJpegXLHandler::QJpegXLHandler() :
    m_parseState(ParseJpegXLNotParsed),
    m_quality(52),
    m_container_width(0),
    m_container_height(0),
    m_decoder(nullptr),
    m_runner(nullptr),
    m_must_jump_to_next_image(false)
{
}

QJpegXLHandler::~QJpegXLHandler()
{
    if (m_runner) {
        JxlThreadParallelRunnerDestroy(m_runner);
    }
    if (m_decoder) {
        JxlDecoderDestroy(m_decoder);
    }
}

bool QJpegXLHandler::canRead() const
{
    if (m_parseState == ParseJpegXLNotParsed && !canRead(device())) {
        return false;
    }

    if (m_parseState != ParseJpegXLError) {
        setFormat("jxl");
        return true;
    }
    return false;
}

bool QJpegXLHandler::canRead(QIODevice *device)
{
    if (!device) {
        return false;
    }
    QByteArray header = device->peek(32);
    if (header.size() < 12) {
        return false;
    }

    JxlSignature signature = JxlSignatureCheck((const uint8_t *)header.constData(), header.size());
    if (signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER) {
        return true;
    }
    return false;
}

bool QJpegXLHandler::ensureParsed() const
{
    if (m_parseState == ParseJpegXLSuccess) {
        return true;
    }
    if (m_parseState == ParseJpegXLError) {
        return false;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->ensureDecoder();
}

bool QJpegXLHandler::ensureDecoder()
{
    if (m_decoder) {
        return true;
    }

    m_rawData = device()->readAll();
    const uint8_t *buffer_remaining = (const uint8_t *)m_rawData.constData();
    size_t  length_remaining = m_rawData.size();

    JxlSignature signature = JxlSignatureCheck(buffer_remaining, length_remaining);
    if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_decoder = JxlDecoderCreate(NULL);
    if (!m_decoder) {
        qWarning("ERROR: JxlDecoderCreate failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    void *m_runner = JxlThreadParallelRunnerCreate(NULL, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetParallelRunner failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderStatus status;

    status = JxlDecoderSubscribeEvents(m_decoder,
                                       JXL_DEC_BASIC_INFO | JXL_DEC_EXTENSIONS | JXL_DEC_ANIMATION_FRAME |
                                       JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE);
    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JxlDecoderSubscribeEvents failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderProcessInput(m_decoder, &buffer_remaining, &length_remaining);
    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JXL decoding failed");
        m_parseState = ParseJpegXLError;
        return false;
    }
    if (status == JXL_DEC_NEED_MORE_INPUT) {
        qWarning("ERROR: JXL data incomplete");
        m_parseState = ParseJpegXLError;
        return false;
    }


    status = JxlDecoderGetBasicInfo(m_decoder, &m_basicinfo);
    if (status != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JXL basic info not available");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_basicinfo.xsize == 0 || m_basicinfo.ysize == 0) {
        qWarning("ERROR: JXL image has zero dimensions");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_basicinfo.xsize > 32768 || m_basicinfo.ysize > 32768) {
        qWarning("JXL image (%dx%d) is too large", m_basicinfo.xsize, m_basicinfo.ysize);
        m_parseState = ParseJpegXLError;
        return false;
    }

    bool loadalpha;

    if (m_basicinfo.alpha_bits > 0) {
        loadalpha = true;
    } else {
        loadalpha = false;
    }

    QImage::Format resultformat;
    JxlPixelFormat pixel_format;
    size_t result_size;

    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.data_type = JXL_TYPE_UINT8;
    pixel_format.align = 1;

    if (loadalpha) {
        resultformat = QImage::Format_RGBA8888;
        result_size = 4;

        pixel_format.num_channels = 4;
    } else {
        resultformat = QImage::Format_RGB888;
        result_size = 3;

        pixel_format.num_channels = 3;
    }

    result_size = result_size * m_basicinfo.xsize * m_basicinfo.ysize;

    QImage result(m_basicinfo.xsize, m_basicinfo.ysize, resultformat);
    if (result.isNull()) {
        qWarning("Memory cannot be allocated");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderSetImageOutBuffer(m_decoder, &pixel_format, result.bits(), result_size);
    if (status != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetImageOutBuffer failed");
        m_parseState = ParseJpegXLError;
        return false;
    }


    do {
        status = JxlDecoderProcessInput(m_decoder, &buffer_remaining, &length_remaining);

        switch (status) {
        case JXL_DEC_ERROR:
            qWarning("ERROR: JXL full image decoding failed");
            m_parseState = ParseJpegXLError;
            return false;
            break;
        case JXL_DEC_NEED_MORE_INPUT:
            qWarning("ERROR: JXL data incomplete to decode full image");
            m_parseState = ParseJpegXLError;
            return false;
            break;
        case JXL_DEC_ANIMATION_FRAME:
            qWarning("animation header");
            //JxlDecoderGetAnimationFrameHeader(m_decoder, &m_animation_frame_header);
            break;
        case JXL_DEC_COLOR_ENCODING:
            qWarning("color profile available");
            {
                size_t icc_size = 0;
                if (JxlDecoderGetICCProfileSize(m_decoder, &pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) == JXL_DEC_SUCCESS) {
                    if (icc_size > 0) {
                        QByteArray icc_data(icc_size, 0);
                        if (JxlDecoderGetColorAsICCProfile(m_decoder, &pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, (uint8_t *)icc_data.data(), icc_data.size()) == JXL_DEC_SUCCESS) {
                            result.setColorSpace(QColorSpace::fromIccProfile(icc_data));

                            if (!result.colorSpace().isValid()) {
                                qWarning("invalid color profile created");
                            }
                        } else {
                            qWarning("Failed to obtain data from JPEG XL decoder");
                        }
                    } else {
                        qWarning("Empty ICC data");
                    }
                } else {
                    qWarning("no ICC, other color profile");
                }
            }
            break;
        default:
            qWarning("event %d", status);
            break;
        }
    } while (status != JXL_DEC_SUCCESS);

    if (loadalpha) {
        m_current_image = result.convertToFormat(QImage::Format_ARGB32);
    } else {
        m_current_image = result.convertToFormat(QImage::Format_RGB32);
    }
    m_parseState = ParseJpegXLSuccess;
    return true;
}

bool QJpegXLHandler::decode_one_frame()
{
    if (!ensureParsed()) {
        return false;
    }


    m_must_jump_to_next_image = false;
    return true;
}

bool QJpegXLHandler::read(QImage *image)
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_must_jump_to_next_image) {
        jumpToNextImage();
    }

    *image = m_current_image;
    if (imageCount() >= 2) {
        m_must_jump_to_next_image = true;
    }
    return true;
}

bool QJpegXLHandler::write(const QImage &image)
{
    if (image.format() == QImage::Format_Invalid) {
        qWarning("No image data to save");
        return false;
    }

    if ((image.width() > 32768) || (image.height() > 32768)) {
        qWarning("Image is too large");
        return false;
    }

    JxlEncoder *encoder = JxlEncoderCreate(NULL);
    if (!encoder) {
        qWarning("Failed to create Jxl encoder");
        return false;
    }

    void *runner = JxlThreadParallelRunnerCreate(NULL, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) != JXL_ENC_SUCCESS) {
        qWarning("JxlEncoderSetParallelRunner failed");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }


    JxlPixelFormat pixel_format;
    QImage::Format tmpformat;
    JxlEncoderStatus status;
    size_t buffer_size;

    pixel_format.data_type = JXL_TYPE_UINT8;
    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 1;

    if (image.hasAlphaChannel()) {
        tmpformat = QImage::Format_RGBA8888;
        pixel_format.num_channels = 4;
    } else {
        tmpformat = QImage::Format_RGB888;
        pixel_format.num_channels = 3;
    }

    const QImage tmpimage = image.convertToFormat(tmpformat);
    const size_t xsize = tmpimage.width();
    const size_t ysize = tmpimage.height();

    buffer_size = pixel_format.num_channels;
    buffer_size = buffer_size * xsize * ysize;

    status = JxlEncoderSetDimensions(encoder, xsize, ysize);
    if (status != JXL_ENC_SUCCESS) {
        qWarning("JxlEncoderSetDimensions failed!");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }

    status = JxlEncoderAddImageFrame(encoder, &pixel_format, (void *)tmpimage.constBits(), buffer_size);
    if (status == JXL_ENC_ERROR) {
        qWarning("JxlEncoderAddImageFrame failed!");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }

    std::vector<uint8_t> compressed;
    compressed.resize(4096);
    size_t offset = 0;
    uint8_t *next_out;
    size_t avail_out;
    do {
        next_out = compressed.data() + offset;
        avail_out =  compressed.size() - offset;
        status = JxlEncoderProcessOutput(encoder, &next_out, &avail_out);

        if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            offset =  next_out - compressed.data();
            compressed.resize(compressed.size() * 2);
        } else if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderProcessOutput failed!");
            JxlThreadParallelRunnerDestroy(runner);
            JxlEncoderDestroy(encoder);
            return false;
        }
    } while (status != JXL_ENC_SUCCESS);

    JxlThreadParallelRunnerDestroy(runner);
    JxlEncoderDestroy(encoder);

    compressed.resize(next_out -  compressed.data());

    if (compressed.size() > 0) {
        qint64 write_status = device()->write((const char *)compressed.data(), compressed.size());

        if (write_status > 0) {
            return true;
        } else if (write_status == -1) {
            qWarning("Write error: %s\n", qUtf8Printable(device()->errorString()));
        }
    }

    return false;
}


QVariant QJpegXLHandler::option(ImageOption option) const
{
    if (!supportsOption(option) || !ensureParsed()) {
        return QVariant();
    }

    switch (option) {
    case Quality:
        return m_quality;
    case Size:
        return m_current_image.size();
    case Animation:
        if (imageCount() >= 2) {
            return true;
        } else {
            return false;
        }
    default:
        return QVariant();
    }
}

void QJpegXLHandler::setOption(ImageOption option, const QVariant &value)
{
    switch (option) {
    case Quality:
        m_quality = value.toInt();
        if (m_quality > 100) {
            m_quality = 100;
        } else if (m_quality < 0) {
            m_quality = 52;
        }
        return;
    default:
        break;
    }
    QImageIOHandler::setOption(option, value);
}

bool QJpegXLHandler::supportsOption(ImageOption option) const
{
    return option == Quality
           || option == Size
           || option == Animation;
}

int QJpegXLHandler::imageCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

//    if (m_decoder->imageCount >= 1) {
//        return m_decoder->imageCount;
//    }
    return 0;
}

int QJpegXLHandler::currentImageNumber() const
{
    if (m_parseState == ParseJpegXLNotParsed) {
        return -1;
    }

    if (m_parseState == ParseJpegXLError || !m_decoder) {
        return 0;
    }

//    return m_decoder->imageIndex;
    return 0;
}

bool QJpegXLHandler::jumpToNextImage()
{
    if (!ensureParsed()) {
        return false;
    }

//    if (m_decoder->imageCount < 2) {
//        return true;
//    }



    if (decode_one_frame()) {
        return true;
    } else {
        m_parseState = ParseJpegXLError;
        return false;
    }

}

bool QJpegXLHandler::jumpToImage(int imageNumber)
{
    if (!ensureParsed()) {
        return false;
    }

    if (decode_one_frame()) {
        return true;
    } else {
        m_parseState = ParseJpegXLError;
        return false;
    }
}

int QJpegXLHandler::nextImageDelay() const
{
    if (!ensureParsed()) {
        return 0;
    }

//    if (m_decoder->imageCount < 2) {
//        return 0;
//    }

    int delay_ms = 1000.0 /** m_decoder->imageTiming.duration*/;
    if (delay_ms < 1) {
        delay_ms = 1;
    }
    return delay_ms;
}

int QJpegXLHandler::loopCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

//    if (m_decoder->imageCount < 2) {
//        return 0;
//    }

    return 1;
}
