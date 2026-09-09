# 工程内置 FFmpeg

此目录保存 MyPlay2 构建和运行所需的 FFmpeg shared development package 子集，避免在 qmake 配置中写入开发电脑的绝对路径。

## 当前版本

- FFmpeg：`N-126390-g9fc8c785e2-20260903`
- 架构：Windows x86-64
- 类型：LGPL shared build
- 复制来源：`ffmpeg-master-latest-win64-lgpl-shared`

完整许可证文本见 `LICENSE.txt`。

## 目录内容

- `include/`：FFmpeg 公共头文件。
- `lib/`：MinGW 导入库及包中附带的 MSVC 导入库。
- `bin/`：MyPlay2 实际链接的 4 个运行时 DLL。

为控制仓库体积，没有复制 FFmpeg 命令行工具，也没有复制当前播放器未使用的 `avfilter`、`avdevice` 和 `swscale` DLL。

## 更新方法

升级 FFmpeg 时必须同时替换 `include/`、`lib/` 和 `bin/`，不要混用不同版本。若 DLL 主版本号变化，还要同步修改 `MyPlay2.pro` 中的 `FFMPEG_RUNTIME_DLLS` 文件名，然后重新运行 qmake 并完成一次干净构建。

`bin/*.dll` 使用 Git LFS 保存。首次克隆后如果 DLL 只是文本指针，请先执行：

```powershell
git lfs pull
```
