# MyPlay2 音视频同步完整调用逻辑

## 1. 先看最核心的具体逻辑

MyPlay2 采用“音频作为主时钟，视频同步音频”的方案。整个同步过程分成前后两个阶段：

```text
阶段一：audioThread 建立并发布音频时钟

音频 AVFrame
  → AudioResample::convert() 生成声卡格式 PCM
  → audioThread::tryStartClock() 建立媒体时间原点
  → audioThread::writePcm() 写入 QAudioOutput
  → audioThread::refreshClock() 发布三个原子变量

阶段二：videoThread 读取音频时钟并处理视频帧

视频 AVFrame
  → videoThread::synchronizeFrame()
  → frameTimestampUs() 取得视频时间
  → audioThread::clockUs() 取得音频时间
  → videoPtsUs - audioClockUs
       正数：视频提前，等待
       接近 0：正常显示
       小于负阈值：视频落后，丢帧
```

最关键的调用是：

```cpp
qint64 audioClockUs = m_audioClockSource->clockUs();
```

`clockUs()` 表示：

```text
当前音频估计已经播放到媒体时间轴的哪个位置
```

例如返回 `2128000` 微秒，表示音频大约播放到媒体第 `2.128` 秒。

便于记忆的近似公式：

```text
当前音频时间 ≈
    音频帧 PTS
    + QAudioOutput 实际处理时长
    + 两次采样之间经过的单调时间
```

项目实际使用的完整公式：

```text
音频时间原点 =
    第一个有效音频帧 PTS
    - 此前已经提交的 PCM 时长

最近一次音频时钟采样 =
    音频时间原点
    + QAudioOutput::processedUSecs()

videoThread 读取到的音频时钟 =
    min(
        最近一次音频时钟采样 + 距离采样经过的单调时间,
        已经提交的 PCM 结束时间
    )
```

## 2. Widget 怎样让 videoThread 知道 audioThread

### 2.1 Widget 同时持有两个线程对象

[`widget.h`](widget.h) 中：

```cpp
videoThread m_videoThread;
audioThread m_audioThread;
```

`Widget` 是协调者。它先启动音频线程：

```cpp
audioStarted = m_audioThread.open(
    audioParameters,
    m_demux.audioTimeBase());
```

再把音频线程对象的地址交给视频线程：

```cpp
m_videoThread.open(
    videoParameters,
    m_demux.videoTimeBase(),
    audioStarted ? &m_audioThread : nullptr);
```

`videoThread::open()` 保存这个地址：

```cpp
m_audioClockSource = audioClockSource;
```

因此后面可以调用：

```cpp
m_audioClockSource->clockUs();
```

这里没有复制新的 `audioThread`，只是保存 `Widget::m_audioThread` 的指针。如果音频启动失败，就传入 `nullptr`，视频改用自身时钟。

停止播放时先停止视频，再停止音频：

```cpp
m_videoThread.stopVideo();
m_audioThread.stopAudio();
```

这样能保证视频线程停止读取音频时钟后，音频线程才停止。

## 3. PTS 和时间基怎样进入线程

PTS 本身只是整数刻度：

```text
实际时间（秒） = PTS × time_base
```

例如时间基 `{1, 8000}` 表示一个刻度为 `1/8000` 秒。PTS 为 `16000` 时：

```text
16000 × 1/8000 = 2 秒
```

[`MyDemux::audioTimeBase()`](mydemux.cpp) 和 `videoTimeBase()` 分别返回对应的：

```cpp
format->streams[streamIndex]->time_base;
```

音频和视频时间基通常不同，不能直接比较两者的原始 PTS。

[`MyDecode::open()`](mydecode.cpp) 还会设置：

```cpp
codecCtx->pkt_timebase = packetTimeBase;
```

这样 FFmpeg 才知道输入 `AVPacket::pts/dts` 的单位，并能正确推导解码帧的 `best_effort_timestamp`。

项目把音频和视频时间都统一换算成微秒：

