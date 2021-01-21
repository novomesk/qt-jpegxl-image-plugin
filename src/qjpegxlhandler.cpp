/*
 * QT plug-in to allow import/export in JPEG XL image format.
 * Author: Daniel Novomesky
 */

/*
*/

#include <QtGlobal>
#include <QThread>

#include <QColorSpace>

#include "qjpegxlhandler_p.h"
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

QJpegXLHandler::QJpegXLHandler() :
    m_parseState(ParseJpegXLNotParsed),
    m_quality(52),
    m_currentimage_index(0),
    m_buffer_remaining(nullptr),
    m_length_remaining(0),
    m_decoder(nullptr),
    m_runner(nullptr),
    m_next_image_delay(0)
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
    if (m_parseState == ParseJpegXLSuccess || m_parseState == ParseJpegXLBasicInfoParsed) {
        return true;
    }
    if (m_parseState == ParseJpegXLError) {
        return false;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->ensureDecoder();
}

bool QJpegXLHandler::ensureALLDecoded() const
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_parseState == ParseJpegXLSuccess) {
        return true;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->decodeALLFrames();
}

bool QJpegXLHandler::ensureDecoder()
{
    if (m_decoder) {
        return true;
    }

    m_rawData = device()->readAll();
    m_buffer_remaining = (const uint8_t *)m_rawData.constData();
    m_length_remaining = m_rawData.size();

    JxlSignature signature = JxlSignatureCheck(m_buffer_remaining, m_length_remaining);
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

    if (!m_runner) {
        m_runner = JxlThreadParallelRunnerCreate(NULL, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    }
    if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetParallelRunner failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderStatus status = JxlDecoderSubscribeEvents(m_decoder,
                              JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING
                              | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JxlDecoderSubscribeEvents failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining);
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

    m_parseState = ParseJpegXLBasicInfoParsed;
    return true;
}

bool QJpegXLHandler::decodeALLFrames()
{
    if (m_parseState != ParseJpegXLBasicInfoParsed) {
        return false;
    }

    JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining);
    if (status != JXL_DEC_COLOR_ENCODING) {
        qWarning("Unexpected event %d instead of JXL_DEC_COLOR_ENCODING", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    QColorSpace colorspace;
    bool loadalpha;

    if (m_basicinfo.alpha_bits > 0) {
        loadalpha = true;
    } else {
        loadalpha = false;
    }

    JxlPixelFormat pixel_format;

    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.data_type = JXL_TYPE_UINT16;
    pixel_format.align = 0;
    pixel_format.num_channels = 4;

    const size_t result_size = 8 * (size_t) m_basicinfo.xsize * (size_t) m_basicinfo.ysize;
    size_t icc_size = 0;
    if (JxlDecoderGetICCProfileSize(m_decoder, &pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) == JXL_DEC_SUCCESS) {
        if (icc_size > 0) {
            QByteArray icc_data(icc_size, 0);
            if (JxlDecoderGetColorAsICCProfile(m_decoder, &pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, (uint8_t *)icc_data.data(), icc_data.size()) == JXL_DEC_SUCCESS) {
                colorspace = QColorSpace::fromIccProfile(icc_data);

                if (!colorspace.isValid()) {
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

    JxlFrameHeader frame_header;
    int delay;

    for (status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining);
            status != JXL_DEC_SUCCESS;
            status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining)) {

        if (status != JXL_DEC_FRAME) {
            qWarning("Unexpected event %d instead of JXL_DEC_FRAME", status);
            m_parseState = ParseJpegXLError;
            return false;
        }

        if (JxlDecoderGetFrameHeader(m_decoder, &frame_header) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderGetFrameHeader failed");
            m_parseState = ParseJpegXLError;
            return false;
        }

        delay = 0;
        if (m_basicinfo.have_animation) {
            if (m_basicinfo.animation.tps_denominator > 0 && m_basicinfo.animation.tps_numerator > 0) {
                delay = (int)(0.5 + 1000.0 * frame_header.duration * m_basicinfo.animation.tps_denominator / m_basicinfo.animation.tps_numerator);
            }
        }

        status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining);
        if (status != JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            qWarning("Unexpected event %d instead of JXL_DEC_NEED_IMAGE_OUT_BUFFER", status);
            m_parseState = ParseJpegXLError;
            return false;
        }

        m_frames.append(QPair(QImage(m_basicinfo.xsize, m_basicinfo.ysize, QImage::Format_RGBA64), delay));
        if (m_frames.last().first.isNull()) {
            qWarning("Memory cannot be allocated");
            m_parseState = ParseJpegXLError;
            return false;
        }
        if (colorspace.isValid()) {
            m_frames.last().first.setColorSpace(colorspace);
        }
        if (JxlDecoderSetImageOutBuffer(m_decoder, &pixel_format, m_frames.last().first.bits(), result_size) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSetImageOutBuffer failed");
            m_parseState = ParseJpegXLError;
            return false;
        }

        status = JxlDecoderProcessInput(m_decoder, &m_buffer_remaining, &m_length_remaining);
        if (status != JXL_DEC_FULL_IMAGE) {
            qWarning("Unexpected event %d instead of JXL_DEC_FULL_IMAGE", status);
            m_parseState = ParseJpegXLError;
            return false;
        }

        qWarning("full image");
        if (!loadalpha) {
            m_frames.last().first = m_frames.last().first.convertToFormat(QImage::Format_RGBX64);
        }
    }

    if (m_frames.isEmpty()) {
        qWarning("no frames loaded by JXL plug-in");
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_next_image_delay = m_frames.first().second;

    m_parseState = ParseJpegXLSuccess;
    m_buffer_remaining = nullptr;
    m_length_remaining = 0;
    m_rawData.clear();
    return true;
}

bool QJpegXLHandler::read(QImage *image)
{
    if (!ensureALLDecoded()) {
        return false;
    }

    const QPair<QImage, int>  &currentimage = m_frames.at(m_currentimage_index);
    *image = currentimage.first;
    m_next_image_delay = currentimage.second;

    if (m_frames.count() >= 2) {
        jumpToNextImage();
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

    JxlEncoderOptions *encoder_options = JxlEncoderOptionsCreate(encoder, NULL);

    JxlPixelFormat pixel_format;
    QImage::Format tmpformat;
    JxlEncoderStatus status;

    pixel_format.data_type = JXL_TYPE_UINT16;
    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 0;

    if (image.hasAlphaChannel()) {
        tmpformat = QImage::Format_RGBA64;
        pixel_format.num_channels = 4;
    } else {
        tmpformat = QImage::Format_RGBX64;
        pixel_format.num_channels = 3;
    }

    const QImage tmpimage = image.convertToFormat(tmpformat).convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
    const size_t xsize = tmpimage.width();
    const size_t ysize = tmpimage.height();
    const size_t buffer_size = 2 * pixel_format.num_channels * xsize * ysize;

    if (xsize == 0 || ysize == 0 || tmpimage.isNull()) {
        qWarning("Unable to allocate memory for output image");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }

    status = JxlEncoderSetDimensions(encoder, xsize, ysize);
    if (status != JXL_ENC_SUCCESS) {
        qWarning("JxlEncoderSetDimensions failed!");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }

    if (image.hasAlphaChannel()) {
        status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, (void *)tmpimage.constBits(), buffer_size);
    } else {
        uint16_t *tmp_buffer = new (std::nothrow) uint16_t [3 * xsize * ysize];
        if (!tmp_buffer) {
            qWarning("Memory allocation error");
            JxlThreadParallelRunnerDestroy(runner);
            JxlEncoderDestroy(encoder);
            return false;
        }

        uint16_t *dest_pixels = tmp_buffer;
        for (int y = 0; y < tmpimage.height(); y++) {
            const uint16_t *src_pixels = reinterpret_cast<const uint16_t *>(tmpimage.constScanLine(y));
            for (int x = 0; x < tmpimage.width(); x++) {
                //R
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels++;
                //G
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels++;
                //B
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels += 2; //skipalpha
            }
        }
        status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, (void *)tmp_buffer, buffer_size);
        delete [] tmp_buffer;
    }

    if (status == JXL_ENC_ERROR) {
        qWarning("JxlEncoderAddImageFrame failed!");
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(encoder);
        return false;
    }

    JxlEncoderCloseInput(encoder);

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

    compressed.resize(next_out - compressed.data());

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
        return QSize(m_basicinfo.xsize, m_basicinfo.ysize);
    case Animation:
        if (m_basicinfo.have_animation) {
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

    if (m_parseState == ParseJpegXLBasicInfoParsed) {
        if (!m_basicinfo.have_animation) {
            return 1;
        }

        if (!ensureALLDecoded()) {
            return 0;
        }
    }

    if (!m_frames.isEmpty()) {
        return m_frames.count();
    }
    return 0;
}

int QJpegXLHandler::currentImageNumber() const
{
    if (m_parseState == ParseJpegXLNotParsed) {
        return -1;
    }

    if (m_parseState == ParseJpegXLError || m_parseState == ParseJpegXLBasicInfoParsed || !m_decoder) {
        return 0;
    }

    return m_currentimage_index;
}

bool QJpegXLHandler::jumpToNextImage()
{
    if (!ensureALLDecoded()) {
        return false;
    }

    const int imagecount = m_frames.count();

    if (imagecount < 2) {
        return true;
    }

    int next_image_index = m_currentimage_index + 1;

    if (next_image_index >= imagecount || next_image_index < 0) {
        m_currentimage_index = 0;
    } else {
        m_currentimage_index = next_image_index;
    }

    return true;
}

bool QJpegXLHandler::jumpToImage(int imageNumber)
{
    if (!ensureALLDecoded()) {
        return false;
    }

    if (imageNumber >= 0 && imageNumber < m_frames.count()) {
        m_currentimage_index = imageNumber;
        return true;
    } else {
        return false;
    }
}

int QJpegXLHandler::nextImageDelay() const
{
    if (!ensureALLDecoded()) {
        return 0;
    }

    if (m_frames.count() < 2) {
        return 0;
    }

    return m_next_image_delay;
}

int QJpegXLHandler::loopCount() const
{
    if (!ensureParsed()) {
        return 0;
    }

    if (m_basicinfo.have_animation) {
        return 1;
    } else {
        return 0;
    }
}
