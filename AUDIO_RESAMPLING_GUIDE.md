# 音频基础与 FFmpeg 重采样学习指南

## 1. 文档目标

本文结合 MyPlay2 当前的音频代码，说明以下内容：

- 数字音频、PCM、采样率、采样格式和声道布局的基本概念。
- packed（交错）与 planar（平面）音频的内存区别。
- 为什么解码后的音频通常不能直接交给声卡播放。
- FFmpeg `libswresample` 的核心 API、参数、返回值和生命周期。
- `AudioResample::convert()` 的具体执行过程。
- 音频解码、重采样和 Qt 播放之间的关系。

当前项目音频路径：

```text
媒体文件
  ↓ MyDemux::read(&packet)
音频 AVPacket
  ↓ audioThread / MyDecode
解码后的 AVFrame
  ↓ AudioResample::convert()
设备要求的 packed PCM（QByteArray）
  ↓ PlayAudio::write()
QAudioOutput / 声卡
```

相关代码：

- [`audiothread.cpp`](audiothread.cpp)：音频包队列、解码循环和 EOF 排空。
- [`audioresample.cpp`](audioresample.cpp)：采样率、采样格式和声道布局转换。
- [`playaudio.cpp`](playaudio.cpp)：Qt 音频设备格式选择和 PCM 写入。
- [`mydecode.cpp`](mydecode.cpp)：FFmpeg `sendPacket/receiveFrame` 解码接口。

## 2. 声音如何变成数字音频

现实中的声音是连续变化的空气振动。计算机通过固定时间间隔测量声音振幅，把连续信号变成离散数字序列。

```text
模拟声音 → 采样 → 量化 → PCM 数字样本
```

一次测量得到的数值称为一个 sample（样本）。把样本按照时间顺序保存，就是最基础的 PCM 音频。

### 2.1 采样率

采样率表示每个声道每秒采集多少个样本，单位是 Hz。

常见采样率：

| 采样率 | 常见用途 |
| --- | --- |
| 8000 Hz | 传统电话语音 |
| 16000 Hz | 语音识别、网络语音 |
| 44100 Hz | CD 音频 |
| 48000 Hz | 视频、电影、专业音频 |
| 96000 Hz | 高采样率制作环境 |

根据奈奎斯特采样定理，要还原最高频率为 `f` 的带限信号，采样率理论上必须大于 `2f`。

当前 `PlayAudio` 优先请求：

```cpp
requested.setSampleRate(48000);
```

### 2.2 位深和采样格式

位深表示一个样本使用多少位保存。常见格式包括：

| FFmpeg 格式 | 含义 | 每个样本字节数 |
| --- | --- | ---: |
| `AV_SAMPLE_FMT_U8` | 8 位无符号整数 | 1 |
| `AV_SAMPLE_FMT_S16` | 16 位有符号整数 | 2 |
| `AV_SAMPLE_FMT_S32` | 32 位有符号整数 | 4 |
| `AV_SAMPLE_FMT_S64` | 64 位有符号整数 | 8 |
| `AV_SAMPLE_FMT_FLT` | 32 位浮点数 | 4 |
| `AV_SAMPLE_FMT_DBL` | 64 位浮点数 | 8 |

整数 PCM 通常将最小负数映射到最小振幅，最大正数映射到最大振幅。浮点 PCM 通常使用 `-1.0` 到 `1.0` 附近的范围。

FFmpeg 查询单个样本字节数：

```cpp
int bytes = av_get_bytes_per_sample(sampleFormat);
```

### 2.3 声道数与声道布局

声道数只说明有几个声道，声道布局还说明每个声道的空间含义。

例如双声道一般是：

```text
FL = Front Left
FR = Front Right
```

5.1 声道可能包含：

```text
FL + FR + FC + LFE + SL + SR
```

只有 `channelCount == 6` 并不能完整说明六个声道分别是什么，因此 FFmpeg 使用 `AVChannelLayout` 保存布局。

当前项目根据 Qt 输出声道数创建默认布局：

```cpp
AVChannelLayout outputLayout = {};
av_channel_layout_default(&outputLayout, outputFormat.channelCount());
```