```cpp
qint64 ptsUs = av_rescale_q(
    timestamp,
    streamTimeBase,
    AV_TIME_BASE_Q);
```

其中：

```text
AV_TIME_BASE_Q = {1, 1000000}
```

## 4. 阶段一：音频时钟完整调用链

```text
audioThread::run()
  → MyDecode::receiveFrame(frame)
  → AudioResample::convert(frame, outputFormat, &pcmData)
  → audioThread::tryStartClock(frame)
  → audioThread::writePcm(pcmData)
       → PlayAudio::write()
       → m_submittedBytes += written
       → audioThread::refreshClock()
            → submittedDurationUs()
            → QAudioOutput::processedUSecs()
            → steadyClockUs()
            → store() 发布三个原子变量

videoThread 需要时：
  → audioThread::clockUs()
       → load() 读取三个原子变量
       → steadyClockUs() 计算采样后经过时间
       → qMin() 限制不能超过 PCM 末尾
```

## 5. `audioThread::run()`：取得音频帧

[`audioThread::run()`](audiothread.cpp) 首先调用：

```cpp
int ret = m_decoder.receiveFrame(frame);
```

返回 `0` 表示成功取得一帧解码音频，随后执行：

```cpp
QByteArray pcmData;
if (m_resample.convert(frame,
                       m_playAudio->format(),
                       &pcmData)) {
    tryStartClock(frame);
    writePcm(pcmData);
}
```

真实顺序是：

```text
重采样成功
  → 尝试建立时钟
  → 写入 PCM
```

只有生成了可播放 PCM 才启动音频时钟。否则重采样失败时，视频可能等待一个根本不会前进的时钟。

## 6. `AudioResample::convert()`：生成声卡格式 PCM

解码器输出可能是 44100 Hz、FLTP 平面浮点音频，而 `QAudioOutput` 可能要求 48000 Hz、S16 交错音频。

`AudioResample::convert()` 使用 `swr_convert()` 完成采样率、采样格式和声道布局转换，最终生成 `QByteArray pcmData`。

同步时统计的是重采样后真正成功写入声卡的字节数，不能使用解码帧原始字节数。

## 7. `tryStartClock()`：建立音频时间原点

### 7.1 选择时间戳

```cpp
int64_t timestamp = frame->best_effort_timestamp;
if (timestamp == AV_NOPTS_VALUE)
    timestamp = frame->pts;
```

优先使用 `best_effort_timestamp`，缺失时退回 `pts`。如果两者都无效，音频仍可播放，但暂时无法建立同步时钟：

```cpp
if (timestamp == AV_NOPTS_VALUE)
    return;
```

### 7.2 换算成微秒

```cpp
const qint64 framePtsUs = av_rescale_q(
    timestamp,
    m_timeBase,
    AV_TIME_BASE_Q);
```

### 7.3 计算音频时间原点

```cpp
m_clockOriginUs =
    framePtsUs - submittedDurationUs();
```

通常第一帧就有 PTS：

```text
framePtsUs          = 2.000 秒
此前提交 PCM 时长   = 0 秒
clockOriginUs       = 2.000 秒
```

如果已经播放了 0.5 秒没有 PTS 的音频，当前帧 PTS 才第一次有效：

```text
当前 framePtsUs     = 2.500 秒
此前提交 PCM 时长   = 0.500 秒
clockOriginUs       = 2.000 秒
```

这样 `QAudioOutput` 从启动时的第 0 秒能映射到媒体第 2 秒。

最后设置：

```cpp
m_clockStarted = true;
refreshClock();
```

## 8. `writePcm()`：写声卡并统计提交量

`writePcm()` 循环调用：

```cpp
const qint64 written = m_playAudio->write(
    pcmData.constData() + offset,
    pcmData.size() - offset);
```

成功写入时：

```cpp
offset += written;
m_submittedBytes += written;
refreshClock();
```

`m_submittedBytes` 表示本次播放累计向设备提交了多少 PCM。

