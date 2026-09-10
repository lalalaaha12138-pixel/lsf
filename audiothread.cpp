#include "audiothread.h"

#include "playaudio.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>

#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

audioThread::audioThread(QObject *parent)
    : QThread(parent)
{
}

audioThread::~audioThread()
{
    stopAudio();
}

bool audioThread::open(AVCodecParameters *parameters, AVRational timeBase)
{
    stopAudio();
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
    m_clockOriginUs = 0;
    m_submittedBytes = 0;
    m_clockStarted = false;
    m_clockUs.store(InvalidClockUs, std::memory_order_release);
    m_clockUpdatedSteadyUs.store(InvalidClockUs, std::memory_order_release);
    m_submittedEndUs.store(InvalidClockUs, std::memory_order_release);
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

bool audioThread::hasPacketCapacity()
{
    QMutexLocker locker(&m_packetMutex);
    // 音频包通常较小，多缓存一些可以降低解封装调度抖动造成的断音概率。
    return m_packets.size() < 256;
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
    m_clockUs.store(InvalidClockUs, std::memory_order_release);
    m_clockUpdatedSteadyUs.store(InvalidClockUs, std::memory_order_release);
    m_submittedEndUs.store(InvalidClockUs, std::memory_order_release);
    m_clockStarted = false;
    m_submittedBytes = 0;

    QMutexLocker locker(&m_packetMutex);
    m_inputFinished = false;
}

qint64 audioThread::clockUs() const
{
    const qint64 sampledClockUs = m_clockUs.load(std::memory_order_acquire);
    const qint64 sampledAtUs =
        m_clockUpdatedSteadyUs.load(std::memory_order_acquire);
    const qint64 submittedEndUs =
        m_submittedEndUs.load(std::memory_order_acquire);
    if (sampledClockUs == InvalidClockUs ||
        sampledAtUs == InvalidClockUs ||
        submittedEndUs == InvalidClockUs) {
        return InvalidClockUs;
    }

    // 视频线程不能跨线程调用 QAudioOutput。两次音频线程采样之间，
    // 使用 steady_clock 推算时钟前进量，并限制它不能超过已提交 PCM 末尾。
    const qint64 elapsedUs = qMax<qint64>(0, steadyClockUs() - sampledAtUs);
    return qMin(sampledClockUs + elapsedUs, submittedEndUs);
}

qint64 audioThread::steadyClockUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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
            // 设备缓冲区已满时，声卡仍在后台消费数据；同步时钟也要继续更新。
            refreshClock();
            QThread::msleep(5);
            continue;
        }
        offset += written;
        m_submittedBytes += written;
        refreshClock();
    }
    return offset == pcmData.size();
}

qint64 audioThread::submittedDurationUs() const
{
    if (!m_playAudio)
        return 0;

    const QAudioFormat outputFormat = m_playAudio->format();
    const int bytesPerSample = outputFormat.sampleSize() / 8;
    const int bytesPerFrame = bytesPerSample * outputFormat.channelCount();
    if (bytesPerFrame <= 0 || outputFormat.sampleRate() <= 0)
        return 0;

    // 一个音频 sample frame 包含所有声道在同一时刻的样本。
    const qint64 submittedSamples = m_submittedBytes / bytesPerFrame;
    return av_rescale(submittedSamples,
                      AV_TIME_BASE,
                      outputFormat.sampleRate());
}

void audioThread::tryStartClock(const AVFrame *frame)
{
    if (m_clockStarted || !frame || m_timeBase.num <= 0 || m_timeBase.den <= 0)
        return;

    // best_effort_timestamp 能处理部分缺失 PTS 以及视频重排序；
    // 音频也统一优先使用它，缺失时再退回 frame->pts。
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = frame->pts;
    if (timestamp == AV_NOPTS_VALUE)
        return;

    const qint64 framePtsUs = av_rescale_q(
        timestamp, m_timeBase, AV_TIME_BASE_Q);

    // 如果前面存在没有时间戳的帧，应从当前帧 PTS 中减去已提交时长，
    // 让 processedUSecs() 从声卡开始播放处映射到正确的媒体时间轴。
    m_clockOriginUs = framePtsUs - submittedDurationUs();
    m_clockStarted = true;
    refreshClock();
}

void audioThread::refreshClock()
{
    if (!m_clockStarted || !m_playAudio)
        return;

    // processedUSecs() 表示 QAudioOutput 从 start() 起已处理的音频时长，
    // 比“已经写入多少 PCM”更接近用户真正听到的位置。
    qint64 playedUs = m_clockOriginUs + m_playAudio->processedUSecs();
    const qint64 submittedEndUs = m_clockOriginUs + submittedDurationUs();

    // 某些音频后端在 Idle 状态下仍可能让 processedUSecs() 略微增长，
    // 时钟不能越过已经提交给设备的最后一个样本。
    if (playedUs > submittedEndUs)
        playedUs = submittedEndUs;

    // 先发布上限和采样时刻，最后用 release 发布基础时钟。
    // clockUs() 读取后即可在不访问 QAudioOutput 的情况下平滑推算。
    m_submittedEndUs.store(submittedEndUs, std::memory_order_release);
    m_clockUpdatedSteadyUs.store(steadyClockUs(), std::memory_order_release);
    m_clockUs.store(playedUs, std::memory_order_release);
}
//清空缓冲区里的pcm
void audioThread::waitForDeviceDrain()
{
    if (!m_playAudio || !m_clockStarted)
        return;

    // write() 成功只表示 PCM 进入设备缓冲区，不代表已经播放。
    // 文件正常结束时最多等待 1 秒，让约 200 ms 的设备缓冲播放完。
    QElapsedTimer timeout;
    timeout.start();
    while (!m_abort.load() && timeout.elapsed() < 1000) {
        refreshClock();
        //判断缓冲区是否播放完 //当前没有更多 PCM 可以播放
        if (m_playAudio->state() == QAudio::IdleState ||
            m_playAudio->state() == QAudio::StoppedState) {
            break;
        }
        QThread::msleep(5);
    }
    refreshClock();
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
    bool decoderDrained = false;
    while (!m_abort.load()) {
        // 优先取走解码器现有输出，避免 sendPacket/receiveFrame 状态失衡。
        int ret = m_decoder.receiveFrame(frame);
        if (ret == 0) {
            QByteArray pcmData;
            if (m_resample.convert(frame, m_playAudio->format(), &pcmData)) {
                // 只有生成了可播放 PCM 后才建立音频主时钟。
                tryStartClock(frame);
                if (!writePcm(pcmData)) {
                    av_frame_unref(frame);
                    break;
                }
            }
            av_frame_unref(frame);
            continue;
        }
        if (ret == AVERROR_EOF) {
            decoderDrained = true;
            break;
        }
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

    if (decoderDrained && !m_abort.load())
        //清空声卡缓冲区
        waitForDeviceDrain();

    av_frame_free(&frame);
    m_resample.close();
    m_playAudio->close();
    m_playAudio.reset();
    // 音频结束后使时钟失效，避免较长的视频尾部永远等待最后一个音频时间。
    m_clockUs.store(InvalidClockUs, std::memory_order_release);
    m_clockUpdatedSteadyUs.store(InvalidClockUs, std::memory_order_release);
    m_submittedEndUs.store(InvalidClockUs, std::memory_order_release);

    {
        QMutexLocker locker(&m_packetMutex);
        m_acceptPackets = false;
    }
    clearPackets();
    m_decoder.close();
}