对于普通单声道和双声道，这种处理通常足够。特殊多声道设备最好使用设备提供的准确布局。

## 3. sample、audio frame 与 nb_samples

这是学习 FFmpeg 音频时最容易混淆的部分。

- sample：一个声道在某个时刻的一个采样值。
- sample frame：同一时刻所有声道的一组采样值。
- `AVFrame::nb_samples`：每个声道包含的样本数量，不是所有声道相加后的数量。

例如：

```text
采样率：48000 Hz
声道数：2
格式：S16
nb_samples：1024
```

这一帧的持续时间为：

```text
1024 / 48000 ≈ 0.02133 秒 ≈ 21.33 ms
```

packed PCM 总字节数为：

```text
nb_samples × channelCount × bytesPerSample
= 1024 × 2 × 2
= 4096 字节
```

未压缩 PCM 的理论数据率：

```text
sampleRate × channelCount × bitsPerSample
```

48 kHz、双声道、S16：

```text
48000 × 2 × 16 = 1,536,000 bit/s
48000 × 2 × 2  =   192,000 byte/s
```

## 4. packed 与 planar 内存布局

FFmpeg 音频格式名称末尾带 `P` 时表示 planar。

### 4.1 packed：所有声道交错排列

双声道 S16 的 `AV_SAMPLE_FMT_S16`：

```text
data[0] → L0 R0 L1 R1 L2 R2 L3 R3 ...
```

所有数据通常都从 `data[0]` 或 `extended_data[0]` 读取。

### 4.2 planar：每个声道单独存放

双声道 S16P 的 `AV_SAMPLE_FMT_S16P`：

```text
extended_data[0] → L0 L1 L2 L3 ...
extended_data[1] → R0 R1 R2 R3 ...
```

常见对应关系：

| packed | planar |
| --- | --- |
| `AV_SAMPLE_FMT_U8` | `AV_SAMPLE_FMT_U8P` |
| `AV_SAMPLE_FMT_S16` | `AV_SAMPLE_FMT_S16P` |
| `AV_SAMPLE_FMT_S32` | `AV_SAMPLE_FMT_S32P` |
| `AV_SAMPLE_FMT_FLT` | `AV_SAMPLE_FMT_FLTP` |
| `AV_SAMPLE_FMT_DBL` | `AV_SAMPLE_FMT_DBLP` |

判断格式是否为 planar：

```cpp
int planar = av_sample_fmt_is_planar(sampleFormat);
```

`AVFrame::data` 只有固定数量的指针槽；声道较多时应优先使用：

```cpp
frame->extended_data
```

当前代码将它直接交给 `swr_convert()`：

```cpp
const uint8_t *const *inputData = frame->extended_data;
```

因此 packed 和 planar 输入都可以由 `libswresample` 正确解释。

## 5. 为什么需要重采样

解码器输出格式由媒体文件和解码器决定，声卡支持的格式由设备决定，两者经常不同。

例如解码器输出：

```text
44100 Hz
5.1 声道
AV_SAMPLE_FMT_FLTP
```

设备要求：

```text
48000 Hz
双声道
16 位有符号小端 packed PCM
```

因此需要同时完成三类转换：

1. 采样率转换：44100 Hz → 48000 Hz。
2. 采样格式转换：FLTP → S16。
3. 声道重混：5.1 → stereo。

日常虽然统称“重采样”，但 `libswresample` 实际同时负责 resampling、sample format conversion 和 channel rematrixing。

## 6. 当前 Qt 输出格式

`PlayAudio::selectOutputFormat()` 优先请求：

```text
codec       = audio/pcm
sampleRate  = 48000
channels    = 2
sampleSize  = 16
sampleType  = SignedInt
byteOrder   = LittleEndian
```

对应 FFmpeg：

```cpp
AV_SAMPLE_FMT_S16
```

如果设备不支持，Qt 使用：

```cpp
device.nearestFormat(requested);
```

`AudioResample::toAvSampleFormat()` 再把 `QAudioFormat` 映射为 `AVSampleFormat`。

当前映射支持：

```text
U8、S16、S32、FLT、DBL，小端 packed 格式
```