设备缓冲区满时，`PlayAudio::write()` 返回 `0`：

```cpp
if (written == 0) {
    refreshClock();
    QThread::msleep(5);
    continue;
}
```

虽然没有写入新数据，但声卡仍在后台消费已有 PCM，所以继续刷新播放时钟，并休眠 5 ms 避免空转。

注意：写入成功只表示 PCM 进入设备缓冲区，不代表用户已经听到全部数据。

## 9. `submittedDurationUs()`：PCM 字节数换成时长

假设输出格式是 48000 Hz、双声道、16 bit：

```text
bytesPerSample = 16 / 8 = 2 字节
bytesPerFrame  = 2 字节 × 2 声道 = 4 字节
```

代码：

```cpp
const int bytesPerSample =
    outputFormat.sampleSize() / 8;

const int bytesPerFrame =
    bytesPerSample * outputFormat.channelCount();

const qint64 submittedSamples =
    m_submittedBytes / bytesPerFrame;

return av_rescale(
    submittedSamples,
    AV_TIME_BASE,
    outputFormat.sampleRate());
```

例如提交 `192000` 字节：

```text
sample frame 数 = 192000 / 4 = 48000
持续时间         = 48000 / 48000 = 1 秒
```

这个时长表示已经提交 PCM 的范围上限，不等于声卡已经播放的时长。

## 10. `refreshClock()`：产生三个原子变量

### 10.1 取得 QAudioOutput 实际处理时长

```cpp
qint64 playedUs =
    m_clockOriginUs
    + m_playAudio->processedUSecs();
```

`processedUSecs()` 表示 `QAudioOutput` 从 `start()` 以来已经处理的 PCM 时长。

例如：

```text
clockOriginUs      = 2.000 秒
processedUSecs()   = 0.120 秒
playedUs           = 2.120 秒
```

它比已写入字节数更接近用户听到的位置，但仍是 Qt 音频后端提供的估计。

### 10.2 计算已提交 PCM 的末尾

```cpp
const qint64 submittedEndUs =
    m_clockOriginUs + submittedDurationUs();
```

假设已经提交 0.2 秒 PCM：

```text
submittedEndUs = 2.000 + 0.200 = 2.200 秒
```

限制声卡时间不能超过它：

```cpp
if (playedUs > submittedEndUs)
    playedUs = submittedEndUs;
```

### 10.3 发布三个原子变量

```cpp
m_submittedEndUs.store(
    submittedEndUs,
    std::memory_order_release);

m_clockUpdatedSteadyUs.store(
    steadyClockUs(),
    std::memory_order_release);

m_clockUs.store(
    playedUs,
    std::memory_order_release);
```

| 原子变量 | 含义 |
| --- | --- |
| `m_clockUs` | 最近一次从声卡采样得到的媒体音频位置 |
| `m_clockUpdatedSteadyUs` | 进行这次采样时的单调时钟时间点 |
| `m_submittedEndUs` | 已提交 PCM 在媒体时间轴上的末尾 |

## 11. `steadyClockUs()`：记录时间点而不是函数开销

```cpp
qint64 audioThread::steadyClockUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()
            .time_since_epoch()).count();
}
```

它返回单调时钟的当前时间点，不是在测量函数本身运行多久。

第一次调用：

```text
sampledAtUs = 50000000
```

中间可能经历解码、睡眠、阻塞和线程调度共 20 ms。第二次调用：

```text
nowUs = 50020000
elapsedUs = 50020000 - 50000000 = 20 ms
```

这 20 ms 包含两个时间点之间所有真实经过时间。`steady_clock` 不受修改系统日期、时区和网络校时影响，适合测量播放器经过时长。

## 12. `clockUs()`：videoThread 最终取得音频时间

[`audioThread::clockUs()`](audiothread.cpp) 首先读取三个原子变量：

