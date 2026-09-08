#include "audiothread.h"

#include "playaudio.h"

#include <QByteArray>
#include <QDebug>
#include <QMutexLocker>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

audioThread::audioThread(QObject *parent)
    : QThread(parent)
{
}

audioThread::~audioThread()
{
    stopAudio();
}

bool audioThread::open(AVCodecParameters *parameters)
{
    stopAudio();
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

bool audioThread::pushPacket(AVPacket *packet)
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

void audioThread::finishPackets()
{
    QMutexLocker locker(&m_packetMutex);
    m_inputFinished = true;
    m_packetReady.wakeOne();
}

void audioThread::stopAudio()
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
    m_resample.close();
    m_decoder.close();

    QMutexLocker locker(&m_packetMutex);
    m_inputFinished = false;
}

void audioThread::clearPackets()
{
    QMutexLocker locker(&m_packetMutex);
    while (!m_packets.isEmpty()) {
        AVPacket *packet = m_packets.dequeue();
        av_packet_free(&packet);
    }
}

bool audioThread::writePcm(const QByteArray &pcmData)
{
    if (!m_playAudio || pcmData.isEmpty())
        return false;

    qint64 offset = 0;
    while (offset < pcmData.size() && !m_abort.load()) {
        const qint64 written = m_playAudio->write(
            pcmData.constData() + offset,
            pcmData.size() - offset);
        if (written < 0) {
            qWarning() << "Writing PCM data to the audio device failed";
            return false;
        }
        if (written == 0) {
            QThread::msleep(5);
            continue;
        }
        offset += written;
    }
    return offset == pcmData.size();
}

void audioThread::run()
{
    // QAudioOutput 必须在线程内部创建、使用和销毁，保证 QObject 线程归属正确。
    m_playAudio.reset(new PlayAudio);
    if (!m_playAudio->open()) {
        qWarning() << "Audio playback thread could not open an output device";
        m_playAudio.reset();
        {
            QMutexLocker locker(&m_packetMutex);
            m_acceptPackets = false;
        }
        clearPackets();
        m_decoder.close();
        return;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        m_playAudio->close();
        m_playAudio.reset();
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
        // 优先取走解码器现有输出，避免 sendPacket/receiveFrame 状态失衡。
        int ret = m_decoder.receiveFrame(frame);
        if (ret == 0) {
            QByteArray pcmData;
            if (m_resample.convert(frame, m_playAudio->format(), &pcmData))
                writePcm(pcmData);
            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR_EOF)
            break;
        if (ret != AVERROR(EAGAIN)) {
            qWarning() << "Receiving an audio frame failed:" << ret;
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
            // 理论上循环顶部已取空输出；仍遇到 EAGAIN 时把包放回队首重试。
            QMutexLocker locker(&m_packetMutex);
            m_packets.prepend(packet);
            continue;
        }

        av_packet_free(&packet);
        if (ret < 0) {
            qWarning() << "Decoder rejected an audio packet:" << ret;
            continue;
        }
    }

    av_frame_free(&frame);
    m_resample.close();
    m_playAudio->close();
    m_playAudio.reset();

    {
        QMutexLocker locker(&m_packetMutex);
        m_acceptPackets = false;
    }
    clearPackets();
    m_decoder.close();
}
