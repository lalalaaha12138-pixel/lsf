#include "widget.h"

#include "ui_widget.h"
#include "videoopenglwidget.h"

#include <QDebug>
#include <QFileInfo>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    // Designer 文件负责整个窗口布局，这里只把真正的 OpenGL 渲染控件
    // 放入预留的视频区域。下方控制按钮目前只完成界面，不连接播放逻辑。
    m_videoWidget = new videoOpenGLWidget(ui->videoFrame);
    ui->videoLayout->addWidget(m_videoWidget);

    // videoThread 发出的是独立 QByteArray，排队到 GUI 线程后再更新渲染缓存。
    connect(&m_videoThread, &videoThread::frameReady,
            m_videoWidget,
            [this](const QByteArray &frameData, int width, int height) {
                m_videoWidget->setFrame(frameData, width, height);
            },
            Qt::QueuedConnection);

    m_packetTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_packetTimer, &QTimer::timeout,
            this, &Widget::dispatchNextPackets);
}

Widget::~Widget()
{
    stopPlayback();
    delete ui;
}

bool Widget::openMedia(const QString &fileName)
{
    stopPlayback();
    if (fileName.isEmpty())
        return false;

    const QByteArray encodedName = fileName.toUtf8();
    if (!m_demux.open(encodedName.constData())) {
        qWarning().noquote() << "Cannot open media:" << fileName;
        return false;
    }

    // 先启动音频线程，使视频线程从第一帧开始就能读取音频主时钟。
    // 音频是可选流，初始化失败时仍允许视频使用自身 PTS 播放。
    bool audioStarted = false;
    AVCodecParameters *audioParameters = m_demux.getAudioParameters();
    if (audioParameters) {
        audioStarted = m_audioThread.open(
            audioParameters, m_demux.audioTimeBase());
        if (!audioStarted)
            qWarning() << "Cannot open the audio decoder";
    }

    // 视频是当前播放器的必要组成部分，初始化失败则终止整个打开流程。
    AVCodecParameters *videoParameters = m_demux.getVideoParameters();
    if (!videoParameters ||
        !m_videoThread.open(videoParameters,
                            m_demux.videoTimeBase(),
                            audioStarted ? &m_audioThread : nullptr)) {
        qWarning().noquote() << "Cannot open the video decoder:" << fileName;
        m_audioThread.stopAudio();
        m_demux.close();
        return false;
    }

    // 快速预读数据包，队列容量负责背压；播放速度不再由投包定时器决定，
    // 而是由 video PTS - audio clock 的同步结果决定。
    setWindowTitle(QFileInfo(fileName).fileName());
    m_packetTimer.start(1);
    dispatchNextPackets();
    return true;
}

void Widget::stopPlayback()
{
    m_packetTimer.stop();
    m_videoThread.stopVideo();
    m_audioThread.stopAudio();
    m_demux.close();
    if (m_videoWidget)
        m_videoWidget->clearFrame();
}

void Widget::dispatchNextPackets()
{
    // 队列满时暂停解封装，等工作线程消费后由下一个定时周期继续。
    // 这既给视频线程保留连续丢帧追赶的空间，也避免无限预读占用内存。
    for (int packetCount = 0; packetCount < 256; ++packetCount) {
        if (!m_videoThread.hasPacketCapacity() ||
            !m_audioThread.hasPacketCapacity()) {
            return;
        }

        AVPacket *packet = nullptr;
        const int readResult = m_demux.read(&packet);

        // 非阻塞输入可能暂时没有新数据。保留定时器，下一轮继续读取，
        // 不能把 EAGAIN 当作文件结束去排空解码器。
        if (readResult == AVERROR(EAGAIN))
            return;

        // AVERROR_EOF 才表示解封装器确认不会再产生新的数据包。
        if (readResult == AVERROR_EOF) {
            m_packetTimer.stop();
            m_videoThread.finishPackets();
            m_audioThread.finishPackets();
            return;
        }

        // 其他负数是真正的读取错误。停止继续读取，并让工作线程处理完
        // 已经进入队列和解码器缓存的数据，错误原因保留在日志中。
        if (readResult < 0) {
            char errorText[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(readResult, errorText, sizeof(errorText));
            qWarning().noquote()
                << "Reading a media packet failed:" << errorText
                << "(" << readResult << ")";
            m_packetTimer.stop();
            m_videoThread.finishPackets();
            m_audioThread.finishPackets();
            return;
        }

        // read() 的契约规定成功时 packet 一定有效；保留这层检查，避免
        // 将来修改解封装器后把空指针继续传给 packetType()。
        if (!packet) {
            qWarning() << "Demuxer returned success without a packet";
            m_packetTimer.stop();
            m_videoThread.finishPackets();
            m_audioThread.finishPackets();
            return;
        }

        const MyDemux::PacketType packetType = m_demux.packetType(packet);
        if (packetType == MyDemux::PacketType::Audio) {
            // pushPacket 接管 AVPacket，成功或失败都会负责释放。
            m_audioThread.pushPacket(packet);
            continue;
        }

        if (packetType == MyDemux::PacketType::Video) {
            if (!m_videoThread.pushPacket(packet)) {
                qWarning() << "Video thread is not accepting packets";
                m_packetTimer.stop();
                m_audioThread.finishPackets();
            }
            continue;
        }

        av_packet_free(&packet);
    }
}