```cpp
const qint64 sampledClockUs =
    m_clockUs.load(std::memory_order_acquire);

const qint64 sampledAtUs =
    m_clockUpdatedSteadyUs.load(
        std::memory_order_acquire);

const qint64 submittedEndUs =
    m_submittedEndUs.load(
        std::memory_order_acquire);
```

如果时钟尚未建立或已经停止，返回 `audioThread::InvalidClockUs`。它使用 `qint64` 最小值，因为合法媒体 PTS 可能为负数，不能简单用 `-1` 表示无效。

计算距离最近采样经过的时间：

```cpp
const qint64 elapsedUs = qMax<qint64>(
    0,
    steadyClockUs() - sampledAtUs);
```

最终返回：

```cpp
return qMin(
    sampledClockUs + elapsedUs,
    submittedEndUs);
```

例如：

```text
最近采样音频时间   = 2.120 秒
采样后又经过       = 0.008 秒
推算结果           = 2.128 秒
已提交 PCM 末尾    = 2.200 秒
最终返回           = 2.128 秒
```

如果只提交到 2.125 秒，最终返回会被限制为 2.125 秒。

## 13. 为什么这里需要 `std::atomic`

现在有两个真正并行的线程：

```text
audioThread::run()  写三个时钟变量
videoThread::run()  通过 clockUs() 读取三个时钟变量
```

如果使用普通变量：

```cpp
qint64 m_clockUs;
```

一个线程写、另一个线程同时读，会形成 data race（数据竞争）。C++ 中的数据竞争属于未定义行为，不只是“偶尔读到旧值”。

可能表现为：

- Debug 正常而 Release 异常。
- 视频线程长时间看不到新值。
- 某些架构读取到不完整数值。
- 编译器基于“程序没有数据竞争”的假设进行意外优化。

使用：

```cpp
std::atomic<qint64>
```

可以保证单次 `load()` 和 `store()` 是原子访问，不会与另一个线程的原子访问形成数据竞争。

`volatile` 不能代替 `atomic`。`volatile` 主要用于硬件寄存器等场景，不会建立 C++ 线程之间的同步关系。

## 14. 为什么 store 使用 `memory_order_release`

音频线程发布快照时：

```cpp
// 先准备配套数据。
m_submittedEndUs.store(endUs,
                       std::memory_order_release);
m_clockUpdatedSteadyUs.store(nowUs,
                             std::memory_order_release);

// 最后发布主要的音频时钟值。
m_clockUs.store(playedUs,
                std::memory_order_release);
```

`release` 可以简化理解为：

```text
我之前准备的数据已经完成，现在正式发布给其他线程
```

这里先发布 PCM 末尾和采样时刻，最后发布采样音频时钟。`release` 约束它之前的操作，防止它们在跨线程可见性意义上被移动到发布之后。

## 15. 为什么 load 使用 `memory_order_acquire`

视频线程通过 `clockUs()` 首先读取：

```cpp
const qint64 sampledClockUs =
    m_clockUs.load(std::memory_order_acquire);
```

`acquire` 可以简化理解为：

```text
我已经接收到写线程的发布，现在读取这次发布对应的数据
```

如果 acquire 读取到了音频线程某次 release 发布的值，就建立：

```text
音频线程 release 之前的操作
        happens-before
视频线程 acquire 之后的操作
```

当前实现对三个原子读取都使用 acquire，三个写入都使用 release。这是一种偏保守、便于理解的写法。

### 15.1 为什么不直接全部改成 relaxed

`memory_order_relaxed` 只保证当前原子变量自身的读写不可撕裂，不负责把它作为其他数据的发布和接收点。

如果三个操作随意改成 relaxed，虽然单个原子仍没有数据竞争，但不能继续依赖“先准备配套值，最后发布主时钟”的跨线程顺序语义。

### 15.2 为什么不用默认 seq_cst

不指定内存顺序时默认是 `memory_order_seq_cst`：

```cpp
m_abort.store(true);
m_abort.load();
```

