#ifndef MYDEMUX_H
#define MYDEMUX_H

extern "C" {
#include <libavutil/rational.h>
}

struct AVFormatContext;
struct AVPacket;
struct AVCodecParameters;
#include <mutex>
class MyDemux
{
public:
    enum class PacketType
    {
        Unknown,
        Video,
        Audio,
        Other
    };

    MyDemux();
    virtual ~MyDemux();
    virtual bool open(const char *url);
    virtual void close();
    virtual void clear();

    // 读取一个压缩数据包，并保留 av_read_frame() 的返回状态。
    // 返回 0 时，*packet 指向新分配的数据包，所有权交给调用者；
    // 返回负数时，*packet 保持为 nullptr，可用 AVERROR_EOF、
    // AVERROR(EAGAIN) 或其他 FFmpeg 错误码区分具体情况。
    virtual int read(AVPacket **packet);
    virtual PacketType packetType(const AVPacket *packet);
    // 返回新分配的视频编码参数，所有权交给调用者。
    // 当前 MyDecode::open 会接管并释放它。
    virtual AVCodecParameters *getVideoParameters();

    // 返回新分配的音频编码参数，所有权交给调用者。
    virtual AVCodecParameters *getAudioParameters();

    // 保留旧的拼写错误接口，避免已有调用代码失效。
    virtual AVCodecParameters *getAudioParametes();

    // 返回容器推测的视频帧率；无法确定时返回 0。
    virtual double videoFrameRate();

    // 返回对应流的时间基。PTS 只有结合时间基才能换算成实际时间。
    // 找不到对应流时返回无效时间基 {0, 1}。
    virtual AVRational videoTimeBase();
    virtual AVRational audioTimeBase();

    virtual bool Seek(double pos);
protected:
    AVFormatContext * format = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    int64_t totalMs = 0;
    std::mutex mtx_;
};

#endif // MYDEMUX_H
