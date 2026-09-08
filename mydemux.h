#ifndef MYDEMUX_H
#define MYDEMUX_H
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
    virtual AVPacket *read();
    virtual PacketType packetType(const AVPacket *packet);
    // 返回新分配的视频编码参数，所有权交给调用者。
    // 当前 MyDecode::open 会接管并释放它。
    virtual AVCodecParameters *getVideoParameters();



    virtual AVCodecParameters *getAudioParametes();

    // 返回容器推测的视频帧率；无法确定时返回 0。
    virtual double videoFrameRate();
    virtual bool Seek(double pos);
protected:
    AVFormatContext * format = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    int64_t totalMs = 0;
    std::mutex mtx_;
};

#endif // MYDEMUX_H
