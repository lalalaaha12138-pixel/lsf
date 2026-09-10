#include "widget.h"

#include "ui_widget.h"
#include "videoopenglwidget.h"

#include <QDebug>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
}

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_videoWidget = new videoOpenGLWidget(this);
    layout->addWidget(m_videoWidget);

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

        AVPacket *packet = m_demux.read();
        if (!packet) {
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
