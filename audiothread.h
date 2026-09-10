#ifndef AUDIOTHREAD_H
#define AUDIOTHREAD_H

#include "audioresample.h"
#include "mydecode.h"

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <limits>
#include <memory>

struct AVCodecParameters;
struct AVFrame;
struct AVPacket;
class PlayAudio;

// 音频工作线程：接收 Widget 分发的音频包，在 run() 中完成解码、
// 重采样和设备播放，并发布供视频线程读取的音频播放时钟。
class audioThread : public QThread
{
public:
    static constexpr qint64 InvalidClockUs =
        std::numeric_limits<qint64>::min();

    explicit audioThread(QObject *parent = nullptr);
    ~audioThread() override;

    // 打开音频解码器并启动线程。函数接管 parameters 的所有权。
    // timeBase 是该音频流中 packet/frame 时间戳对应的时间基。
    bool open(AVCodecParameters *parameters, AVRational timeBase);

    // 接管 packet 的所有权；线程处理完成后会调用 av_packet_free。
    bool pushPacket(AVPacket *packet);

    // 供解封装线程实施背压；队列达到上限时暂停继续读取文件。
    bool hasPacketCapacity();

    // 通知线程后续不会再有音频包，使解码器输出内部延迟帧。
    void finishPackets();

    // 请求线程退出、等待 run() 结束并清空所有待处理数据。
    void stopAudio();

    // 返回声卡估计已经播放到的媒体时间，单位为微秒。
    // 返回 InvalidClockUs 表示声卡尚未建立有效音频时钟或已经停止。
    qint64 clockUs() const;

protected:
    void run() override;

private:
    void clearPackets();
    bool writePcm(const QByteArray &pcmData);
    void tryStartClock(const AVFrame *frame);
    void refreshClock();
    void waitForDeviceDrain();
    qint64 submittedDurationUs() const;
    static qint64 steadyClockUs();

    MyDecode m_decoder;
    AudioResample m_resample;
    std::unique_ptr<PlayAudio> m_playAudio;

    QQueue<AVPacket *> m_packets;
    QMutex m_packetMutex;
    QWaitCondition m_packetReady;
    std::atomic_bool m_abort{false};
    std::atomic<qint64> m_clockUs{InvalidClockUs};
    std::atomic<qint64> m_clockUpdatedSteadyUs{InvalidClockUs};
    std::atomic<qint64> m_submittedEndUs{InvalidClockUs};
    bool m_acceptPackets = false;
    bool m_inputFinished = false;

    // 以下字段只在音频工作线程中访问。m_clockUs 是唯一跨线程读取的值。
    AVRational m_timeBase{0, 1};
    qint64 m_clockOriginUs = 0;
    qint64 m_submittedBytes = 0;
    bool m_clockStarted = false;
};

#endif // AUDIOTHREAD_H
