#include <nan.h>
#include "DecoderFF.h"
#include "Memory.h"
#include "Packers.h"

extern "C" {
  #include <libavutil/opt.h>
  #include <libavcodec/avcodec.h>
  #include <libavutil/imgutils.h>
}

namespace streampunk {

DecoderFF::DecoderFF(uint32_t width, uint32_t height)
  : mWidth(width), mHeight(height), mPixFmt((uint32_t)AV_PIX_FMT_YUV420P),
    mCodec(NULL), mContext(NULL), mFrame(NULL) {
}

DecoderFF::~DecoderFF() {
  av_frame_free(&mFrame);
  avcodec_close(mContext);
  av_free(mContext);
}

void DecoderFF::init(const std::string& srcFormat) {
  avcodec_register_all();
  av_log_set_level(AV_LOG_INFO);

  AVCodecID codecID = AV_CODEC_ID_NONE;
  if (!srcFormat.compare("h264"))
    codecID = AV_CODEC_ID_H264;
  else if (!srcFormat.compare("vp8"))
    codecID = AV_CODEC_ID_VP8;

  mCodec = avcodec_find_decoder(codecID);
  if (!mCodec) {
    std::string err = std::string("Decoder for format \'") + srcFormat.c_str() + "\' not found";
    return Nan::ThrowError(err.c_str());
  }

  mContext = avcodec_alloc_context3(mCodec);
  if (!mContext)
    return Nan::ThrowError("Could not allocate video codec context");

  mContext->width = mWidth;
  mContext->height = mHeight;
  mContext->refcounted_frames = 1;

  if (avcodec_open2(mContext, mCodec, NULL) < 0)
    return Nan::ThrowError("Could not open codec");

  mFrame = av_frame_alloc();
  if (!mFrame)
    return Nan::ThrowError("Could not allocate video frame");

  mSrcFormat = srcFormat;
}

uint32_t DecoderFF::bytesReq() const {
  return mWidth * mHeight * 3 / 2;
}

void DecoderFF::decodeFrame (const std::string& srcFormat, std::shared_ptr<Memory> srcBuf, std::shared_ptr<Memory> dstBuf, 
                             uint32_t frameNum, uint32_t *pDstBytes) {

  if (mSrcFormat != srcFormat) {
    init(srcFormat);      
  }

  AVPacket pkt;
  av_init_packet(&pkt);
  pkt.data = srcBuf->buf();
  pkt.size = srcBuf->numBytes();
  int got_output;
  int bytesUsed = avcodec_decode_video2(mContext, mFrame, &got_output, &pkt);

  if (bytesUsed <= 0) {
    printf("Failed to decode\n");
    *pDstBytes = 0;
  }
  else {
    uint32_t lumaBytes = mFrame->width * mFrame->height;
    uint32_t chromaBytes = lumaBytes / 4;
    memcpy(dstBuf->buf(), mFrame->data[0], lumaBytes);
    memcpy(dstBuf->buf() + lumaBytes, mFrame->data[1], chromaBytes);
    memcpy(dstBuf->buf() + lumaBytes + chromaBytes, mFrame->data[2], chromaBytes);
    *pDstBytes = lumaBytes + chromaBytes * 2;
  }
  
  av_packet_unref(&pkt);
  av_frame_unref(mFrame);
}

} // namespace streampunk