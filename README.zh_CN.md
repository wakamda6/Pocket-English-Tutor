<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 英语外教

用 FoloToy AI Passport 提问「这句话用英语怎么说」，或直接开启一段英文对话。老师可以根据场合建议措辞、示范语法纠正，再继续聊天。设备直连火山引擎豆包 Seeduplex。

## 配置设备

1. 按[构建和测试说明](docs/development/engineering/build-and-test.zh_CN.md)构建固件，并按[刷写与数据规则](docs/development/engineering/firmware-layout.zh_CN.md)刷写。
2. 首次启动时，用手机扫描设备屏幕上的二维码，加入免密码的 **Passport-XXXX** 热点。手机提示热点无法上网时，选择继续连接。
3. 连接后，从手机的 Wi-Fi 登录提示打开配网页。如果没有弹出，访问 `http://192.168.4.1`。网页打开后，Passport 会自动扫描附近的 2.4 GHz Wi-Fi。选中网络，手动输入密码和 Seeduplex API Key，再点击 **连接并保存**。隐藏网络可手动输入名称。
4. Passport 获取 Wi-Fi 地址后保存配置、关闭热点并自动重启。等屏幕显示模式选择界面。首次开始对话时才调用 Seeduplex 验证 Key。

结束会话后长按 **OK** 可重新开启热点。按 **OK** 退出配网，或在网页点击 **清除设备配置**。热点闲置五分钟会关闭。网页通过开放热点上的本地 HTTP 配置，设置保存在设备默认的未加密 NVS 分区，建议使用专用测试 Key。

开聊前短按上键选 **畅聊模式**，短按下键选 **教学模式**。畅聊全程使用英文，简短纠正语法、发音和拼写。教学主要用中文讲解，例句和发音示范用英文。长按上键加快语速，长按下键减慢语速。默认 1.0×，可在 0.7× 到 1.3× 间调节，重启后保留，两种模式都在开始新会话时应用该设置。

按一下确认键，等屏幕显示 **正在听** 后开始说话。设备会边录边上传。再按一下确认键提交这一轮，设备停麦后才会播放回复。看到 **轮到你了** 后，再按确认键继续同一段对话。长按确认键结束会话。回复音量默认 100%，会话中可用上下键调节。

## 试用

- 先问「这句话用英语怎么说？」，再换一个谈话对象，追问怎么说合适。
- 切到英文直接聊。可以说 `I go to a concert last night`，听老师是否会纠正过去式并接着聊。

[Seeduplex 接入文档](https://docs.volcengine.com/docs/doubao-voice/end-to-end-real-time-voice-full-duplex-version?lang=zh)

配网参考见 [Passport SoftAP 实践](docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.zh_CN.md)和[乐鑫 ESP-IDF 5.5.3 配网页示例](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/protocols/http_server/captive_portal)。