如果 `nearestFormat()` 返回 24 位整数或其他未映射格式，当前转换会失败，这是现有实现的限制之一。

## 7. SwrContext 生命周期

`SwrContext` 是 `libswresample` 的核心上下文，保存输入格式、输出格式、滤波器状态和延迟样本。

标准生命周期：

```text
swr_alloc_set_opts2()
        ↓
swr_init()
        ↓
重复 swr_convert()
        ↓
输入结束时排空重采样器
        ↓
swr_free()
```

不要混淆：

```text
swr_close()  关闭内部状态，但不释放 SwrContext 本身
swr_free()   释放上下文，并把指针设置为 nullptr
```

## 8. 配置重采样器

当前配置代码的核心是：

```cpp
int swr_alloc_set_opts2(
    SwrContext **context,
    const AVChannelLayout *outputLayout,
    AVSampleFormat outputFormat,
    int outputSampleRate,
    const AVChannelLayout *inputLayout,
    AVSampleFormat inputFormat,
    int inputSampleRate,
    int logOffset,
    void *logContext);
```

当前调用：

```cpp
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
```

参数对应关系：

| 参数 | 当前来源 |
| --- | --- |
| 输出声道布局 | Qt 输出声道数生成的默认布局 |
| 输出采样格式 | `toAvSampleFormat(outputFormat)` |
| 输出采样率 | `QAudioFormat::sampleRate()` |
| 输入声道布局 | `AVFrame::ch_layout` |
| 输入采样格式 | `AVFrame::format` |
| 输入采样率 | `AVFrame::sample_rate` |

配置完成后必须初始化：

```cpp
int ret = swr_init(m_context);
```

返回值：

```text
>= 0  初始化成功
< 0   初始化失败，返回 FFmpeg 错误码
```

检查上下文是否已经初始化：

```cpp
int initialized = swr_is_initialized(m_context);
```

## 9. AVChannelLayout 的所有权

`AVChannelLayout` 内部可能持有动态数据，因此不能假设普通赋值永远安全。

推荐接口：

```cpp
av_channel_layout_default(&layout, channelCount);
av_channel_layout_copy(&destination, &source);
av_channel_layout_compare(&left, &right);
av_channel_layout_check(&layout);
av_channel_layout_describe(&layout, buffer, bufferSize);
av_channel_layout_uninit(&layout);
```

当前代码正确地在临时输出布局使用结束后调用：

```cpp
av_channel_layout_uninit(&outputLayout);
```

缓存输入布局时使用深拷贝：

```cpp
av_channel_layout_copy(&m_inputLayout, &frame->ch_layout);
```

释放缓存时调用：

```cpp
av_channel_layout_uninit(&m_inputLayout);
```

## 10. 为什么需要保存当前输入输出格式

`SwrContext` 初始化成本高于单次转换，不应该每个 `AVFrame` 都重新创建。

当前 `matchesCurrentFormat()` 比较：

```text
输入采样率
输入采样格式
输入声道布局
输出采样率
输出声道数
输出采样格式
```

格式没有变化：复用 `m_context`。

格式发生变化：调用 `configure()` 关闭旧上下文并重新初始化。

媒体中途发生格式切换并不常见，但网络流、拼接文件和某些异常媒体可能出现，所以转换函数不能永远假定格式固定。

## 11. 输出样本容量计算

输入 1024 个样本不代表一定输出 1024 个样本。采样率变化以及重采样器内部延迟都会影响输出数量。

当前公式：

```cpp
const int outputSamples = static_cast<int>(av_rescale_rnd(
    swr_get_delay(m_context, m_inputSampleRate) + frame->nb_samples,
    m_outputSampleRate,
    m_inputSampleRate,
    AV_ROUND_UP));
```

对应数学形式：

```text
outputSamples = ceil(
    (delayInInputSamples + inputSamples)
    × outputRate / inputRate
)
```

### `swr_get_delay()`

```cpp
int64_t swr_get_delay(SwrContext *context, int64_t base);
```

它返回重采样器当前保存的延迟，单位由 `base` 指定。

当前传入：

```cpp
base = m_inputSampleRate
```

所以返回值可以直接与 `frame->nb_samples` 相加。