`seq_cst` 更强，还为所有同类操作提供统一的全局顺序。当前音频时钟只有发布者和读取者，需要的是发布/接收关系，所以 acquire/release 已经足够，也更明确地表达代码意图。

### 15.3 三个原子变量不是一个事务

三个原子仍然是三个独立对象。视频线程可能恰好遇到音频线程刷新，读到相邻两次刷新中的字段。

当前代码通过“先写上限和采样时刻，最后发布基础时钟”以及 `submittedEndUs` 上限，使结果保持保守可用。同步阈值是毫秒级，因此允许极小的估算误差。

如果以后要求三个字段必须来自完全相同的一次快照，可以使用 `QMutex` 保护一个结构体：

```cpp
struct AudioClockSnapshot
{
    qint64 clockUs;
    qint64 sampledAtUs;
    qint64 submittedEndUs;
};
```

也可以使用版本号实现 sequence lock，但对当前学习阶段没有必要。

### 15.4 atomic 不一定等于底层无锁

`std::atomic<T>` 保证原子语义，但标准不保证所有平台上的所有 `T` 都通过无锁 CPU 指令实现。

可以检查：

```cpp
m_clockUs.is_lock_free();
```

当前 64 位平台通常能高效处理 64 位整数原子操作，但代码不应把 `atomic` 和“标准保证 lock-free”画等号。

## 16. 阶段二：videoThread 处理视频帧的完整调用链

```text
videoThread::run()
  → MyDecode::receiveFrame(frame)
  → videoThread::synchronizeFrame(frame)
       → frameTimestampUs(frame)
            将视频 PTS 换算成微秒
       → m_audioClockSource->clockUs()
            取得当前音频媒体时间
       → differenceUs = videoPtsUs - audioClockUs
            视频提前：循环等待
            视频落后：返回 Drop
            时间接近：返回 Present
            收到停止：返回 Abort

run() 根据结果继续：
  Present → copyYuv420pFrame() → emit frameReady()
  Drop    → av_frame_unref()，不发送画面
  Abort   → av_frame_unref()，退出线程
```

## 17. `videoThread::run()`：视频处理入口

[`videoThread::run()`](videothread.cpp) 调用：

```cpp
int ret = m_decoder.receiveFrame(frame);
```

返回 `0` 表示成功得到视频帧，随后调用：

```cpp
const SyncDecision decision =
    synchronizeFrame(frame);
```

返回类型：

```cpp
enum class SyncDecision
{
    Present,
    Drop,
    Abort
};
```

这样把“时间同步决策”和“真正复制、显示画面”分开。

## 18. `frameTimestampUs()`：取得视频帧媒体时间

先取得适合显示顺序的时间戳：

```cpp
int64_t timestamp = frame->best_effort_timestamp;
if (timestamp == AV_NOPTS_VALUE)
    timestamp = frame->pts;
```

存在 B 帧时，解码顺序可能不同于显示顺序，因此不能直接使用 `pkt_dts` 控制画面显示。

再用视频流时间基换算成微秒：

```cpp
return av_rescale_q(
    timestamp,
    m_timeBase,
    AV_TIME_BASE_Q);
```

如果没有有效时间戳，返回 `AV_NOPTS_VALUE`。`synchronizeFrame()` 无法判断它应该什么时候显示，只能返回 `Present`。

## 19. `synchronizeFrame()`：连接前后两个阶段

### 19.1 取得视频时间

```cpp
const qint64 videoPtsUs =
    frameTimestampUs(frame);
```

### 19.2 调用 clockUs() 取得音频时间

```cpp
qint64 audioClockUs = m_audioClockSource
    ? m_audioClockSource->clockUs()
    : audioThread::InvalidClockUs;
```

这就是两个阶段的连接点：

```text
audioThread 发布三个原子变量
  → clockUs() 计算当前音频媒体时间
  → videoThread 取得 audioClockUs
  → synchronizeFrame() 处理视频帧
```

### 19.3 计算差值

