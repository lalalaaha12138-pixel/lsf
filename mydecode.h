#ifndef MYDECODE_H
#define MYDECODE_H

extern "C" {
#include <libavutil/rational.h>
}

#include <mutex>
struct AVCodecParameters;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
class MyDecode
{
public:
    MyDecode();
    virtual ~MyDecode();

    // 打开与 para 对应的解码器，并接管 para 的所有权；无论成功失败都会释放它。
    // packetTimeBase 是压缩包 PTS/DTS 的时间基，解码器用它推导帧时间戳。
    virtual bool open(AVCodecParameters *para, AVRational packetTimeBase);

    // FFmpeg 新式解码流程：先 sendPacket 投递压缩包，再用 receiveFrame 取帧。
    // packet 为 nullptr 时表示输入结束，要求解码器输出内部缓存的延迟帧。
    virtual int sendPacket(const AVPacket *packet);
    virtual int receiveFrame(AVFrame *frame);

    // clear 仅清空解码缓存，close 会同时释放解码器上下文。
    virtual void clear();
    virtual void close();
protected:
     // 保护 codecCtx，便于后续将解码工作迁移到独立线程。
     std::mutex mut_;
     AVCodecContext *codecCtx = nullptr;
};

#endif // MYDECODE_H
