#include "videothread.h"

#include <QDebug>
#include <QMutexLocker>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
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

bool videoThread::open(AVCodecParameters *parameters)
{
    stopVideo();
    if (!parameters)
        return false;

    // MyDecode::open 接管并释放 parameters。
    if (!m_decoder.open(parameters))
        return false;

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
            QByteArray frameData;
            if (copyYuv420pFrame(frame, &frameData))
                emit frameReady(frameData, frame->width, frame->height);
            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR_EOF)
            break;
        if (ret != AVERROR(EAGAIN)) {
            qWarning() << "Receiving a video frame failed:" << ret;
            break;
        }

        AVPacket *packet = nullptr;
        bool inputFinished = false;
        {
            QMutexLocker locker(&m_packetMutex);
            while (m_packets.isEmpty() && !m_inputFinished && !m_abort.load())
                m_packetReady.wait(&m_packetMutex);

            if (m_abort.load())
                break;
            if (!m_packets.isEmpty())
                packet = m_packets.dequeue();
            inputFinished = m_inputFinished && m_packets.isEmpty();
        }

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