```cpp
qint64 differenceUs =
    videoPtsUs - audioClockUs;
```

| `differenceUs` | 含义 | 处理 |
| ---: | --- | --- |
| 大于 0 | 视频时间更大，视频提前 | 等待 |
| 接近 0 | 音视频接近 | 显示 |
| 小于 0 | 视频时间更小，视频落后 | 立即显示或丢帧 |

## 20. 视频提前时怎样等待

假设：

```text
videoPtsUs    = 2.180 秒
audioClockUs  = 2.128 秒
differenceUs  = +52 ms
```

视频提前，需要等待：

```cpp
while (differenceUs > 2000 && !m_abort.load()) {
    QThread::msleep(sleepMs);

    audioClockUs = m_audioClockSource->clockUs();
    differenceUs = videoPtsUs - audioClockUs;
}
```

调用过程：

```text
synchronizeFrame()
  → 判断 differenceUs > 2000 微秒
  → QThread::msleep()，每次最多等待 10 ms
  → audioThread::clockUs() 重新取得最新音频时间
  → 重新计算 differenceUs
  → 继续等待或返回 Present
```

每次最多等待 10 ms，是为了持续更新音频时钟、及时响应停止，并避免一次睡眠过长造成过冲。

`2000` 微秒是 2 ms 容差。线程调度和音频后端都存在误差，没有必要忙等最后几微秒。

## 21. 视频落后时怎样丢帧

先估计一帧持续时间：

```cpp
qint64 frameDurationUs = 40000;

if (frame->duration > 0) {
    frameDurationUs = av_rescale_q(
        frame->duration,
        m_timeBase,
        AV_TIME_BASE_Q);
}
```

默认 `40000` 微秒，即 40 ms，对应约 25 FPS。

迟到阈值限制到 20~100 ms：

```cpp
const qint64 lateThresholdUs = qBound<qint64>(
    20000,
    qAbs(frameDurationUs),
    100000);
```

这样能避免异常 `duration` 导致轻微抖动就丢帧，或者严重落后仍不丢帧。

判断：

```cpp
if (differenceUs < -lateThresholdUs)
    return SyncDecision::Drop;
```

例如：

```text
audioClockUs       = 2.128 秒
videoPtsUs         = 2.070 秒
differenceUs       = -58 ms
lateThresholdUs    = 40 ms
```

因为 `-58 ms < -40 ms`，当前帧已经明显过时，返回 `Drop`。

`run()` 随后只执行：

```cpp
av_frame_unref(frame);
```

不会复制 YUV，也不会发送给 GUI，而是立即处理下一帧追赶音频。

## 22. 正常显示时调用哪些函数

当 `synchronizeFrame()` 返回 `Present`：

```cpp
QByteArray frameData;
if (copyYuv420pFrame(frame, &frameData))
    emit frameReady(frameData,
                    frame->width,
                    frame->height);
```

完整调用链：

```text
SyncDecision::Present
  → copyYuv420pFrame()
       按 linesize 逐行复制 Y、U、V
  → QByteArray 独立保存紧密 YUV420P
  → emit frameReady()
  → Qt::QueuedConnection
  → GUI 线程中的 lambda
  → videoOpenGLWidget::setFrame()
  → update()
  → paintGL()
```

必须复制 YUV 数据，因为随后会调用：

```cpp
av_frame_unref(frame);
```

如果把 `AVFrame::data[]` 原始指针直接交给 GUI，视频线程复用帧后，这些指针可能失效或指向新数据。

## 23. 没有音频时怎样处理视频

`clockUs()` 返回 `InvalidClockUs` 可能表示：

- 文件没有音频流。
- 音频设备打开失败。
- 音频帧没有有效 PTS。
- 音频已经播放结束。

视频线程使用 `QElapsedTimer` 建立备用时钟：

```cpp
if (!m_videoTimer.isValid()) {
    m_firstVideoPtsUs = videoPtsUs;
    m_videoTimer.start();
}
```

