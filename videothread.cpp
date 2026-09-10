#include "videothread.h"

#include "audiothread.h"

#include <QDebug>
#include <QMutexLocker>
#include <QtGlobal>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

videoThread::videoThread(QObject *parent)
    : QThread(parent)
{
}

videoThread::~videoThread()
{
    stopVideo();
}

bool videoThread::open(AVCodecParameters *parameters,
                       AVRational timeBase,
                       const audioThread *audioClockSource)
{
    stopVideo();
    if (!parameters)
        return false;
    if (timeBase.num <= 0 || timeBase.den <= 0) {
        // open() 的契约是只要收到 parameters 就接管所有权。
        avcodec_parameters_free(&parameters);
        return false;
    }

    // MyDecode::open 接管并释放 parameters。
    if (!m_decoder.open(parameters, timeBase))
        return false;

    m_timeBase = timeBase;
    m_audioClockSource = audioClockSource;
    m_videoTimer.invalidate();
    m_firstVideoPtsUs = 0;
    {
        QMutexLocker locker(&m_packetMutex);
        m_abort.store(false);
        m_acceptPackets = true;
        m_inputFinished = false;
    }
    start();
    return true;
}

bool videoThread::pushPacket(AVPacket *packet)
{
    if (!packet)
        return false;

    QMutexLocker locker(&m_packetMutex);
    if (!m_acceptPackets || m_inputFinished || m_abort.load()) {
        locker.unlock();
        av_packet_free(&packet);
        return false;
    }

    m_packets.enqueue(packet);
    m_packetReady.wakeOne();
    return true;
}

bool videoThread::hasPacketCapacity()
{
    QMutexLocker locker(&m_packetMutex);
    // 允许预读若干视频包，使同步线程落后时可以连续丢帧追赶，
    // 同时限制内存不会随着媒体时长无限增长。
    return m_packets.size() < 64;
}

void videoThread::finishPackets()
{
    QMutexLocker locker(&m_packetMutex);
    m_inputFinished = true;
    m_packetReady.wakeOne();
}

void videoThread::stopVideo()
{
    m_abort.store(true);
    {
        QMutexLocker locker(&m_packetMutex);
        m_acceptPackets = false;
        m_packetReady.wakeAll();
    }

    if (isRunning() && QThread::currentThread() != this)
        wait();

    clearPackets();
    m_decoder.close();
    m_audioClockSource = nullptr;
    m_videoTimer.invalidate();

    QMutexLocker locker(&m_packetMutex);
    m_inputFinished = false;
}

void videoThread::clearPackets()
{
    QMutexLocker locker(&m_packetMutex);
    while (!m_packets.isEmpty()) {
        AVPacket *packet = m_packets.dequeue();
        av_packet_free(&packet);
    }
}