### `av_rescale_rnd()`

```cpp
int64_t av_rescale_rnd(
    int64_t value,
    int64_t numerator,
    int64_t denominator,
    AVRounding rounding);
```

它在避免普通整数乘法溢出的同时完成比例换算。这里使用 `AV_ROUND_UP`，保证输出缓冲宁可稍大，也不能少分配。

也可以使用：

```cpp
int outputSamples = swr_get_out_samples(context, inputSamples);
```

它返回下一次转换可能需要的输出样本数上界。

## 12. 输出缓冲区大小

当前输出是 packed PCM，因此总字节数为：

```cpp
outputSamples * outputChannels * bytesPerSample
```

代码先按照最大容量分配：

```cpp
pcmData->resize(
    outputSamples * m_outputChannels * bytesPerSample);
```

然后设置输出平面：

```cpp
uint8_t *outputData[] = {
    reinterpret_cast<uint8_t *>(pcmData->data())
};
```

只有一个输出指针，是因为输出格式为 packed。若输出格式改成 `S16P`、`FLTP` 等 planar 格式，就必须为每个声道准备独立的输出平面。

FFmpeg 也提供通用大小计算接口：

```cpp
int av_samples_get_buffer_size(
    int *linesize,
    int channelCount,
    int sampleCount,
    AVSampleFormat sampleFormat,
    int align);
```

## 13. 执行 swr_convert

核心接口：

```cpp
int swr_convert(
    SwrContext *context,
    uint8_t *const *output,
    int outputCapacity,
    const uint8_t *const *input,
    int inputSamples);
```

需要注意：

- `outputCapacity` 是每个声道最多容纳的样本数，不是字节数。
- `inputSamples` 是每个声道的输入样本数，即 `frame->nb_samples`。
- 返回值是每个声道实际输出的样本数，不是字节数。
- 返回负数表示 FFmpeg 错误码。

当前调用：

```cpp
const int convertedSamples = swr_convert(
    m_context,
    outputData,
    outputSamples,
    frame->extended_data,
    frame->nb_samples);
```

转换完成后，使用实际输出数量缩小 `QByteArray`：

```cpp
pcmData->resize(
    convertedSamples * m_outputChannels * bytesPerSample);
```

这一点很重要，因为预估容量可能大于实际输出数量。不能把未写入的尾部空间送给声卡。

## 14. 解码器排空与重采样器排空

这里存在两套不同的内部缓存。

### 14.1 排空音频解码器

媒体包结束后：

```cpp
m_decoder.sendPacket(nullptr);
```

然后持续调用：

```cpp
m_decoder.receiveFrame(frame);
```

直到返回 `AVERROR_EOF`。这是取出音频解码器中的延迟帧。

### 14.2 排空重采样器

即使解码器已经排空，`SwrContext` 内部仍可能保留少量延迟样本。排空方式是使用空输入调用 `swr_convert()`：

```cpp
int converted = swr_convert(
    context,
    outputData,
    outputCapacity,
    nullptr,
    0);
```

重复调用，直到返回 `0`。

当前 MyPlay2 在结束时直接调用 `m_resample.close()`，尚未显式排空 `SwrContext`，因此变采样率音频的尾部理论上可能少量丢样。学习或完善播放器时，可以为 `AudioResample` 增加 `flush()` 接口。

不要把排空和清空混淆：

```text
drain/flush output  把剩余有效数据输出
discard/reset       丢弃内部数据并重置状态
```

## 15. AudioResample::convert() 完整流程

当前函数可以拆成以下步骤：

```text
1. 检查 frame、pcmData 和 nb_samples
2. 比较本帧格式与当前 SwrContext 配置
3. 格式变化时重新 configure()
4. 查询 swr_get_delay()
5. 计算最大输出样本数
6. 根据输出格式计算每样本字节数
7. 调整 QByteArray 到最大容量
8. 使用 frame->extended_data 调用 swr_convert()
9. 根据实际输出样本数缩小 QByteArray
10. 把 packed PCM 交给 PlayAudio
```

可以把关键变量理解为：

