#include "mydemux.h"

#include <cerrno>

extern "C"{
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/error.h>
    #include <libavcodec/avcodec.h>
}
//static double r2d(AVRational time_stamp){
//    return time_stamp.den == 0 ? 0 : time_stamp.num/time_stamp.den;
//}
MyDemux::MyDemux()
{
    static bool isFirst = true;
    static std::mutex mtx;
    std::unique_lock<std::mutex> stdLock(mtx);
    if (isFirst){
//        av_register_all();
        avformat_network_init();
        isFirst = false;
    }

}

MyDemux::~MyDemux()
{
    close();
}

void MyDemux::close()
{
    std::lock_guard<std::mutex> closeLock(mtx_);

    if (format) {
        avformat_close_input(&format);
    }

    videoStream = -1;
    audioStream = -1;
    totalMs = 0;
}

void MyDemux::clear()
{
    std::lock_guard<std::mutex> clearLock(mtx_);

    if (format) {
        avformat_flush(format);
    }
}

bool MyDemux::open(const char *url)
{
    std::unique_lock<std::mutex> openLock(this->mtx_);
    if (!url || !*url)
        return false;

    // 支持复用同一个 MyDemux 实例打开新文件。
    if (format)
        avformat_close_input(&format);
    videoStream = -1;
    audioStream = -1;
    totalMs = 0;

    int ret = avformat_open_input(&format,url,nullptr,nullptr);
    if (ret != 0){
        char buf[1024] = {0};
        av_strerror(ret,buf,sizeof(buf) -1);
        return false;
    }

    ret = avformat_find_stream_info(format,nullptr);
    if (ret < 0) {
        avformat_close_input(&format);
        return false;
    }

    // 让 FFmpeg 从容器中选择最合适的视频流和音频流。
    videoStream = av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    audioStream = av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
    if (videoStream < 0) {
        avformat_close_input(&format);
        audioStream = -1;
        return false;
    }

    if (format->duration != AV_NOPTS_VALUE)
        totalMs = format->duration/(AV_TIME_BASE/1000);

    return true;
}
int MyDemux::read(AVPacket **packet)
{
    // 输出参数无效时无法安全返回数据包。
    if (!packet)
        return AVERROR(EINVAL);

    // 失败路径必须保证调用方不会拿到旧指针。
    *packet = nullptr;

    std::unique_lock<std::mutex> readLock(this->mtx_);
    if (!format)
        return AVERROR(EINVAL);

    AVPacket *newPacket = av_packet_alloc();
    if (!newPacket)
        return AVERROR(ENOMEM);

    // 不把负数统一转换成 nullptr，而是把 FFmpeg 原始返回值交给上层。
    // 上层因此可以区分正常 EOF、暂时无数据和真正的读取错误。
    const int ret = av_read_frame(format, newPacket);
    if (ret < 0) {
        av_packet_free(&newPacket);
        return ret;
    }

    // av_read_frame() 返回 0 才会进入这里；从此由调用方释放数据包。
    *packet = newPacket;
    return 0;
}

MyDemux::PacketType MyDemux::packetType(const AVPacket *packet)
{
    std::lock_guard<std::mutex> typeLock(mtx_);

    if (!format || !packet || packet->stream_index < 0 ||
        packet->stream_index >= static_cast<int>(format->nb_streams)) {
        return PacketType::Unknown;
    }

    if (packet->stream_index == videoStream) {
        return PacketType::Video;
    }

    if (packet->stream_index == audioStream) {
        return PacketType::Audio;
    }

    return PacketType::Other;
}


AVCodecParameters *MyDemux::getVideoParameters()
{
    std::lock_guard<std::mutex> parameterLock(mtx_);
    if (!format || videoStream < 0)
        return nullptr;
    // 复制参数而不是暴露 format 内部指针，避免关闭媒体后出现悬空引用。
    AVCodecParameters *pa = avcodec_parameters_alloc();
    if (!pa)
        return nullptr;
    if (avcodec_parameters_copy(pa, format->streams[videoStream]->codecpar) < 0) {
        avcodec_parameters_free(&pa);
        return nullptr;
    }
    return pa;
}

AVCodecParameters *MyDemux::getAudioParametes()
{
    return getAudioParameters();
}

AVCodecParameters *MyDemux::getAudioParameters()
{
    std::lock_guard<std::mutex> parameterLock(mtx_);
    if(!format || audioStream < 0)return nullptr;
    AVCodecParameters * pa = avcodec_parameters_alloc();
    if (!pa)
        return nullptr;
    if (avcodec_parameters_copy(pa, format->streams[audioStream]->codecpar) < 0) {
        avcodec_parameters_free(&pa);
        return nullptr;
    }
    return pa;
}

double MyDemux::videoFrameRate()
{
    std::lock_guard<std::mutex> rateLock(mtx_);
    if (!format || videoStream < 0)
        return 0.0;

    // av_guess_frame_rate 会综合 avg_frame_rate、r_frame_rate 等容器信息。
    const AVRational rate = av_guess_frame_rate(
        format, format->streams[videoStream], nullptr);
    return rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
}

AVRational MyDemux::videoTimeBase()
{
    std::lock_guard<std::mutex> timeBaseLock(mtx_);
    if (!format || videoStream < 0)
        return AVRational{0, 1};
    return format->streams[videoStream]->time_base;
}

AVRational MyDemux::audioTimeBase()
{
    std::lock_guard<std::mutex> timeBaseLock(mtx_);
    if (!format || audioStream < 0)
        return AVRational{0, 1};
    return format->streams[audioStream]->time_base;
}

//跳转到秒数
bool MyDemux::Seek(double pos)
{

    std::unique_lock<std::mutex> seekLock(this->mtx_);
    if (!format || videoStream < 0)
        return false;
    avformat_flush(format);//清理缓存，防止粘包 这里注意不要调用clear（）,不然会死锁
    int64_t timePos = av_rescale_q((int64_t)(pos * AV_TIME_BASE),AV_TIME_BASE_Q,this->format->streams[videoStream]->time_base);
    int ret = av_seek_frame(format,videoStream,timePos,AVSEEK_FLAG_BACKWARD);

    return ret >= 0;
}



