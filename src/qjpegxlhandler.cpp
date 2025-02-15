/*
 * QT plug-in to allow import/export in JPEG XL image format.
 * Author: Daniel Novomesky
 */

#include <QThread>
#include <QtGlobal>

#include "qjpegxlhandler_p.h"
#include "util_p.h"

#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

#if JPEGXL_NUMERIC_VERSION > JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
#include <jxl/cms.h>
#endif

#include <string.h>

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

        if (m_parseState == ParseJpegXLFinished) {
            return false;
        }

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

    JxlSignature signature = JxlSignatureCheck(reinterpret_cast<const uint8_t *>(header.constData()), header.size());
    if (signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER) {
        return true;
    }
    return false;
}

bool QJpegXLHandler::ensureParsed() const
{
    if (m_parseState == ParseJpegXLSuccess || m_parseState == ParseJpegXLBasicInfoParsed || m_parseState == ParseJpegXLFinished) {
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

    if (m_parseState == ParseJpegXLSuccess || m_parseState == ParseJpegXLFinished) {
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

    JxlSignature signature = JxlSignatureCheck(reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size());
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

    if (JxlDecoderSetInput(m_decoder, reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderCloseInput(m_decoder);

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

    if (m_basicinfo.xsize > 262144 || m_basicinfo.ysize > 262144) {
        qWarning("JXL image (%dx%d) is too large", m_basicinfo.xsize, m_basicinfo.ysize);
        m_parseState = ParseJpegXLError;
        return false;
    }

    if (sizeof(void *) <= 4) {
        /* On 32bit systems, there is limited address space.
         * We skip imagess bigger than 8192 x 8192 pixels.
         * If we don't do it, abort() in libjxl may close whole application */
        if (m_basicinfo.xsize > ((8192 * 8192) / m_basicinfo.ysize)) {
            qWarning("JXL image (%dx%d) is too large for 32bit build of the plug-in", m_basicinfo.xsize, m_basicinfo.ysize);
            m_parseState = ParseJpegXLError;
            return false;
        }
    } else {
        /* On 64bit systems
         * We skip images bigger than 16384 x 16384 pixels.
         * It is an artificial limit not to use extreme amount of memory */
        if (m_basicinfo.xsize > ((16384 * 16384) / m_basicinfo.ysize)) {
            qWarning("JXL image (%dx%d) is bigger than security limit 256 megapixels", m_basicinfo.xsize, m_basicinfo.ysize);
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
    if (m_basicinfo.uses_original_profile == JXL_FALSE && m_basicinfo.have_animation == JXL_FALSE) {
#if JPEGXL_NUMERIC_VERSION > JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
        const JxlCmsInterface *jxlcms = JxlGetDefaultCms();
        if (jxlcms) {
            status = JxlDecoderSetCms(m_decoder, *jxlcms);
            if (status != JXL_DEC_SUCCESS) {
                qWarning("JxlDecoderSetCms ERROR");
            }
        } else {
            qWarning("No JPEG XL CMS Interface");
        }
#endif
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

    status = JxlDecoderGetColorAsEncodedProfile(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                                &m_input_pixel_format,
#endif
                                                JXL_COLOR_PROFILE_TARGET_DATA,
                                                &color_encoding);

    if (status == JXL_DEC_SUCCESS && color_encoding.color_space == JXL_COLOR_SPACE_RGB && color_encoding.white_point == JXL_WHITE_POINT_D65
        && color_encoding.primaries == JXL_PRIMARIES_SRGB && color_encoding.transfer_function == JXL_TRANSFER_FUNCTION_SRGB) {
        m_colorspace = QColorSpace(QColorSpace::SRgb);
    } else {
        size_t icc_size = 0;
        if (JxlDecoderGetICCProfileSize(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                        &m_input_pixel_format,
#endif
                                        JXL_COLOR_PROFILE_TARGET_DATA,
                                        &icc_size)
            == JXL_DEC_SUCCESS) {
            if (icc_size > 0) {
                QByteArray icc_data(icc_size, 0);
                if (JxlDecoderGetColorAsICCProfile(m_decoder,
#if JPEGXL_NUMERIC_VERSION < JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
                                                   &m_input_pixel_format,
#endif
                                                   JXL_COLOR_PROFILE_TARGET_DATA,
                                                   reinterpret_cast<uint8_t *>(icc_data.data()),
                                                   icc_data.size())
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

    m_current_image = imageAlloc(m_basicinfo.xsize, m_basicinfo.ysize, m_input_image_format);
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

            // all frames in animation have been read
            m_parseState = ParseJpegXLFinished;
        } else {
            m_parseState = ParseJpegXLSuccess;
        }
    } else {
        // the static image has been read
        m_parseState = ParseJpegXLFinished;
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

    if ((image.width() > 0) && (image.height() > 0)) {
        if ((image.width() > 262144) || (image.height() > 262144)) {
            qWarning("Image (%dx%d) is too large to save!", image.width(), image.height());
            return false;
        }

        if (sizeof(void *) <= 4) {
            if (image.width() > ((8192 * 8192) / image.height())) {
                qWarning("Image (%dx%d) is too large save via 32bit build of JXL plug-in", image.width(), image.height());
                return false;
            }
        } else {
            if (image.width() > ((16384 * 16384) / image.height())) {
                qWarning("Image (%dx%d) will not be saved because it has more than 256 megapixels", image.width(), image.height());
                return false;
            }
        }
    } else {
        qWarning("Image has zero dimension!");
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

    if (m_quality > 100) {
        m_quality = 100;
    } else if (m_quality < 0) {
        m_quality = 90;
    }

    JxlBasicInfo output_info;
    JxlEncoderInitBasicInfo(&output_info);

    output_info.animation.tps_numerator = 10;
    output_info.animation.tps_denominator = 1;
    output_info.orientation = JXL_ORIENT_IDENTITY;

    bool save_cmyk = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (image.format() == QImage::Format_CMYK8888 && image.colorSpace().isValid() && image.colorSpace().colorModel() == QColorSpace::ColorModel::Cmyk) {
        save_cmyk = true;
    }
#endif

    JxlEncoderStatus status;
    JxlPixelFormat pixel_format;
    pixel_format.endianness = JXL_NATIVE_ENDIAN;
    pixel_format.align = 0;

    if (save_cmyk) { // CMYK is always lossless
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        JxlEncoderUseContainer(encoder, JXL_TRUE);
        JxlEncoderSetCodestreamLevel(encoder, 10);

        output_info.have_container = JXL_TRUE;
        output_info.uses_original_profile = JXL_TRUE;
        output_info.xsize = image.width();
        output_info.ysize = image.height();
        output_info.num_color_channels = 3;
        output_info.bits_per_sample = 8;
        output_info.alpha_bits = 0;
        output_info.num_extra_channels = 1;

        pixel_format.num_channels = 3;
        pixel_format.data_type = JXL_TYPE_UINT8;

        JxlPixelFormat format_extra;
        format_extra.num_channels = 1;
        format_extra.data_type = JXL_TYPE_UINT8;
        format_extra.endianness = JXL_NATIVE_ENDIAN;
        format_extra.align = 0;

        JxlExtraChannelInfo extra_black_channel;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_BLACK, &extra_black_channel);
        extra_black_channel.bits_per_sample = output_info.bits_per_sample;
        extra_black_channel.exponent_bits_per_sample = output_info.exponent_bits_per_sample;

        const QByteArray cmyk_profile = image.colorSpace().iccProfile();
        if (cmyk_profile.isEmpty()) {
            qWarning("ERROR saving CMYK JXL: empty ICC profile");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        status = JxlEncoderSetBasicInfo(encoder, &output_info);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetBasicInfo for CMYK image failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        status = JxlEncoderSetExtraChannelInfo(encoder, 0, &extra_black_channel);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetExtraChannelInfo for CMYK image failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        status = JxlEncoderSetICCProfile(encoder, reinterpret_cast<const uint8_t *>(cmyk_profile.constData()), cmyk_profile.size());
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetICCProfile for CMYK image failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        const size_t extra_buffer_size = size_t(image.width()) * size_t(image.height());
        const size_t cmy_buffer_size = extra_buffer_size * 3;

        uchar *pixels_cmy = nullptr;
        uchar *pixels_black = nullptr;

        pixels_cmy = reinterpret_cast<uchar *>(malloc(cmy_buffer_size));
        if (!pixels_cmy) {
            qWarning("Memory cannot be allocated for CMY buffer");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        pixels_black = reinterpret_cast<uchar *>(malloc(extra_buffer_size));
        if (!pixels_black) {
            qWarning("Memory cannot be allocated for BLACK buffer");
            free(pixels_cmy);
            pixels_cmy = nullptr;

            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        uchar *dest_CMY = pixels_cmy;
        uchar *dest_K = pixels_black;
        for (int y = 0; y < image.height(); y++) {
            const uchar *src_CMYK = image.constScanLine(y);
            for (int x = 0; x < image.width(); x++) {
                *dest_CMY = 255 - *src_CMYK; // C
                dest_CMY++;
                src_CMYK++;
                *dest_CMY = 255 - *src_CMYK; // M
                dest_CMY++;
                src_CMYK++;
                *dest_CMY = 255 - *src_CMYK; // Y
                dest_CMY++;
                src_CMYK++;
                *dest_K = 255 - *src_CMYK; // K
                dest_K++;
                src_CMYK++;
            }
        }

        JxlEncoderFrameSettings *frame_settings_lossless = JxlEncoderFrameSettingsCreate(encoder, nullptr);
        JxlEncoderSetFrameDistance(frame_settings_lossless, 0);
        JxlEncoderSetFrameLossless(frame_settings_lossless, JXL_TRUE);

        status = JxlEncoderAddImageFrame(frame_settings_lossless, &pixel_format, pixels_cmy, cmy_buffer_size);
        if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderAddImageFrame failed!");
            free(pixels_black);
            pixels_black = nullptr;
            free(pixels_cmy);
            pixels_cmy = nullptr;
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        status = JxlEncoderSetExtraChannelBuffer(frame_settings_lossless, &format_extra, pixels_black, extra_buffer_size, 0);

        free(pixels_black);
        pixels_black = nullptr;
        free(pixels_cmy);
        pixels_cmy = nullptr;

        if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderSetExtraChannelBuffer failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
#else
        if (runner) {
            JxlThreadParallelRunnerDestroy(runner);
        }
        JxlEncoderDestroy(encoder);
        return false;
#endif
    } else { // RGB or GRAY saving
        int save_depth = 8; // 8 / 16 / 32
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
        bool save_fp = false;
#endif
        bool is_gray = false;
        // depth detection
        switch (image.format()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
        case QImage::Format_RGBX32FPx4:
        case QImage::Format_RGBA32FPx4:
        case QImage::Format_RGBA32FPx4_Premultiplied:
            save_depth = 32;
            save_fp = true;
            break;
        case QImage::Format_RGBX16FPx4:
        case QImage::Format_RGBA16FPx4:
        case QImage::Format_RGBA16FPx4_Premultiplied:
            save_depth = 16;
            save_fp = true;
            break;
#endif
        case QImage::Format_BGR30:
        case QImage::Format_A2BGR30_Premultiplied:
        case QImage::Format_RGB30:
        case QImage::Format_A2RGB30_Premultiplied:
        case QImage::Format_RGBX64:
        case QImage::Format_RGBA64:
        case QImage::Format_RGBA64_Premultiplied:
            save_depth = 16;
            break;
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
        case QImage::Format_RGB888:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        case QImage::Format_CMYK8888:
#endif
            save_depth = 8;
            break;
        case QImage::Format_Grayscale16:
            save_depth = 16;
            is_gray = true;
            break;
        case QImage::Format_Grayscale8:
        case QImage::Format_Alpha8:
        case QImage::Format_Mono:
        case QImage::Format_MonoLSB:
            save_depth = 8;
            is_gray = true;
            break;
        case QImage::Format_Indexed8:
            save_depth = 8;
            is_gray = image.isGrayscale();
            break;
        default:
            if (image.depth() > 32) {
                save_depth = 16;
            } else {
                save_depth = 8;
            }
            break;
        }

        QImage::Format tmpformat;

        if (save_depth > 8 && is_gray) { // 16bit depth gray
            pixel_format.data_type = JXL_TYPE_UINT16;
            output_info.num_color_channels = 1;
            output_info.bits_per_sample = 16;
            tmpformat = QImage::Format_Grayscale16;
            pixel_format.num_channels = 1;
        } else if (is_gray) { // 8bit depth gray
            pixel_format.data_type = JXL_TYPE_UINT8;
            output_info.num_color_channels = 1;
            output_info.bits_per_sample = 8;
            tmpformat = QImage::Format_Grayscale8;
            pixel_format.num_channels = 1;
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
        } else if (save_depth > 16) { // 32bit depth rgb
            pixel_format.data_type = JXL_TYPE_FLOAT;
            output_info.exponent_bits_per_sample = 8;
            output_info.num_color_channels = 3;
            output_info.bits_per_sample = 32;

            if (image.hasAlphaChannel()) {
                tmpformat = QImage::Format_RGBA32FPx4;
                pixel_format.num_channels = 4;
                output_info.alpha_bits = 32;
                output_info.alpha_exponent_bits = 8;
                output_info.num_extra_channels = 1;
            } else {
                tmpformat = QImage::Format_RGBX32FPx4;
                pixel_format.num_channels = 3;
                output_info.alpha_bits = 0;
                output_info.num_extra_channels = 0;
            }
#endif
        } else if (save_depth > 8) { // 16bit depth rgb
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
            pixel_format.data_type = save_fp ? JXL_TYPE_FLOAT16 : JXL_TYPE_UINT16;
            output_info.exponent_bits_per_sample = save_fp ? 5 : 0;
#else
            pixel_format.data_type = JXL_TYPE_UINT16;
            output_info.exponent_bits_per_sample = 0;
#endif
            output_info.num_color_channels = 3;
            output_info.bits_per_sample = 16;

            if (image.hasAlphaChannel()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
                tmpformat = save_fp ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA64;
                output_info.alpha_exponent_bits = save_fp ? 5 : 0;
#else
                tmpformat = QImage::Format_RGBA64;
                output_info.alpha_exponent_bits = 0;
#endif
                pixel_format.num_channels = 4;
                output_info.alpha_bits = 16;
                output_info.num_extra_channels = 1;
            } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
                tmpformat = save_fp ? QImage::Format_RGBX16FPx4 : QImage::Format_RGBX64;
#else
                tmpformat = QImage::Format_RGBX64;
#endif
                pixel_format.num_channels = 3;
                output_info.alpha_bits = 0;
                output_info.num_extra_channels = 0;
            }
        } else { // 8bit depth rgb
            pixel_format.data_type = JXL_TYPE_UINT8;
            output_info.num_color_channels = 3;
            output_info.bits_per_sample = 8;

            if (image.hasAlphaChannel()) {
                tmpformat = QImage::Format_RGBA8888;
                pixel_format.num_channels = 4;
                output_info.alpha_bits = 8;
                output_info.num_extra_channels = 1;
            } else {
                tmpformat = QImage::Format_RGB888;
                pixel_format.num_channels = 3;
                output_info.alpha_bits = 0;
                output_info.num_extra_channels = 0;
            }
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QImage tmpimage;
        if (image.colorSpace().isValid()) {
            if (is_gray && image.colorSpace().colorModel() != QColorSpace::ColorModel::Gray) {
                // convert to Gray profile
                QPointF gray_whitePoint = image.colorSpace().whitePoint();
                if (gray_whitePoint.isNull()) {
                    gray_whitePoint = QPointF(0.3127f, 0.329f);
                }

                QColorSpace::TransferFunction gray_trc = image.colorSpace().transferFunction();
                float gamma_gray = image.colorSpace().gamma();
                if (gray_trc == QColorSpace::TransferFunction::Custom) {
                    gray_trc = QColorSpace::TransferFunction::SRgb;
                }

                const QColorSpace gray_profile(gray_whitePoint, gray_trc, gamma_gray);
                if (gray_profile.isValid()) {
                    tmpimage = image.convertedToColorSpace(gray_profile, tmpformat);
                } else {
                    qWarning("JXL plugin created invalid grayscale QColorSpace!");
                    tmpimage = image.convertToFormat(tmpformat);
                }
            } else if (!is_gray && image.colorSpace().colorModel() != QColorSpace::ColorModel::Rgb) {
                // convert to RGB profile
                QPointF whitePoint = image.colorSpace().whitePoint();
                if (whitePoint.isNull()) {
                    whitePoint = QPointF(0.3127f, 0.329f);
                }

                const QPointF redP(0.64f, 0.33f);
                const QPointF greenP(0.3f, 0.6f);
                const QPointF blueP(0.15f, 0.06f);

                QColorSpace::TransferFunction trc_rgb = image.colorSpace().transferFunction();
                float gamma_rgb = image.colorSpace().gamma();
                if (trc_rgb == QColorSpace::TransferFunction::Custom) {
                    trc_rgb = QColorSpace::TransferFunction::SRgb;
                }

                const QColorSpace rgb_profile(whitePoint, redP, greenP, blueP, trc_rgb, gamma_rgb);
                if (rgb_profile.isValid()) {
                    tmpimage = image.convertedToColorSpace(rgb_profile, tmpformat);
                } else {
                    qWarning("JXL plugin created invalid RGB QColorSpace!");
                    tmpimage = image.convertToFormat(tmpformat);
                }
            } else { // ColorSpace matches the format
                tmpimage = image.convertToFormat(tmpformat);
            }
        } else { // no ColorSpace or invalid
            tmpimage = image.convertToFormat(tmpformat);
        }
#else
        QImage tmpimage = image.convertToFormat(tmpformat);
#endif

        output_info.xsize = tmpimage.width();
        output_info.ysize = tmpimage.height();

        if (output_info.xsize == 0 || output_info.ysize == 0 || tmpimage.isNull()) {
            qWarning("Unable to allocate memory for output image");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        JxlColorEncoding color_profile;
        JxlColorEncodingSetToSRGB(&color_profile, is_gray ? JXL_TRUE : JXL_FALSE);

        QByteArray iccprofile;

        if (m_quality == 100) { // try to use ICC for lossless
            output_info.uses_original_profile = JXL_TRUE;
            iccprofile = tmpimage.colorSpace().iccProfile();
        } else { // try to detect encoded profile (smaller than ICC)
            output_info.uses_original_profile = JXL_FALSE;

            if (tmpimage.colorSpace().isValid()) {
                QPointF whiteP(0.3127f, 0.329f);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                whiteP = image.colorSpace().whitePoint();
#endif

                switch (tmpimage.colorSpace().primaries()) {
                case QColorSpace::Primaries::SRgb:
                    color_profile.white_point = JXL_WHITE_POINT_D65;
                    color_profile.primaries = JXL_PRIMARIES_SRGB;
                    break;
                case QColorSpace::Primaries::AdobeRgb:
                    color_profile.white_point = JXL_WHITE_POINT_D65;
                    color_profile.primaries = JXL_PRIMARIES_CUSTOM;
                    color_profile.primaries_red_xy[0] = 0.640;
                    color_profile.primaries_red_xy[1] = 0.330;
                    color_profile.primaries_green_xy[0] = 0.210;
                    color_profile.primaries_green_xy[1] = 0.710;
                    color_profile.primaries_blue_xy[0] = 0.150;
                    color_profile.primaries_blue_xy[1] = 0.060;
                    break;
                case QColorSpace::Primaries::DciP3D65:
                    color_profile.white_point = JXL_WHITE_POINT_D65;
                    color_profile.primaries = JXL_PRIMARIES_P3;
                    color_profile.primaries_red_xy[0] = 0.680;
                    color_profile.primaries_red_xy[1] = 0.320;
                    color_profile.primaries_green_xy[0] = 0.265;
                    color_profile.primaries_green_xy[1] = 0.690;
                    color_profile.primaries_blue_xy[0] = 0.150;
                    color_profile.primaries_blue_xy[1] = 0.060;
                    break;
                case QColorSpace::Primaries::ProPhotoRgb:
                    color_profile.white_point = JXL_WHITE_POINT_CUSTOM;
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
                    whiteP = QPointF(0.3457f, 0.3585f);
#endif
                    color_profile.white_point_xy[0] = whiteP.x();
                    color_profile.white_point_xy[1] = whiteP.y();
                    color_profile.primaries = JXL_PRIMARIES_CUSTOM;
                    color_profile.primaries_red_xy[0] = 0.7347;
                    color_profile.primaries_red_xy[1] = 0.2653;
                    color_profile.primaries_green_xy[0] = 0.1596;
                    color_profile.primaries_green_xy[1] = 0.8404;
                    color_profile.primaries_blue_xy[0] = 0.0366;
                    color_profile.primaries_blue_xy[1] = 0.0001;
                    break;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                case QColorSpace::Primaries::Bt2020:
                    color_profile.white_point = JXL_WHITE_POINT_D65;
                    color_profile.primaries = JXL_PRIMARIES_2100;
                    color_profile.primaries_red_xy[0] = 0.708;
                    color_profile.primaries_red_xy[1] = 0.292;
                    color_profile.primaries_green_xy[0] = 0.170;
                    color_profile.primaries_green_xy[1] = 0.797;
                    color_profile.primaries_blue_xy[0] = 0.131;
                    color_profile.primaries_blue_xy[1] = 0.046;
                    break;
#endif
                default:
                    if (is_gray && !whiteP.isNull()) {
                        color_profile.white_point = JXL_WHITE_POINT_CUSTOM;
                        color_profile.white_point_xy[0] = whiteP.x();
                        color_profile.white_point_xy[1] = whiteP.y();
                    } else {
                        iccprofile = tmpimage.colorSpace().iccProfile();
                    }
                    break;
                }

                if (iccprofile.isEmpty()) {
                    const double gamma_profile = tmpimage.colorSpace().gamma();

                    switch (tmpimage.colorSpace().transferFunction()) {
                    case QColorSpace::TransferFunction::Linear:
                        color_profile.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                        break;
                    case QColorSpace::TransferFunction::Gamma:
                        if (gamma_profile > 0) {
                            color_profile.transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
                            color_profile.gamma = 1.0 / gamma_profile;
                        } else {
                            iccprofile = tmpimage.colorSpace().iccProfile();
                        }
                        break;
                    case QColorSpace::TransferFunction::SRgb:
                        color_profile.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                        break;
                    default:
                        iccprofile = tmpimage.colorSpace().iccProfile();
                        break;
                    }
                }
            }
        }

        if ((save_depth > 8) && (image.hasAlphaChannel() || output_info.uses_original_profile)) {
            output_info.have_container = JXL_TRUE;
            JxlEncoderUseContainer(encoder, JXL_TRUE);
        }

        status = JxlEncoderSetBasicInfo(encoder, &output_info);
        if (status != JXL_ENC_SUCCESS) {
            qWarning("JxlEncoderSetBasicInfo failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }

        if (iccprofile.size() > 0) {
            status = JxlEncoderSetICCProfile(encoder, reinterpret_cast<const uint8_t *>(iccprofile.constData()), iccprofile.size());
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

        JxlEncoderFrameSettings *encoder_options = JxlEncoderFrameSettingsCreate(encoder, nullptr);

        if (m_quality == 100) { // lossless
            JxlEncoderSetFrameDistance(encoder_options, 0.0f);
            JxlEncoderSetFrameLossless(encoder_options, JXL_TRUE);
        } else {
            JxlEncoderSetFrameDistance(encoder_options, JxlEncoderDistanceFromQuality(m_quality));
            JxlEncoderSetFrameLossless(encoder_options, JXL_FALSE);
        }

        size_t buffer_size;
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
        if (tmpimage.format() == QImage::Format_RGBX32FPx4) { // pack 32-bit depth RGBX -> RGB
            buffer_size = 12 * size_t(tmpimage.width()) * size_t(tmpimage.height());

            float *packed_pixels32 = reinterpret_cast<float *>(malloc(buffer_size));
            if (!packed_pixels32) {
                qWarning("ERROR: JXL plug-in failed to allocate memory");
                return false;
            }

            float *dest_pixels32 = packed_pixels32;
            for (int y = 0; y < tmpimage.height(); y++) {
                const float *src_pixels32 = reinterpret_cast<const float *>(tmpimage.constScanLine(y));
                for (int x = 0; x < tmpimage.width(); x++) {
                    *dest_pixels32 = *src_pixels32; // R
                    dest_pixels32++;
                    src_pixels32++;
                    *dest_pixels32 = *src_pixels32; // G
                    dest_pixels32++;
                    src_pixels32++;
                    *dest_pixels32 = *src_pixels32; // B
                    dest_pixels32++;
                    src_pixels32 += 2; // skip X
                }
            }

            status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, packed_pixels32, buffer_size);
            free(packed_pixels32);
        } else if (tmpimage.format() == QImage::Format_RGBX16FPx4 || tmpimage.format() == QImage::Format_RGBX64) {
#else
        if (tmpimage.format() == QImage::Format_RGBX64) {
#endif
            // pack 16-bit depth RGBX -> RGB
            buffer_size = 6 * size_t(tmpimage.width()) * size_t(tmpimage.height());

            quint16 *packed_pixels16 = reinterpret_cast<quint16 *>(malloc(buffer_size));
            if (!packed_pixels16) {
                qWarning("ERROR: JXL plug-in failed to allocate memory");
                return false;
            }

            quint16 *dest_pixels16 = packed_pixels16;
            for (int y = 0; y < tmpimage.height(); y++) {
                const quint16 *src_pixels16 = reinterpret_cast<const quint16 *>(tmpimage.constScanLine(y));
                for (int x = 0; x < tmpimage.width(); x++) {
                    *dest_pixels16 = *src_pixels16; // R
                    dest_pixels16++;
                    src_pixels16++;
                    *dest_pixels16 = *src_pixels16; // G
                    dest_pixels16++;
                    src_pixels16++;
                    *dest_pixels16 = *src_pixels16; // B
                    dest_pixels16++;
                    src_pixels16 += 2; // skip X
                }
            }

            status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, packed_pixels16, buffer_size);
            free(packed_pixels16);
        } else { // use QImage's data directly
            pixel_format.align = tmpimage.bytesPerLine();

            buffer_size = size_t(tmpimage.height() - 1) * size_t(tmpimage.bytesPerLine());
            switch (pixel_format.data_type) {
            case JXL_TYPE_FLOAT:
                buffer_size += 4 * size_t(pixel_format.num_channels) * size_t(tmpimage.width());
                break;
            case JXL_TYPE_UINT8:
                buffer_size += size_t(pixel_format.num_channels) * size_t(tmpimage.width());
                break;
            case JXL_TYPE_UINT16:
            case JXL_TYPE_FLOAT16:
                buffer_size += 2 * size_t(pixel_format.num_channels) * size_t(tmpimage.width());
                break;
            default:
                qWarning("ERROR: unsupported data type");
                return false;
                break;
            }

            status = JxlEncoderAddImageFrame(encoder_options, &pixel_format, tmpimage.constBits(), buffer_size);
        }

        if (status == JXL_ENC_ERROR) {
            qWarning("JxlEncoderAddImageFrame failed!");
            if (runner) {
                JxlThreadParallelRunnerDestroy(runner);
            }
            JxlEncoderDestroy(encoder);
            return false;
        }
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
        qint64 write_status = device()->write(reinterpret_cast<const char *>(compressed.data()), compressed.size());

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

    m_parseState = ParseJpegXLSuccess;
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
        m_parseState = ParseJpegXLSuccess;
        return true;
    }

    if (imageNumber > m_currentimage_index) {
        JxlDecoderSkipFrames(m_decoder, imageNumber - m_currentimage_index);
        m_currentimage_index = imageNumber;
        m_parseState = ParseJpegXLSuccess;
        return true;
    }

    if (!rewind()) {
        return false;
    }

    if (imageNumber > 0) {
        JxlDecoderSkipFrames(m_decoder, imageNumber);
    }
    m_currentimage_index = imageNumber;
    m_parseState = ParseJpegXLSuccess;
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
        return (m_basicinfo.animation.num_loops > 0) ? m_basicinfo.animation.num_loops - 1 : -1;
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

    if (JxlDecoderSetInput(m_decoder, reinterpret_cast<const uint8_t *>(m_rawData.constData()), m_rawData.size()) != JXL_DEC_SUCCESS) {
        qWarning("ERROR: JxlDecoderSetInput failed");
        m_parseState = ParseJpegXLError;
        return false;
    }

    JxlDecoderCloseInput(m_decoder);

    if (m_basicinfo.uses_original_profile || m_basicinfo.have_animation) {
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

#if JPEGXL_NUMERIC_VERSION > JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
        const JxlCmsInterface *jxlcms = JxlGetDefaultCms();
        if (jxlcms) {
            status = JxlDecoderSetCms(m_decoder, *jxlcms);
            if (status != JXL_DEC_SUCCESS) {
                qWarning("JxlDecoderSetCms ERROR");
            }
        } else {
            qWarning("No JPEG XL CMS Interface");
        }
#endif

        JxlColorEncoding color_encoding;
        JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
        JxlDecoderSetPreferredColorProfile(m_decoder, &color_encoding);
    }

    return true;
}
