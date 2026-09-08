#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H

#include "audioresample.h"
#include "mydecode.h"

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>

struct AVCodecParameters;
struct AVPacket;
class PlayAudio;

// 音频工作线程：接收 Widget 分发的音频包，在 run() 中完成解码、
// 重采样和设备播放。当前不参与音画同步。
class audioThread : public QThread
{
public:
    explicit audioThread(QObject *parent = nullptr);
    ~audioThread() override;

    // 打开音频解码器并启动线程。函数接管 parameters 的所有权。
    bool open(AVCodecParameters *parameters);

    // 接管 packet 的所有权；线程处理完成后会调用 av_packet_free。
    bool pushPacket(AVPacket *packet);

    // 通知线程后续不会再有音频包，使解码器输出内部延迟帧。
    void finishPackets();

    // 请求线程退出、等待 run() 结束并清空所有待处理数据。
    void stopAudio();

protected:
    void run() override;

private:
    void clearPackets();
    bool writePcm(const QByteArray &pcmData);

    MyDecode m_decoder;
    AudioResample m_resample;
    std::unique_ptr<PlayAudio> m_playAudio;

    QQueue<AVPacket *> m_packets;
    QMutex m_packetMutex;
    QWaitCondition m_packetReady;
    std::atomic_bool m_abort{false};
    bool m_acceptPackets = false;
    bool m_inputFinished = false;
};

#endif // AUDIOTHREAD_H
