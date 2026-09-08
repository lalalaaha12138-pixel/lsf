#ifndef AUDIORESAMPLE_H
#define AUDIORESAMPLE_H

#include <QAudioFormat>
#include <QByteArray>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

struct AVFrame;
struct SwrContext;

// 将解码器输出的各种采样格式转换为音频设备要求的交错 PCM。
class AudioResample
{
public:
    AudioResample();
    ~AudioResample();

    // 转换一帧音频。输入参数变化时会自动重建 SwrContext。
    bool convert(const AVFrame *frame,
                 const QAudioFormat &outputFormat,
                 QByteArray *pcmData);

    void close();

private:
    bool configure(const AVFrame *frame, const QAudioFormat &outputFormat);
    bool matchesCurrentFormat(const AVFrame *frame,
                              const QAudioFormat &outputFormat) const;
    static AVSampleFormat toAvSampleFormat(const QAudioFormat &format);

    SwrContext *m_context = nullptr;
    AVChannelLayout m_inputLayout = {};
    bool m_hasInputLayout = false;
    int m_inputSampleRate = 0;
    AVSampleFormat m_inputSampleFormat = AV_SAMPLE_FMT_NONE;
    int m_outputSampleRate = 0;
    int m_outputChannels = 0;
    AVSampleFormat m_outputSampleFormat = AV_SAMPLE_FMT_NONE;
};

#endif // AUDIORESAMPLE_H