当前视频帧应该出现的相对时间：

```cpp
const qint64 targetElapsedUs =
    videoPtsUs - m_firstVideoPtsUs;
```

实际已经经过的时间：

```cpp
m_videoTimer.nsecsElapsed() / 1000
```

两者相减得到还需要等待多久。这样无音频视频也会按照自身 PTS 播放，而不是瞬间显示完整个文件。

## 24. 收到停止请求时怎样退出

视频等待循环不断检查：

```cpp
m_abort.load()
```

当前没有指定内存顺序，所以这里默认使用 `memory_order_seq_cst`。

如果用户关闭窗口或切换文件，`stopVideo()` 设置退出标志并唤醒线程。`synchronizeFrame()` 返回 `Abort`，`run()` 释放当前帧并退出，避免停止操作长时间等待。

## 25. `waitForDeviceDrain()`：音频结束后为什么还要等

解码器 EOF 不表示声卡播放完成：

```text
解封装器 EOF
  → sendPacket(nullptr) 排空解码器
  → 最后的 AVFrame 转换并写入 QAudioOutput
  → 设备缓冲区可能仍有约 200 ms PCM
```

正常结束时调用：

```cpp
waitForDeviceDrain();
```

内部逻辑：

```cpp
while (!m_abort.load() && timeout.elapsed() < 1000) {
    refreshClock();

    if (m_playAudio->state() == QAudio::IdleState ||
        m_playAudio->state() == QAudio::StoppedState) {
        break;
    }

    QThread::msleep(5);
}
```

它负责：

- 等待设备消费尾部 PCM。
- 等待期间继续刷新音频时钟。
- 用户主动停止时立即结束。
- 最多等待 1 秒，避免异常音频后端永久阻塞线程。

音频结束后把时钟设为无效。如果视频比音频更长，视频尾部会切换到自身 PTS 时钟。

需要区分三个排空层级：

```text
解码器 drain：sendPacket(nullptr)，取出延迟 AVFrame
重采样器 drain：空输入 swr_convert()，取出延迟样本
声卡 drain：waitForDeviceDrain()，等待已写 PCM 被消费
```

当前项目尚未单独排空 `SwrContext`，变采样率时理论上可能遗留少量尾部样本。

## 26. 快速投包和队列背压为什么与同步有关

旧方式如果每 40 ms 才给视频线程一个包，视频落后时即使丢掉一帧，也要再等 40 ms 才有下一包，很难连续追赶。

当前 `Widget` 使用：

```cpp
m_packetTimer.start(1);
```

快速预读，同时限制：

```text
视频队列最多 64 个 packet
音频队列最多 256 个 packet
```

队列满时 `dispatchNextPackets()` 暂停读取，下一次定时器再尝试。这让视频线程可以连续解码和丢帧，又不会把整个文件一次性读进内存。

## 27. 两个阶段调用的函数对照表

### 27.1 音频时钟产生和读取

| 顺序 | 函数 | 功能 |
| ---: | --- | --- |
| 1 | `MyDemux::audioTimeBase()` | 取得音频 PTS 的单位 |
| 2 | `MyDecode::open()` | 设置 `pkt_timebase` 并打开解码器 |
| 3 | `audioThread::run()` | 驱动音频解码循环 |
| 4 | `MyDecode::receiveFrame()` | 取得音频 `AVFrame` |
| 5 | `AudioResample::convert()` | 转换成声卡要求的 PCM |
| 6 | `audioThread::tryStartClock()` | 用音频 PTS 建立媒体时间原点 |
| 7 | `audioThread::writePcm()` | 分段写入声卡并累计成功字节数 |
| 8 | `PlayAudio::write()` | 把 PCM 写入音频设备缓冲区 |
| 9 | `submittedDurationUs()` | 把累计 PCM 字节换算成微秒 |
| 10 | `QAudioOutput::processedUSecs()` | 取得设备已经处理的音频时长 |
| 11 | `refreshClock()` | 计算并发布三个原子时钟值 |
| 12 | `steadyClockUs()` | 取得单调时钟时间点 |
| 13 | `clockUs()` | 读取原子值、推算当前音频位置并限制上限 |

