#ifndef VIDEOTHREAD_H
#define VIDEOTHREAD_H

#include "mydecode.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>

struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
class audioThread;

// 视频工作线程：接收 Widget 分发的视频包，在 run() 中完成解码，
// 以音频时钟同步显示时间，再发送拥有独立生命周期的 YUV420P 数据。
class videoThread : public QThread
{
    Q_OBJECT

public:
    explicit videoThread(QObject *parent = nullptr);
    ~videoThread() override;

    // 打开视频解码器并启动线程。函数接管 parameters 的所有权。
    // audioClockSource 为 nullptr 时使用视频 PTS 和本地单调时钟自行调度。
    bool open(AVCodecParameters *parameters,
              AVRational timeBase,
              const audioThread *audioClockSource);

    // 接管 packet 的所有权；线程处理完成后会调用 av_packet_free。
    bool pushPacket(AVPacket *packet);

    // 供解封装线程实施背压；队列达到上限时暂停继续读取文件。
    bool hasPacketCapacity();

    // 通知线程后续不会再有视频包，使解码器输出内部延迟帧。
    void finishPackets();

    // 请求线程退出、等待 run() 结束并清空待处理包。
    void stopVideo();

signals:
    // frameData 按 Y + U + V 顺序紧密排列。QByteArray 使用隐式共享，
    // 信号排队到 GUI 线程后仍然拥有有效数据。
    void frameReady(const QByteArray &frameData, int width, int height);

protected:
    void run() override;

private:
    enum class SyncDecision
    {
        Present,
        Drop,
        Abort
    };

    static bool copyYuv420pFrame(const AVFrame *frame, QByteArray *frameData);

    //返回的是多少份的时间基数 也就是frame的pts
    qint64 frameTimestampUs(const AVFrame *frame) const;

    //
    SyncDecision synchronizeFrame(const AVFrame *frame);
    void clearPackets();

    MyDecode m_decoder;
    QQueue<AVPacket *> m_packets;
    QMutex m_packetMutex;
    QWaitCondition m_packetReady;
    std::atomic_bool m_abort{false};
    bool m_acceptPackets = false;
    bool m_inputFinished = false;

    // open() 在线程启动前写入，run() 中只读。
    AVRational m_timeBase{0, 1};


    //把主线程里的audioThread 地址传给videoThread
    const audioThread *m_audioClockSource = nullptr;

    // 没有可用音频时钟时，以第一帧视频 PTS 为起点自行控制播放速度。
    QElapsedTimer m_videoTimer;
    qint64 m_firstVideoPtsUs = 0;
};

#endif // VIDEOTHREAD_H