| 变量 | 单位 | 含义 |
| --- | --- | --- |
| `frame->nb_samples` | 样本/声道 | 本帧输入数量 |
| `outputSamples` | 样本/声道 | 输出缓冲最大容量 |
| `convertedSamples` | 样本/声道 | 实际转换数量 |
| `bytesPerSample` | 字节/样本 | 单个声道的一个样本大小 |
| `pcmData->size()` | 字节 | 所有声道的 packed PCM 总大小 |

## 16. PlayAudio 如何消费 PCM

`QAudioOutput::start()` 在 Push 模式下返回一个 `QIODevice *`：

```cpp
m_device = start();
```

之后可以向设备写入 PCM：

```cpp
m_device->write(data, size);
```

声卡缓冲区可能暂时装不下整块数据，因此当前代码先查询：

```cpp
int available = bytesFree();
```

每次只写当前能容纳的部分。`audioThread::writePcm()` 循环处理部分写入：

```text
written > 0  更新偏移，继续写剩余数据
written = 0  缓冲区暂满，等待 5 ms 后重试
written < 0  写入失败
```

当前设备缓冲设置为约 200 ms：

```cpp
setBufferSize(format().bytesForDuration(200000));
```

Qt 5 的 `bytesForDuration()` 使用微秒，因此 `200000` 表示 200 ms。

## 17. 音频时间戳与播放时长

重采样只解决格式兼容，不自动解决音画同步。

解码音频帧常用时间信息：

```cpp
frame->pts
frame->time_base
frame->nb_samples
frame->sample_rate
```

一帧音频自身持续时间可以用：

```text
duration = nb_samples / sample_rate
```

播放器通常选择音频作为主时钟，因为声卡按照固定采样率持续消费 PCM。视频根据音频时钟和视频 PTS 决定等待、显示或丢帧。

当前项目已经以 `QAudioOutput::processedUSecs()` 为基础建立音频主时钟。视频线程把视频帧 PTS 换算成微秒后与音频时钟比较：视频早则等待，落后超过约一帧则丢帧。音频时钟不可用时，视频使用自身 PTS 和单调时钟调度。

## 18. 常用 FFmpeg 音频 API 速查

### 解码相关

| API | 作用 |
| --- | --- |
| `avcodec_find_decoder()` | 根据 codec id 查找音频解码器 |
| `avcodec_alloc_context3()` | 创建解码上下文 |
| `avcodec_parameters_to_context()` | 把流参数复制到解码上下文 |
| `avcodec_open2()` | 打开解码器 |
| `avcodec_send_packet()` | 向解码器发送压缩包；`nullptr` 表示输入结束 |
| `avcodec_receive_frame()` | 获取解码后的音频 `AVFrame` |
| `avcodec_flush_buffers()` | seek 后丢弃解码器旧缓存 |
| `avcodec_free_context()` | 释放解码上下文 |

### AVFrame 相关

| API/字段 | 作用 |
| --- | --- |
| `av_frame_alloc()` | 创建空 `AVFrame` |
| `av_frame_unref()` | 释放当前帧引用，以便复用结构体 |
| `av_frame_free()` | 释放 `AVFrame` 本身 |
| `frame->sample_rate` | 输入采样率 |
| `frame->format` | `AVSampleFormat`，字段类型表现为整数 |
| `frame->ch_layout` | 输入声道布局 |
| `frame->nb_samples` | 每个声道的样本数 |
| `frame->extended_data` | packed 或 planar 输入平面指针 |

### 采样格式相关

| API | 作用 |
| --- | --- |
| `av_get_sample_fmt_name()` | 获取采样格式名称 |
| `av_get_bytes_per_sample()` | 查询单个样本字节数 |
| `av_sample_fmt_is_planar()` | 判断是否为 planar |
| `av_get_packed_sample_fmt()` | 获取对应 packed 格式 |
| `av_get_planar_sample_fmt()` | 获取对应 planar 格式 |
| `av_samples_get_buffer_size()` | 计算音频缓冲区大小 |

### 声道布局相关

