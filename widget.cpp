#include "widget.h"

#include "ui_widget.h"
#include "videoopenglwidget.h"

#include <QDebug>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QtGlobal>
#include <cmath>

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

    // 视频线程是当前播放器的必要组成部分，初始化失败则终止打开流程。
    AVCodecParameters *videoParameters = m_demux.getVideoParameters();
    if (!videoParameters || !m_videoThread.open(videoParameters)) {
        qWarning().noquote() << "Cannot open the video decoder:" << fileName;
        m_demux.close();
        return false;
    }

    // 音频是可选流，初始化失败时仍允许继续播放视频。
    AVCodecParameters *audioParameters = m_demux.getAudioParameters();
    if (audioParameters && !m_audioThread.open(audioParameters))
        qWarning() << "Cannot open the audio decoder";

    // 暂不做音画同步，仍按容器推测的视频帧率向线程投递视频包。
    const double frameRate = m_demux.videoFrameRate();
    const int interval = frameRate > 0.0
        ? qBound(1, static_cast<int>(std::lround(1000.0 / frameRate)), 1000)
        : 33;

    setWindowTitle(QFileInfo(fileName).fileName());
    m_packetTimer.start(interval);
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
    // 每个定时周期最多检查 256 个包，并在投递一个视频包后返回。
    // 位于该视频包之前的音频包会同时进入音频线程队列。
    for (int packetCount = 0; packetCount < 256; ++packetCount) {
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
            return;
        }

        av_packet_free(&packet);
    }
}
