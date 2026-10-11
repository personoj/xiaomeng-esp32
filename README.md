> 三个公开仓库的安装、职责和配对方式见 [INTEGRATION.md](INTEGRATION.md)。本快照的未验证修复见 [RELEASE_NOTES.md](RELEASE_NOTES.md)。云端优化任务见 [CLOUD_AGENT_PROMPT.md](CLOUD_AGENT_PROMPT.md)。

> 公开版先运行 `python scripts/export_public_avatar.py` 生成原创占位动画，再构建/烧录。原 `export_vector_avatar.py` / `export_avatar.py` 是保留的本地导出工具，依赖未发布的第三方引擎，不能用于干净克隆；公开版没有继承原角色的视觉验收结果。详见 [PUBLIC_ASSETS.md](PUBLIC_ASSETS.md)。

# 小萌 ESP32 终端

微雪 ESP32-S3-Touch-LCD-1.85C **V2 / Rev2.0** 专用。使用板载 ES8311、ES7210、360×360 ST77916 屏和硬件回声参考通道。

设备名称“小萌”，主色 `#D97527`。0.4.0 使用用户选择的按键对话：

1. 先说“你好小萌”，听到本地“我在”。
2. 按住板边的 **BOOT** 键说话，松开提交整句话；不按键不上传环境音。
3. 播报中按住 BOOT 会先停止旧播放，再收新一句话。短于 100ms 的输入不触发模型。
4. 退出时也按住说“拜拜”或“你可以退下了”，松开后由当前云端模型调用 `end_conversation`，本地播“好的，我先退下了”并回待机。没有固定退出词表。

BOOT 使用 GPIO0，RESET/RST 是复位键。声学前处理持续运行，但本地 VAD 不提交云端轮次。两段固定提示嵌入固件；初次连接尚未 ready 时最多缓存 8 秒按键音频，连接中断/待机丢弃旧录音。设备须搭配支持 `input_mode=push_to_talk` 的新版后端与 USB 桥接；网页仍可使用原自动轮次模式。该方案隔离松开期间的自身播报输入，不代表 AEC 已完全修复。

## USB 联调

先启动 `../xiaoyun-realtime/scripts/start.ps1`，再运行：

```powershell
.\scripts\start_usb.ps1
```

桥接绑定 audio；业务网页应绑定后端 `.local/device-pairing.json` 中的 session_id，使用 page 通道。桥接日志为 `.local/bridge.stdout.log`，不要启动同一 session 的浏览器音频工作台。USB 拔出后桥接等待重连；恢复时保持待机，不重放旧声音。BOOT 按键用于按住说话/松开提交；先用语音唤醒。停止桥接用 `scripts/stop_usb.ps1`。

## 手机热点

手机开启 2.4GHz/兼容模式，电脑接同一热点。在后端运行：

```powershell
.\scripts\start.ps1 -LanAddress <电脑热点IP> -Restart -EnablePrivateFirewall
```

防火墙仅放行 Private 网络和本地子网；如需管理员权限请在管理员 PowerShell 运行。不向校园公共网或公网开放普通浏览器接口。将 `network.example.json` 复制到 `.local/network.json`，在本地填写热点 SSID、密码和电脑 IP:8766。停止占用 COM9 的 USB 桥接后：

```powershell
& 'D:\conda\envs_dirs\codex\Scripts\python.exe' .\scripts\provision.py --mode wifi
```

配对 token 从电脑本地配置写入设备 NVS，不含模型 API Key。恢复 USB 模式使用 `--mode usb`。网络模式的实际验证状态见下文记录。

## 构建、烧录、恢复

```powershell
.\scripts\idf.ps1 build
.\scripts\flash.ps1
.\scripts\restore.ps1   # 仅在要恢复原固件时运行
```

IDF v6.1 使用专用 Python。依赖由 `dependencies.lock` 固定。0.3.x 屏幕资源由 `scripts/export_vector_avatar.py` 从旁边项目的原角色 JS 引擎采样真实 SVG 轮廓，需要 Python NumPy、Pillow、Playwright 和本机 Edge；输出 `.local/avatar-vector.bin`。六个状态/音量库各 384 帧，板子插值坐标并只绘制一套形状；不再混合两张完整截图。绘制使用整数音量，可由调度器分配到空闲核心；本地识别在 core 1，沿用当前组件的默认模型加载模式。LCD 在 GPIO18 的 TE 帧同步后，通过双内部 DMA 缓冲、保持片选的整帧传输刷新。原图形来源说明保留在 THIRD_PARTY_NOTICES.md。

从 0.2.0 更新时需同时烧录 app 和新的 XYVG 动画分区；`scripts/flash.ps1` 已采用新资源。旧 `export_avatar.py` / `.local/avatar.bin` 仅供旧版 RGB565 RLE 资源恢复。0.2.0 恢复副本为 `.local/xiaoyun-terminal-v0.2.bin`、`.local/avatar-raster-v0.2.bin`。

动画验证：`scripts/test_avatar.ps1` 编译与固件同一份 C 渲染器，检查 2544 帧及源 SVG 帧、状态衔接、音量变化和内存边界，并输出 `.local/animation-review/` 的多帧对比与 GIF。`device.animation` 日志给出状态切换前 12 帧的实际帧率、最大帧间隔、绘制/传输时间，不能用电脑端预览帧率代替该实机数据。

固定语音为 `main/prompts/*.pcm`，构建会直接嵌入程序，无需联网生成。`manifest.json` 记录文案、音色、采样率和校验值。只有主动运行 `scripts/generate_prompts.py` 才会使用后端已有配置调用云端录制，并生成 `.local/prompt-*.wav` 试听文件；该脚本不会访问电脑麦克风。

电脑端播放/状态模拟检查：`scripts/test_native.ps1`，使用已安装的 Visual Studio C++ 编译器运行实际 C 逻辑，将驱动和 DMA 时钟换成测试环境。它验证状态和样本记账，不能代替板子声学与实时性能测试。提示播放及其后 180ms 内不上传麦克风、不识别唤醒词，防止设备识别自己的提示音。

延迟日志：`device.prompt` 记录提示开始与结束；`device.playback_started.receive_to_play_ms` 记录设备收到首段音频到首个 DMA 播放回执；`bridge.first_audio_ack.delivery_ms` 记录首段 USB 发送到确认；`bridge.speech_to_playback.button_release_to_play_ms` 记录按键释放到实际播放。旧自动模式日志为 `local_vad_end_to_play_ms`，其起点是 VAD 边沿，不能与按键计时混为一谈。

串口一次只能由一个进程占用。原 Flash 已备份到 `.local/backups/`，该目录不得提交。所有实际通过项、问题和未完成验收见 [公开快照状态](RELEASE_NOTES.md)。

退出由当前云端模型调用设备专用 `end_conversation` 工具。没有固定结束词表；模型理解“拜拜”“今天先聊到这里”等结束意图后，后端发送 `device.standby(reason=spoken_exit)`，设备停止上传、播放本地确认并恢复唤醒。工具只在设备 audio 会话启用，浏览器和 page 工具沿用原协议。
