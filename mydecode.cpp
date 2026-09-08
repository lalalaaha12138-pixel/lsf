 #include "mydecode.h"
extern "C"{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include <iostream>
MyDecode::MyDecode()
{

}

MyDecode::~MyDecode()
{
    close();
}

bool MyDecode::open(AVCodecParameters *para)
{
    // 一个 MyDecode 实例可以反复打开不同的视频流。
    close();
    std::unique_lock<std::mutex> openLock(this->mut_);
    if (!para) {
        return false;
    }
    // 根据流中的 codec_id 自动查找对应的软件解码器。
    const AVCodec *codecTemp = avcodec_find_decoder(para->codec_id);
    if (!codecTemp){
        avcodec_parameters_free(&para);
        return false;
    }
    codecCtx = avcodec_alloc_context3(codecTemp);
    if (!codecCtx){
        avcodec_parameters_free(&para);
        return false;
    }

    // 将容器层的编码参数复制到真正执行解码的 AVCodecContext。
    int ret = avcodec_parameters_to_context(codecCtx,para);
    if (ret < 0){
        avcodec_parameters_free(&para);
        avcodec_free_context(&codecCtx);
        char buf[1024] = {0};
        av_strerror(ret,buf,sizeof (buf) -1);
        return false;
    }

    // 允许 FFmpeg 在支持的解码器中使用帧级或切片级多线程。
    codecCtx->thread_count = 6;


    ret = avcodec_open2(codecCtx,codecTemp, nullptr);
    if (ret < 0){
        avcodec_parameters_free(&para);
        avcodec_free_context(&codecCtx);
        char buf[1024] = {0};
        av_strerror(ret,buf,sizeof (buf) -1);
        return false;
    }

    avcodec_parameters_free(&para);

    return true;
}

int MyDecode::sendPacket(const AVPacket *packet)
{
    std::lock_guard<std::mutex> sendLock(mut_);

    if (!codecCtx) {
        return AVERROR(EINVAL);
    }

    // nullptr 是 FFmpeg 规定的 drain 信号，用于在文件末尾取出 B 帧。
    return avcodec_send_packet(codecCtx, packet);
}

int MyDecode::receiveFrame(AVFrame *frame)
{
    std::lock_guard<std::mutex> receiveLock(mut_);

    if (!codecCtx || !frame) {
        return AVERROR(EINVAL);
    }

    return avcodec_receive_frame(codecCtx, frame);
}

void MyDecode::clear()
{
    std::unique_lock<std::mutex> clearLock(this->mut_);
    if(this->codecCtx){
        avcodec_flush_buffers(codecCtx);
    }
}

void MyDecode::close()
{
    std::unique_lock<std::mutex> closeLock(this->mut_);
    if(this->codecCtx){
        avcodec_free_context(&codecCtx);
    }
}
