/*
 * QT plug-in to allow import/export in JPEG XL image format.
 * Author: Daniel Novomesky
 */

#include <QThread>
#include <QtGlobal>

#include "qjpegxlhandler_p.h"
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

QJpegXLHandler::QJpegXLHandler()
    : m_parseState(ParseJpegXLNotParsed)
    , m_quality(90)
    , m_currentimage_index(0)
    , m_previousimage_index(-1)
    , m_decoder(nullptr)
    , m_runner(nullptr)
    , m_next_image_delay(0)
    , m_input_image_format(QImage::Format_Invalid)
    , m_target_image_format(QImage::Format_Invalid)
    , m_buffer_size(0)
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

bool QJpegXLHandler::ensureALLCounted() const
{
    if (!ensureParsed()) {
        return false;
    }

    if (m_parseState == ParseJpegXLSuccess) {
        return true;
    }

    QJpegXLHandler *that = const_cast<QJpegXLHandler *>(this);

    return that->countALLFrames();
}

bool QJpegXLHandler::ensureDecoder()
{
    if (m_decoder) {
        return true;
    }

    m_rawData = device()->readAll();

    if (m_rawData.isEmpty()) {
        return false;
    }

    JxlSignature signature = JxlSignatureCheck((const uint8_t *)m_rawData.constData(), m_rawData.size());
    if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER) {
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_decoder = JxlDecoderCreate(nullptr);
    if (!m_decoder) {
        qWarning("ERROR: JxlDecoderCreate failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    int num_worker_threads = QThread::idealThreadCount();
    if (!m_runner && num_worker_threads >= 4) {
        /* use half of the threads because plug-in is usually used in environment
         * where application performs another tasks in backround (pre-load other images) */
        num_worker_threads = num_worker_threads / 2;
        num_worker_threads = qBound(2, num_worker_threads, 64);
        m_runner = JxlThreadParallelRunnerCreate(nullptr, num_worker_threads);

        if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSetParallelRunner failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    if (JxlDecoderSetInput(m_decoder, (const uint8_t *)m_rawData.constData(), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderStatus status = JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME);
    if (status == JXL_DEC_ERROR) {
        qWarning("ERROR: JxlDecoderSubscribeEvents failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderProcessInput(m_decoder);
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
    } else if (sizeof(void *) <= 4) {
        /* On 32bit systems, there is limited address space.
         * We skip imagess bigger than 8192 x 8192 pixels.
         * If we don't do it, abort() in libjxl may close whole application */
        if ((m_basicinfo.xsize * m_basicinfo.ysize) > 67108864) {
            qWarning("JXL image (%dx%d) is too large for 32bit build of the plug-in", m_basicinfo.xsize, m_basicinfo.ysize);
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    m_parseState = ParseJpegXLBasicInfoParsed;
    return true;
}

bool QJpegXLHandler::countALLFrames()
{
    if (m_parseState != ParseJpegXLBasicInfoParsed) {
        return false;
    }

    JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder);
    if (status != JXL_DEC_COLOR_ENCODING) {
        qWarning("Unexpected event %d instead of JXL_DEC_COLOR_ENCODING", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlColorEncoding color_encoding;
    if (m_basicinfo.uses_original_profile == JXL_FALSE) {
        JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
        JxlDecoderSetPreferredColorProfile(m_decoder, &color_encoding);
    }

    bool loadalpha;

    if (m_basicinfo.alpha_bits > 0) {
        loadalpha = true;
    } else {
        loadalpha = false;
    }

    m_input_pixel_format.endianness = JXL_NATIVE_ENDIAN;
    m_input_pixel_format.align = 0;
    m_input_pixel_format.num_channels = 4;

    if (m_basicinfo.bits_per_sample > 8) { // high bit depth
        m_input_pixel_format.data_type = JXL_TYPE_UINT16;
        m_buffer_size = 8 * (size_t)m_basicinfo.xsize * (size_t)m_basicinfo.ysize;
        m_input_image_format = QImage::Format_RGBA64;

        if (loadalpha) {
            m_target_image_format = QImage::Format_RGBA64;
        } else {
            m_target_image_format = QImage::Format_RGBX64;
        }
    } else { // 8bit depth
        m_input_pixel_format.data_type = JXL_TYPE_UINT8;
        m_buffer_size = 4 * (size_t)m_basicinfo.xsize * (size_t)m_basicinfo.ysize;
        m_input_image_format = QImage::Format_RGBA8888;

        if (loadalpha) {
            m_target_image_format = QImage::Format_ARGB32;
        } else {
            m_target_image_format = QImage::Format_RGB32;
        }
    }

    status = JxlDecoderGetColorAsEncodedProfile(m_decoder, &m_input_pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, &color_encoding);

    if (status == JXL_DEC_SUCCESS && color_encoding.color_space == JXL_COLOR_SPACE_RGB && color_encoding.white_point == JXL_WHITE_POINT_D65
        && color_encoding.primaries == JXL_PRIMARIES_SRGB && color_encoding.transfer_function == JXL_TRANSFER_FUNCTION_SRGB) {
        m_colorspace = QColorSpace(QColorSpace::SRgb);
    } else {
        size_t icc_size = 0;
        if (JxlDecoderGetICCProfileSize(m_decoder, &m_input_pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size) == JXL_DEC_SUCCESS) {
            if (icc_size > 0) {
                QByteArray icc_data((int)icc_size, 0);
                if (JxlDecoderGetColorAsICCProfile(m_decoder, &m_input_pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, (uint8_t *)icc_data.data(), icc_data.size())
                    == JXL_DEC_SUCCESS) {
                    m_colorspace = QColorSpace::fromIccProfile(icc_data);

                    if (!m_colorspace.isValid()) {
                        qWarning("JXL image has Qt-unsupported or invalid ICC profile!");
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

    if (m_basicinfo.have_animation) { // count all frames
        JxlFrameHeader frame_header;
        int delay;

        for (status = JxlDecoderProcessInput(m_decoder); status != JXL_DEC_SUCCESS; status = JxlDecoderProcessInput(m_decoder)) {
            if (status != JXL_DEC_FRAME) {
                switch (status) {
                case JXL_DEC_ERROR:
                    qWarning("ERROR: JXL decoding failed");
                    break;
                case JXL_DEC_NEED_MORE_INPUT:
                    qWarning("ERROR: JXL data incomplete");
                    break;
                default:
                    qWarning("Unexpected event %d instead of JXL_DEC_FRAME", status);
                    break;
                }
                m_parseState = ParseJpegXLError;
                return false;
            }

            if (JxlDecoderGetFrameHeader(m_decoder, &frame_header) != JXL_DEC_SUCCESS) {
                qWarning("ERROR: JxlDecoderGetFrameHeader failed");
                m_parseState = ParseJpegXLError;
                return false;
            }

            if (m_basicinfo.animation.tps_denominator > 0 && m_basicinfo.animation.tps_numerator > 0) {
                delay = (int)(0.5 + 1000.0 * frame_header.duration * m_basicinfo.animation.tps_denominator / m_basicinfo.animation.tps_numerator);
            } else {
                delay = 0;
            }

            m_framedelays.append(delay);
        }

        if (m_framedelays.isEmpty()) {
            qWarning("no frames loaded by the JXL plug-in");
            m_parseState = ParseJpegXLError;
            return false;
        }

        if (m_framedelays.count() == 1) {
            qWarning("JXL file was marked as animation but it has only one frame.");
            m_basicinfo.have_animation = JXL_FALSE;
        }
    } else { // static picture
        m_framedelays.resize(1);
        m_framedelays[0] = 0;
    }

    if (!rewind()) {
        return false;
    }

    m_next_image_delay = m_framedelays[0];
    m_parseState = ParseJpegXLSuccess;
    return true;
}

bool QJpegXLHandler::decode_one_frame()
{
    JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder);
    if (status != JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        qWarning("Unexpected event %d instead of JXL_DEC_NEED_IMAGE_OUT_BUFFER", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_current_image = QImage(m_basicinfo.xsize, m_basicinfo.ysize, m_input_image_format);
    if (m_current_image.isNull()) {
        qWarning("Memory cannot be allocated");
        m_parseState = ParseJpegXLError;
        return false;
    }

    m_current_image.setColorSpace(m_colorspace);

    if (JxlDecoderSetImageOutBuffer(m_decoder, &m_input_pixel_format, m_current_image.bits(), m_buffer_size) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetImageOutBuffer failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    status = JxlDecoderProcessInput(m_decoder);
    if (status != JXL_DEC_FULL_IMAGE) {
        qWarning("Unexpected event %d instead of JXL_DEC_FULL_IMAGE", status);
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_target_image_format != m_input_image_format) {
        m_current_image.convertTo(m_target_image_format);
    }

    m_next_image_delay = m_framedelays[m_currentimage_index];
    m_previousimage_index = m_currentimage_index;

    if (m_framedelays.count() > 1) {
        m_currentimage_index++;

        if (m_currentimage_index >= m_framedelays.count()) {
            if (!rewind()) {
                return false;
            }
        }
    }

    return true;
}

bool QJpegXLHandler::read(QImage *image)
{
    if (!ensureALLCounted()) {
        return false;
    }

    if (m_currentimage_index == m_previousimage_index) {
        *image = m_current_image;
        return jumpToNextImage();
    }

    if (decode_one_frame()) {
        *image = m_current_image;
        return true;
    } else {
        return false;
    }
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

    JxlEncoder *encoder = JxlEncoderCreate(nullptr);
    if (!encoder) {
        qWarning("Failed to create Jxl encoder");
        return false;
    }

    void *runner = nullptr;
    int num_worker_threads = qBound(1, QThread::idealThreadCount(), 64);

    if (num_worker_threads > 1) {
        runner = JxlThreadParallelRunnerCreate(nullptr, num_worker_threads);
        if (JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, runner) != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetParallelRunner failed");
            JxlThreadParallelRunnerDestroy(runner);
            JxlEncoderDestroy(encoder);
            return false;
        }
    }

    JxlEncoderOptions *encoder_options = JxlEncoderOptionsCreate(encoder, nullptr);

    if (m_quality > 100) {
        m_quality = 100;
    } else if (m_quality < 0) {
        m_quality = 90;
    }

    JxlEncoderOptionsSetDistance(encoder_options, (100.0f - m_quality) / 10.0f);

    JxlEncoderOptionsSetLossless(encoder_options, (m_quality == 100) ? JXL_TRUE : JXL_FALSE);

    JxlBasicInfo output_info;
    JxlEncoderInitBasicInfo(&output_info);

    JxlColorEncoding color_profile;
    JxlColorEncodingSetToSRGB(&color_profile, JXL_FALSE);

    bool convert_color_profile;
    QByteArray iccprofile;

    if (image.colorSpace().isValid()) {
        if (image.colorSpace().primaries() != QColorSpace::Primaries::SRgb || image.colorSpace().transferFunction() != QColorSpace::TransferFunction::SRgb) {
            convert_color_profile = true;
        } else {
            convert_color_profile = false;
        }
    } else { // no profile or Qt-unsupported ICC profile
        convert_color_profile = false;
        iccprofile = image.colorSpace().iccProfile();
        if (iccprofile.size() > 0) {
            output_info.uses_original_profile = 1;
        }
    }

    JxlPixelFormat pixel_format;
    QImage::Format tmpformat;
    JxlEncoderStatus status;

    pixel_format.data_type = JXL_TYPE_UINT16;
    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 0;

    if (image.hasAlphaChannel()) {
        tmpformat = QImage::Format_RGBA64;
        pixel_format.num_channels = 4;
        output_info.alpha_bits = 16;
        output_info.num_extra_channels = 1;
    } else {
        tmpformat = QImage::Format_RGBX64;
        pixel_format.num_channels = 3;
        output_info.alpha_bits = 0;
    }

    const QImage tmpimage =
        convert_color_profile ? image.convertToFormat(tmpformat).convertedToColorSpace(QColorSpace(QColorSpace::SRgb)) : image.convertToFormat(tmpformat);

    const size_t xsize = tmpimage.width();
    const size_t ysize = tmpimage.height();
    const size_t buffer_size = 2 * pixel_format.num_channels * xsize * ysize;

    if (xsize == 0 || ysize == 0 || tmpimage.isNull()) {
        qWarning("Unable to allocate memory for output image");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
    }

    output_info.xsize = tmpimage.width();
    output_info.ysize = tmpimage.height();
    output_info.bits_per_sample = 16;
    output_info.intensity_target = 255.0f;
    output_info.orientation = JXL_ORIENT_IDENTITY;
    output_info.num_color_channels = 3;
    output_info.animation.tps_numerator = 10;
    output_info.animation.tps_denominator = 1;

    status = JxlEncoderSetBasicInfo(encoder, &output_info);
    if (status != JXL_ENC_SUCCESS) {
        qWarning("JxlEncoderSetBasicInfo failed!");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
    }

    if (!convert_color_profile && iccprofile.size() > 0) {
        status = JxlEncoderSetICCProfile(encoder, (const uint8_t *)iccprofile.constData(), iccprofile.size());
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetICCProfile failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    } else {
        status = JxlEncoderSetColorEncoding(encoder, &color_profile);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetColorEncoding failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    }

    if (image.hasAlphaChannel()) {
        status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, (void *)tmpimage.constBits(), buffer_size);
    } else {
        uint16_t *tmp_buffer = new (std::nothrow) uint16_t[3 * xsize * ysize];
        if (!tmp_buffer) {
            qWarning("Memory allocation error");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        uint16_t *dest_pixels = tmp_buffer;
        for (int y = 0; y < tmpimage.height(); y++) {
            const uint16_t *src_pixels = reinterpret_cast<const uint16_t *>(tmpimage.constScanLine(y));
            for (int x = 0; x < tmpimage.width(); x++) {
                // R
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels++;
                // G
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels++;
                // B
                *dest_pixels = *src_pixels;
                dest_pixels++;
                src_pixels += 2; // skipalpha
            }
        }
        status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, (void *)tmp_buffer, buffer_size);
        delete[] tmp_buffer;
    }

    if (status == JXL_ENC_ERROR) {
        qWarning("JxlEncoderAddImageFrame failed!");
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
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
        avail_out = compressed.size() - offset;
        status = JxlEncoderProcessOutput(encoder, &next_out, &avail_out);

        if (status == JXL_ENC_NEED_MORE_OUTPUT) {
            offset = next_out - compressed.data();
            compressed.resize(compressed.size() * 2);
        } else if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderProcessOutput failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
    } while (status != JXL_ENC_SUCCESS);

    if (runner) {
        JxlThreadParallelRunnerDestroy(runner);
    }
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
    if (option == Quality) {
        return m_quality;
    }

    if (!supportsOption(option) || !ensureParsed()) {
        return QVariant();
    }

    switch (option) {
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
            m_quality = 90;
        }
        return;
    default:
        break;
    }
    QImageIOHandler::setOption(option, value);
}

bool QJpegXLHandler::supportsOption(ImageOption option) const
{
    return option == Quality || option == Size || option == Animation;
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

        if (!ensureALLCounted()) {
            return 0;
        }
    }

    if (!m_framedelays.isEmpty()) {
        return m_framedelays.count();
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
    if (!ensureALLCounted()) {
        return false;
    }

    if (m_framedelays.count() > 1) {
        m_currentimage_index++;

        if (m_currentimage_index >= m_framedelays.count()) {
            if (!rewind()) {
                return false;
            }
        } else {
            JxlDecoderSkipFrames(m_decoder, 1);
        }
    }

    return true;
}

bool QJpegXLHandler::jumpToImage(int imageNumber)
{
    if (!ensureALLCounted()) {
        return false;
    }

    if (imageNumber < 0 || imageNumber >= m_framedelays.count()) {
        return false;
    }

    if (imageNumber == m_currentimage_index) {
        return true;
    }

    if (imageNumber > m_currentimage_index) {
        JxlDecoderSkipFrames(m_decoder, imageNumber - m_currentimage_index);
        m_currentimage_index = imageNumber;
        return true;
    }

    if (!rewind()) {
        return false;
    }

    if (imageNumber > 0) {
        JxlDecoderSkipFrames(m_decoder, imageNumber);
    }
    m_currentimage_index = imageNumber;
    return true;
}

int QJpegXLHandler::nextImageDelay() const
{
    if (!ensureALLCounted()) {
        return 0;
    }

    if (m_framedelays.count() < 2) {
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

bool QJpegXLHandler::rewind()
{
    m_currentimage_index = 0;

    JxlDecoderReleaseInput(m_decoder);
    JxlDecoderRewind(m_decoder);
    if (m_runner) {
        if (JxlDecoderSetParallelRunner(m_decoder, JxlThreadParallelRunner, m_runner) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSetParallelRunner failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    }

    if (JxlDecoderSetInput(m_decoder, (const uint8_t *)m_rawData.constData(), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (m_basicinfo.uses_original_profile) {
        if (JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSubscribeEvents failed");
            m_parseState = ParseJpegXLError;
            return false;
        }
    } else {
        if (JxlDecoderSubscribeEvents(m_decoder, JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
            qWarning("ERROR: JxlDecoderSubscribeEvents failed");
            m_parseState = ParseJpegXLError;
            return false;
        }

        JxlDecoderStatus status = JxlDecoderProcessInput(m_decoder);
        if (status != JXL_DEC_COLOR_ENCODING) {
            qWarning("Unexpected event %d instead of JXL_DEC_COLOR_ENCODING", status);
            m_parseState = ParseJpegXLError;
            return false;
        }

        JxlColorEncoding color_encoding;
        JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
        JxlDecoderSetPreferredColorProfile(m_decoder, &color_encoding);
    }

    return true;
}
