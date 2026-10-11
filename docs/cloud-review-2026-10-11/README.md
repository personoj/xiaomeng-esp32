# ESP32云端主机证据

补充证据，不改变功能。实现/并发论证和复现命令见 [PLAYBACK_HOST_TESTS.md](../PLAYBACK_HOST_TESTS.md)。

[原始比较](playback-comparison.json)：5次交替before/after，每尺寸/版本45批、每批4000事件。96/960/3840输入样本的已知响应临界区进入数65/641/2561→2。主机CPU/event中位1.0825→0.9920、7.53825→6.79225、29.39175→25.43625微秒。驱动、锁和解码器为主机替身，这些不是ESP实时性或设备首音延迟。

旧版共同测试与新版扩展测试在ASan/UBSan下通过，计数器溢出旧版负例失败、新版通过。[最终sanitizer PASS摘要](sanitizer-pass-summary.txt)。LeakSanitizer因容器ptrace未测。

实际DMA信用、24→16kHz转换和取消/尾样本约束保留。单次发布会让整个chunk在转换后可见，真实首样本/欠载、双核/PSRAM内存序仍需本地审查。未编译IDF固件、未接设备、未烧录，也没有声学/模型端到端验收。

JSON仅替换绝对工作区前缀，数据不变；无私人录音、真实会话、密钥或Library私有链接。基线及已测本地HEAD见 [test-summary.json](test-summary.json)。只建draft PR，不合并。