bool videoThread::copyYuv420pFrame(const AVFrame *frame,
                                   QByteArray *frameData)
{
    if (!frame || !frameData || frame->width <= 0 || frame->height <= 0 ||
        !frame->data[0] || !frame->data[1] || !frame->data[2]) {
        return false;
    }
    if (frame->format != AV_PIX_FMT_YUV420P) {
        qWarning() << "videoThread only supports AV_PIX_FMT_YUV420P, got:"
                   << frame->format;
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    const int ySize = width * height;
    const int chromaSize = chromaWidth * chromaHeight;
    frameData->resize(ySize + 2 * chromaSize);

    char *dstPlanes[3] = {
        frameData->data(),
        frameData->data() + ySize,
        frameData->data() + ySize + chromaSize
    };
    const int rowBytes[3] = {width, chromaWidth, chromaWidth};
    const int planeRows[3] = {height, chromaHeight, chromaHeight};
    for (int plane = 0; plane < 3; ++plane) {
        for (int row = 0; row < planeRows[plane]; ++row) {
            std::memcpy(dstPlanes[plane] + row * rowBytes[plane],
                        frame->data[plane] + row * frame->linesize[plane],
                        static_cast<size_t>(rowBytes[plane]));
        }
    }
    return true;
}

qint64 videoThread::frameTimestampUs(const AVFrame *frame) const
{
    if (!frame || m_timeBase.num <= 0 || m_timeBase.den <= 0)
        return AV_NOPTS_VALUE;

    // 解码视频可能存在 B 帧，best_effort_timestamp 比 pkt_dts 更接近显示顺序。
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = frame->pts;
    if (timestamp == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;

    return av_rescale_q(timestamp, m_timeBase, AV_TIME_BASE_Q);
}

videoThread::SyncDecision videoThread::synchronizeFrame(const AVFrame *frame)
{
    const qint64 videoPtsUs = frameTimestampUs(frame);
    if (videoPtsUs == AV_NOPTS_VALUE)
        return SyncDecision::Present;

    // 本地计时器既是无音频时的主时钟，也是音频结束后的回退时钟。
    if (!m_videoTimer.isValid()) {
        m_firstVideoPtsUs = videoPtsUs;
        m_videoTimer.start();
    }

    qint64 audioClockUs = m_audioClockSource
        ? m_audioClockSource->clockUs()
        : audioThread::InvalidClockUs;

    if (audioClockUs != audioThread::InvalidClockUs) {
        qint64 frameDurationUs = 40000;
        if (frame->duration > 0) {
            frameDurationUs = av_rescale_q(
                frame->duration, m_timeBase, AV_TIME_BASE_Q);
        }

        // 视频落后超过约一帧时直接丢弃；阈值限制在 20~100 ms，
        // 避免异常 duration 导致所有帧都被丢弃或完全不丢帧。
        const qint64 lateThresholdUs = qBound<qint64>(
            20000, qAbs(frameDurationUs), 100000);
        qint64 differenceUs = videoPtsUs - audioClockUs;
        if (differenceUs < -lateThresholdUs)
            return SyncDecision::Drop;

        // 视频早于音频时分段等待。每次最多睡 10 ms，以便及时响应停止，
        // 并重新读取不断前进的音频时钟，避免一次睡眠过长造成过冲。
        while (differenceUs > 2000 && !m_abort.load()) {
            const unsigned long sleepMs = static_cast<unsigned long>(
                qBound<qint64>(1, (differenceUs - 2000 + 999) / 1000, 10));
            QThread::msleep(sleepMs);

            audioClockUs = m_audioClockSource->clockUs();
            if (audioClockUs == audioThread::InvalidClockUs)
                break;

            differenceUs = videoPtsUs - audioClockUs;
            if (differenceUs < -lateThresholdUs)
                return SyncDecision::Drop;
        }

        return m_abort.load() ? SyncDecision::Abort : SyncDecision::Present;
    }

    // 没有音频、音频尚未建立 PTS，或者音频已经结束时，
    // 按视频自身 PTS 相对于第一帧的时间控制显示，避免瞬间播放完整个视频。
    const qint64 targetElapsedUs = videoPtsUs - m_firstVideoPtsUs;
    while (!m_abort.load()) {
        const qint64 remainingUs = targetElapsedUs
            - m_videoTimer.nsecsElapsed() / 1000;
        if (remainingUs <= 2000)
            break;

        const unsigned long sleepMs = static_cast<unsigned long>(
            qBound<qint64>(1, (remainingUs - 2000 + 999) / 1000, 10));
        QThread::msleep(sleepMs);
    }

    return m_abort.load() ? SyncDecision::Abort : SyncDecision::Present;
}

void videoThread::run()
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        {
            QMutexLocker locker(&m_packetMutex);
            m_acceptPackets = false;
        }
        clearPackets();
        m_decoder.close();
        return;
    }

    bool draining = false;
    while (!m_abort.load()) {
        // 优先取走已有输出，保持 FFmpeg send/receive 状态机平衡。
        int ret = m_decoder.receiveFrame(frame);
        if (ret == 0) {
            const SyncDecision decision = synchronizeFrame(frame);
            if (decision == SyncDecision::Abort) {
                av_frame_unref(frame);
                break;
            }

            if (decision == SyncDecision::Present) {
                QByteArray frameData;
                if (copyYuv420pFrame(frame, &frameData))
                    emit frameReady(frameData, frame->width, frame->height);
            }
            // Drop 时不复制、不发送帧，下一帧会继续与最新音频时钟比较。
            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR_EOF)
            break;
        if (ret != AVERROR(EAGAIN)) {
            qWarning() << "Receiving a video frame failed:" << ret;
            break;
        }
        //receiveFrame() 返回 EEAGAIN
        //→如果缓存区中暂时没有完整输出帧
        //→需要从包队列取 AVPacket，并并调用 sendPacket()
        AVPacket *packet = nullptr;
        bool inputFinished = false;//代表文件没有结束
        {
            QMutexLocker locker(&m_packetMutex);
            while (m_packets.isEmpty() && !m_inputFinished && !m_abort.load())
                //解封装器返回 AVERROR_EOF
                //说明不会再产生新的 AVPacket
                //但必须先处理完 m_packets 中已有的包
                //有包了||终止了就唤醒，没包阻塞
                m_packetReady.wait(&m_packetMutex);

            if (m_abort.load())
                break;
            if (!m_packets.isEmpty())
                packet = m_packets.dequeue();
            //更新是不是是真的文件结束了
            inputFinished = m_inputFinished && m_packets.isEmpty();
        }
        //如果队列里每包了且是文件尾，send(nullptr),读取缓冲区最后的尾帧
        if (!packet && inputFinished) {
            if (!draining) {
                m_decoder.sendPacket(nullptr);
                draining = true;
                continue;
            }
            break;
        }
        if (!packet)
            continue;

        ret = m_decoder.sendPacket(packet);

        //sendPacket() 返回醒EAGAIN
        //说明包没有被接受
        //需要把包放回队首
        //先调用 receiveFrame() 取走解码器已有输出
        if (ret == AVERROR(EAGAIN)) {
            QMutexLocker locker(&m_packetMutex);
            m_packets.prepend(packet);
            continue;
        }

        av_packet_free(&packet);
        if (ret < 0)
            qWarning() << "Decoder rejected a video packet:" << ret;
    }

    av_frame_free(&frame);
    {
        QMutexLocker locker(&m_packetMutex);
        m_acceptPackets = false;
    }
    clearPackets();
    m_decoder.close();
}