| API | 作用 |
| --- | --- |
| `av_channel_layout_default()` | 根据声道数生成默认布局 |
| `av_channel_layout_copy()` | 深拷贝布局 |
| `av_channel_layout_compare()` | 比较两个布局 |
| `av_channel_layout_check()` | 检查布局是否合法 |
| `av_channel_layout_describe()` | 生成便于阅读的布局名称 |
| `av_channel_layout_uninit()` | 释放布局内部资源 |

### libswresample 相关

| API | 作用 |
| --- | --- |
| `swr_alloc()` | 创建空重采样上下文 |
| `swr_alloc_set_opts2()` | 创建或配置输入输出格式 |
| `swr_init()` | 初始化上下文 |
| `swr_is_initialized()` | 检查是否已经初始化 |
| `swr_get_delay()` | 查询当前重采样延迟 |
| `swr_get_out_samples()` | 估算下一次最大输出样本数 |
| `swr_convert()` | 转换音频样本 |
| `swr_convert_frame()` | 使用 `AVFrame` 输入输出的高层转换接口 |
| `swr_close()` | 关闭内部状态但不释放上下文 |
| `swr_free()` | 释放上下文并置空指针 |

## 19. 常见错误

### 把 nb_samples 当成总样本数

错误理解：

```text
双声道 nb_samples=1024，一共只有 1024 个采样值
```

正确理解：

```text
每个声道 1024 个样本，共有 1024 × 2 个采样值
```

### 只读取 frame->data[0]

packed 输入通常可以从一个平面读取，但 planar 输入的其他声道位于其他平面。应把 `frame->extended_data` 交给 `swr_convert()`。

### 把 swr_convert 返回值当成字节数

它返回的是每个声道的输出样本数。packed 总字节数还要乘：

```text
声道数 × 每样本字节数
```

### 输出缓冲只按输入样本数分配

升采样时输出样本可能多于输入样本，而且还要考虑 `swr_get_delay()`。

### 忘记处理部分写入

`QIODevice::write()` 不保证一次写完全部 PCM，必须根据返回值更新偏移。

### 忘记释放 AVChannelLayout

通过 `av_channel_layout_copy()` 等方式初始化的布局，结束时需要 `av_channel_layout_uninit()`。

### 只排空解码器，不排空重采样器

解码器 EOF 不代表 `SwrContext` 已经没有延迟样本。需要空输入调用 `swr_convert()` 才能完整输出尾部。

## 20. 调试建议

配置重采样器时可以打印输入输出信息：

```cpp
char inputLayout[128] = {};
av_channel_layout_describe(
    &frame->ch_layout,
    inputLayout,
    sizeof(inputLayout));

qDebug() << "input rate:" << frame->sample_rate
         << "input format:"
         << av_get_sample_fmt_name(
                static_cast<AVSampleFormat>(frame->format))
         << "input layout:" << inputLayout
         << "nb_samples:" << frame->nb_samples;
```

建议重点观察：

```text
输入/输出采样率是否正确
输入是 packed 还是 planar
声道布局是否有效
outputSamples 是否足够
convertedSamples 是否长期为 0
QAudioOutput 是否处于 ActiveState 或 IdleState
QAudioOutput::error() 是否异常
```

FFmpeg 错误码可以转换为文字：

```cpp
char errorText[AV_ERROR_MAX_STRING_SIZE] = {};
av_strerror(ret, errorText, sizeof(errorText));
```

## 21. 推荐学习顺序

1. 先掌握 sample、sample rate、channel 和 `nb_samples`。
2. 手动画出 S16 packed 与 S16P planar 的内存布局。
3. 理解 `AVFrame::extended_data` 为什么是二级指针。
4. 理解 `swr_alloc_set_opts2()` 的六个核心格式参数。
5. 手算一帧 PCM 的持续时间和字节数。
6. 理解 `swr_get_delay()` 和输出容量公式。
7. 单步观察一次 `swr_convert()` 的输入和返回值。
8. 最后学习重采样器排空、音频时钟和音画同步。

最需要记住的公式与规则：

```text
帧时长 = nb_samples / sample_rate

packed PCM 字节数 =
    samplesPerChannel × channels × bytesPerSample

最大输出样本数 ≈
    ceil((resampleDelay + inputSamples) × outRate / inRate)

swr_convert() 返回值 = 每个声道实际输出的样本数
```
