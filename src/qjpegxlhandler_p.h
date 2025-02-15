#ifndef QJPEGXLHANDLER_P_H
#define QJPEGXLHANDLER_P_H

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageIOHandler>
#include <QVariant>
#include <QVector>

#include <jxl/decode.h>

class QJpegXLHandler : public QImageIOHandler
{
public:
    QJpegXLHandler();
    ~QJpegXLHandler();

    bool canRead() const override;
    bool read(QImage *image) override;
    bool write(const QImage &image) override;

    static bool canRead(QIODevice *device);

    QVariant option(ImageOption option) const override;
    void setOption(ImageOption option, const QVariant &value) override;
    bool supportsOption(ImageOption option) const override;

    int imageCount() const override;
    int currentImageNumber() const override;
    bool jumpToNextImage() override;
    bool jumpToImage(int imageNumber) override;

    int nextImageDelay() const override;

    int loopCount() const override;

private:
    bool ensureParsed() const;
    bool ensureALLCounted() const;
    bool ensureDecoder();
    bool countALLFrames();
    bool decode_one_frame();
    bool rewind();

    enum ParseJpegXLState {
        ParseJpegXLError = -1,
        ParseJpegXLNotParsed = 0,
        ParseJpegXLSuccess = 1,
        ParseJpegXLBasicInfoParsed = 2,
        ParseJpegXLFinished = 3,
    };

    ParseJpegXLState m_parseState;
    int m_quality;
    int m_currentimage_index;
    int m_previousimage_index;

    QByteArray m_rawData;

    JxlDecoder *m_decoder;
    void *m_runner;
    JxlBasicInfo m_basicinfo;

    QVector<int> m_framedelays;
    int m_next_image_delay;

    QImage m_current_image;
    QColorSpace m_colorspace;
    bool m_isCMYK;
    uint32_t m_cmyk_channel_id;
    uint32_t m_alpha_channel_id;

    QImage::Format m_input_image_format;
    QImage::Format m_target_image_format;

    JxlPixelFormat m_input_pixel_format;
};

#endif // QJPEGXLHANDLER_P_H
