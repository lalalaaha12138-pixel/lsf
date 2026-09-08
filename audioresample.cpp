#include "audioresample.h"

#include <QDebug>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

AudioResample::AudioResample() = default;

AudioResample::~AudioResample()
{
    close();
}

AVSampleFormat AudioResample::toAvSampleFormat(const QAudioFormat &format)
{
    if (format.byteOrder() != QAudioFormat::LittleEndian)
        return AV_SAMPLE_FMT_NONE;

    if (format.sampleType() == QAudioFormat::SignedInt) {
        if (format.sampleSize() == 16)
            return AV_SAMPLE_FMT_S16;
        if (format.sampleSize() == 32)
            return AV_SAMPLE_FMT_S32;
    } else if (format.sampleType() == QAudioFormat::UnSignedInt &&
               format.sampleSize() == 8) {
        return AV_SAMPLE_FMT_U8;
    } else if (format.sampleType() == QAudioFormat::Float) {
        if (format.sampleSize() == 32)
            return AV_SAMPLE_FMT_FLT;
        if (format.sampleSize() == 64)
            return AV_SAMPLE_FMT_DBL;
    }

    return AV_SAMPLE_FMT_NONE;
}

bool AudioResample::matchesCurrentFormat(
    const AVFrame *frame, const QAudioFormat &outputFormat) const
{
    if (!m_context || !m_hasInputLayout || !frame)
        return false;

    return frame->sample_rate == m_inputSampleRate &&
           frame->format == m_inputSampleFormat &&
           av_channel_layout_compare(&frame->ch_layout, &m_inputLayout) == 0 &&
           outputFormat.sampleRate() == m_outputSampleRate &&
           outputFormat.channelCount() == m_outputChannels &&
           toAvSampleFormat(outputFormat) == m_outputSampleFormat;
}

bool AudioResample::configure(const AVFrame *frame,
                              const QAudioFormat &outputFormat)
{
    close();
    if (!frame || frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0)
        return false;

    const AVSampleFormat outputSampleFormat = toAvSampleFormat(outputFormat);
    if (outputSampleFormat == AV_SAMPLE_FMT_NONE ||
        outputFormat.sampleRate() <= 0 || outputFormat.channelCount() <= 0) {
        qWarning() << "Unsupported QAudio output format:" << outputFormat;
        return false;
    }

    AVChannelLayout outputLayout = {};
    av_channel_layout_default(&outputLayout, outputFormat.channelCount());

    const int ret = swr_alloc_set_opts2(
        &m_context,
        &outputLayout,
        outputSampleFormat,
        outputFormat.sampleRate(),
        &frame->ch_layout,
        static_cast<AVSampleFormat>(frame->format),
        frame->sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&outputLayout);

    if (ret < 0 || !m_context || swr_init(m_context) < 0) {
        qWarning() << "Cannot initialize the audio resampler";
        close();
        return false;
    }

    if (av_channel_layout_copy(&m_inputLayout, &frame->ch_layout) < 0) {
        close();
        return false;
    }

    m_hasInputLayout = true;
    m_inputSampleRate = frame->sample_rate;
    m_inputSampleFormat = static_cast<AVSampleFormat>(frame->format);
    m_outputSampleRate = outputFormat.sampleRate();
    m_outputChannels = outputFormat.channelCount();
    m_outputSampleFormat = outputSampleFormat;
    return true;
}

bool AudioResample::convert(const AVFrame *frame,
                            const QAudioFormat &outputFormat,
                            QByteArray *pcmData)
{
    if (!frame || !pcmData || frame->nb_samples <= 0)
        return false;

    if (!matchesCurrentFormat(frame, outputFormat) &&
        !configure(frame, outputFormat)) {
        return false;
    }

    const int outputSamples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(m_context, m_inputSampleRate) + frame->nb_samples,
        m_outputSampleRate,
        m_inputSampleRate,
        AV_ROUND_UP));
    const int bytesPerSample = av_get_bytes_per_sample(m_outputSampleFormat);
    if (outputSamples <= 0 || bytesPerSample <= 0)
        return false;

    pcmData->resize(outputSamples * m_outputChannels * bytesPerSample);
    uint8_t *outputData[] = {
        reinterpret_cast<uint8_t *>(pcmData->data())
    };
    const uint8_t *const *inputData = frame->extended_data;
    const int convertedSamples = swr_convert(
        m_context,
        outputData,
        outputSamples,
        inputData,
        frame->nb_samples);
    if (convertedSamples < 0) {
        pcmData->clear();
        qWarning() << "Audio resampling failed:" << convertedSamples;
        return false;
    }

    pcmData->resize(convertedSamples * m_outputChannels * bytesPerSample);
    return !pcmData->isEmpty();
}

void AudioResample::close()
{
    swr_free(&m_context);
    if (m_hasInputLayout)
        av_channel_layout_uninit(&m_inputLayout);

    m_hasInputLayout = false;
    m_inputSampleRate = 0;
    m_inputSampleFormat = AV_SAMPLE_FMT_NONE;
    m_outputSampleRate = 0;
    m_outputChannels = 0;
    m_outputSampleFormat = AV_SAMPLE_FMT_NONE;
}