### 27.2 视频同步和显示

| 顺序 | 函数 | 功能 |
| ---: | --- | --- |
| 1 | `MyDemux::videoTimeBase()` | 取得视频 PTS 的单位 |
| 2 | `videoThread::run()` | 驱动视频解码循环 |
| 3 | `MyDecode::receiveFrame()` | 取得视频 `AVFrame` |
| 4 | `frameTimestampUs()` | 将视频显示时间戳换算成微秒 |
| 5 | `synchronizeFrame()` | 组织同步判断 |
| 6 | `audioThread::clockUs()` | 取得当前音频媒体位置 |
| 7 | `QThread::msleep()` | 视频提前时分段等待 |
| 8 | `SyncDecision::Drop` | 视频严重落后时跳过当前帧 |
| 9 | `copyYuv420pFrame()` | 显示前复制紧密排列的 YUV420P |
| 10 | `frameReady()` | 把帧排队发送到 GUI 线程 |
| 11 | `videoOpenGLWidget::setFrame()` | 保存视频数据并请求重绘 |
| 12 | `paintGL()` | 上传纹理并渲染画面 |

## 28. 完整数值例子

第一次有效音频帧：

```text
音频时间基 = {1, 48000}
音频 PTS   = 96000
```

换算：

```text
framePtsUs = 96000 × 1/48000 × 1000000
           = 2000000 微秒
           = 2.000 秒
```

假设此前没有提交 PCM：

```text
clockOriginUs = 2.000 秒
```

随后：

```text
QAudioOutput 已处理     = 120 ms
最近一次音频时钟        = 2.120 秒
距离采样又经过          = 8 ms
已提交 PCM 末尾         = 2.200 秒
```

`clockUs()` 返回：

```text
min(2.120 + 0.008, 2.200) = 2.128 秒
```

如果当前视频帧 PTS 是 2.180 秒：

```text
difference = 2.180 - 2.128 = +52 ms
```

视频提前，`synchronizeFrame()` 分段等待。

如果视频帧 PTS 是 2.070 秒：

```text
difference = 2.070 - 2.128 = -58 ms
```

假设迟到阈值为 40 ms，则当前视频帧返回 `Drop`。

## 29. 当前实现的边界

- `processedUSecs()` 的精度受 Qt 音频后端和系统设备影响。
- 单调时钟推算假设音频以 1.0 倍速度连续播放，暂停和倍速需要额外处理。
- 三个独立原子变量不是完全一致的事务快照，只满足当前毫秒级估算需求。
- GUI 线程过忙时，`frameReady()` 排队也可能造成画面延迟。
- 网络流或拼接媒体出现巨大 PTS 跳变时，尚未自动重建时钟。
- 当前队列按 packet 数量背压，没有按照总字节数限制。
- 当前重采样器结束时尚未单独排空 `SwrContext` 尾部样本。

## 30. 最需要记住的内容

```text
第一阶段：audioThread 产生音频时钟

音频 PTS
  → tryStartClock() 建立媒体时间原点
  → writePcm() 累计真正提交的 PCM
  → processedUSecs() 取得设备处理进度
  → refreshClock() 发布三个原子变量
  → clockUs() 加上单调时间并限制到 PCM 末尾

第二阶段：videoThread 使用音频时钟

视频 PTS
  → frameTimestampUs() 换算成微秒
  → clockUs() 取得音频媒体位置
  → difference = videoPts - audioClock
  → 正数等待、接近零显示、负值超过阈值丢帧

线程安全：

std::atomic 防止跨线程数据竞争
release 表示写线程完成发布
acquire 表示读线程接收发布
acquire/release 不会自动把多个原子变量变成一个事务
volatile 不能代替 atomic
```
