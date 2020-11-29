#ifndef QJPEGXLHANDLER_P_H
#define QJPEGXLHANDLER_P_H

#include <QImage>
#include <QVariant>
#include <qimageiohandler.h>
#include <QByteArray>

#include <jxl/decode.h>

class QJpegXLHandler : public QImageIOHandler
{
public:
  QJpegXLHandler();
  ~QJpegXLHandler();

  bool canRead() const override;
  bool read ( QImage *image ) override;
  bool write ( const QImage &image ) override;

  static bool canRead ( QIODevice *device );

  QVariant option ( ImageOption option ) const override;
  void setOption ( ImageOption option, const QVariant &value ) override;
  bool supportsOption ( ImageOption option ) const override;

  int imageCount() const override;
  int currentImageNumber() const override;
  bool jumpToNextImage() override;
  bool jumpToImage ( int imageNumber ) override;

  int nextImageDelay() const override;

  int loopCount() const override;
private:
  bool ensureParsed() const;
  bool ensureDecoder();
  bool decode_one_frame();

  enum ParseJpegXLState
  {
    ParseJpegXLError = -1,
    ParseJpegXLNotParsed = 0,
    ParseJpegXLSuccess = 1
  };

  ParseJpegXLState m_parseState;
  int m_quality;

  uint32_t m_container_width;
  uint32_t m_container_height;

  QByteArray m_rawData;

  JxlDecoder *m_decoder;
  void *m_runner;
  JxlBasicInfo m_basicinfo;
  JxlAnimationFrameHeader m_animation_frame_header;

  QImage        m_current_image;

  bool m_must_jump_to_next_image;
};

#endif // QJPEGXLHANDLER_P_H
