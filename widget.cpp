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
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_videoFrame(av_frame_alloc())
{
    ui->setupUi(this);

    // videoOpenGLWidget 不再自己加载 UI，它只是普通子控件，由播放器窗口托管。
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_videoWidget = new videoOpenGLWidget(this);
    layout->addWidget(m_videoWidget);

    m_decodeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_decodeTimer, &QTimer::timeout,
            this, &Widget::decodeNextFrame);
}

Widget::~Widget()
{
    stopPlayback();
    av_frame_free(&m_videoFrame);
    delete ui;
}

bool Widget::openMedia(const QString &fileName)
{
    // 允许重复打开文件：先完整清理上一个文件的解码状态和残留画面。
    stopPlayback();
    if (fileName.isEmpty() || !m_videoFrame)
        return false;

    // FFmpeg 在 Windows 下接收 UTF-8 路径，确保中文文件名可以正常打开。
    const QByteArray encodedName = fileName.toUtf8();
    if (!m_demux.open(encodedName.constData())) {
        qWarning().noquote() << "Cannot open media:" << fileName;
        return false;
    }

    // getVideoParameters 返回独立副本；MyDecode::open 会接管并释放它。
    AVCodecParameters *videoParameters = m_demux.getVideoParameters();
    if (!videoParameters || !m_videoDecoder.open(videoParameters)) {
        qWarning().noquote() << "Cannot open the video decoder:" << fileName;
        m_demux.close();
        return false;
    }

    // 音频初始化失败不影响视频继续播放。
    AVCodecParameters *audioParameters = m_demux.getAudioParameters();
    if (audioParameters && !m_audioThread.open(audioParameters))
        qWarning() << "Cannot open the audio decoder";

    // 以视频流帧率驱动解码定时器；容器没有帧率信息时回退到约 30 fps。
    const double frameRate = m_demux.videoFrameRate();
    const int interval = frameRate > 0.0
        ? qBound(1, static_cast<int>(std::lround(1000.0 / frameRate)), 1000)
        : 33;

    setWindowTitle(QFileInfo(fileName).fileName());
    m_draining = false;
    m_decodeTimer.start(interval);
    decodeNextFrame();
    return true;
}

void Widget::stopPlayback()
{
    m_decodeTimer.stop();
    m_audioThread.stopAudio();
    m_draining = false;
    if (m_videoFrame)
        av_frame_unref(m_videoFrame);
    m_videoDecoder.close();
    m_demux.close();
    if (m_videoWidget)
        m_videoWidget->clearFrame();
}

bool Widget::presentFrame()
{
    // setFrame 会复制 YUV420P 数据，因此这里可以立即 unref。
    const bool accepted = m_videoWidget->setFrame(m_videoFrame);
    av_frame_unref(m_videoFrame);
    return accepted;
}

void Widget::decodeNextFrame()
{
    if (!m_videoFrame)
        return;

    // 先尝试取走解码器中已经生成的帧，再投递新包。
    // 这样既满足 FFmpeg send/receive 协议，也能处理一个包输出多帧的情况。
    int ret = m_videoDecoder.receiveFrame(m_videoFrame);
    if (ret == 0) {
        presentFrame();
        return;
    }
    if (ret == AVERROR_EOF) {
        m_decodeTimer.stop();
        return;
    }

    // 音频包转交给 audioThread，字幕等其他包直接跳过。每个定时器周期
    // 限制最多读取 256 个包，防止长时间无视频包时阻塞 GUI 线程。
    for (int packetCount = 0; packetCount < 256; ++packetCount) {
        AVPacket *packet = m_demux.read();
        if (!packet) {
            m_audioThread.finishPackets();
            // av_read_frame 返回空表示文件结束。发送空包通知解码器进入 drain，
            // H.264/H.265 等含 B 帧的编码可能仍有延迟帧需要取出。
            if (!m_draining) {
                m_videoDecoder.sendPacket(nullptr);
                m_draining = true;
            }

            ret = m_videoDecoder.receiveFrame(m_videoFrame);
            if (ret == 0)
                presentFrame();
            else
                m_decodeTimer.stop();
            return;
        }

        const MyDemux::PacketType packetType = m_demux.packetType(packet);
        if (packetType == MyDemux::PacketType::Audio) {
            // pushPacket 接管 AVPacket，成功或失败都会负责释放。
            m_audioThread.pushPacket(packet);
            continue;
        }
        if (packetType != MyDemux::PacketType::Video) {
            av_packet_free(&packet);
            continue;
        }

        ret = m_videoDecoder.sendPacket(packet);
        if (ret == AVERROR(EAGAIN)) {
            // EAGAIN 表示解码器输出队列尚未取空：先接收一帧，再重送同一个包，
            // 避免因为状态切换而静默丢包。
            const int receiveRet = m_videoDecoder.receiveFrame(m_videoFrame);
            ret = m_videoDecoder.sendPacket(packet);
            av_packet_free(&packet);
            if (receiveRet == 0) {
                presentFrame();
                return;
            }
        } else {
            av_packet_free(&packet);
        }

        if (ret < 0) {
            qWarning() << "Decoder rejected a video packet:" << ret;
            continue;
        }

        ret = m_videoDecoder.receiveFrame(m_videoFrame);
        if (ret == 0) {
            presentFrame();
            return;
        }
        if (ret == AVERROR_EOF) {
            m_decodeTimer.stop();
            return;
        }
    }
}
