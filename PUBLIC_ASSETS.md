# 公开版屏幕资源

固件的XYVG加载、几何插值和显示代码原样保留。原私人角色不可再分发，因此没有提交其JS引擎、帧数组、屏幕截图或Flash备份。公开版可先运行：

```sh
python scripts/export_public_avatar.py
```

它使用Python标准库生成抽象圆形音频指示器，写入`.local/avatar-vector.bin`，与现有`flash.ps1`使用的分区/路径一致。没有调用模型或接触设备，不使用参考角色轮廓。公开占位动画尚未做实机视觉验收；不能把历史原角色验收结果用于它。

`scripts/export_vector_avatar.py` / `export_avatar.py`保留为原本地集成代码示例，依赖不在公开仓库内的`../xiaoyun-realtime/web/vendor/xiaoyun-engine.js`。不要为运行这些脚本把受限素材提交到公开仓库。`tests/check_avatar.py`的源角色截图比较部分同样需要私人夹具；协议、按键、播放测试不依赖它。

构建使用ESP-IDF 6.1和`dependencies.lock`。安装并激活自己的ESP-IDF环境后，标准命令`idf.py build`即可构建；原开发机`idf.ps1`包含工具链安装路径后备。不要在云端运行烧录、恢复或配网命令。
