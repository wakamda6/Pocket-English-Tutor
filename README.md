<p align="right"><strong>English</strong> · <a href="README.zh_CN.md">简体中文</a></p>

# English Tutor

Use your FoloToy AI Passport to ask how to say something in English or to
have an English conversation. The tutor can suggest wording for the situation,
model a grammar correction, and continue the conversation. The Passport
connects directly to Doubao Seeduplex on Volcengine.

## Set up the device

1. Build the firmware using [Build and Test](docs/development/engineering/build-and-test.md)
   and flash it using the applicable [data policy](docs/development/engineering/firmware-layout.md#flashing-and-stored-data).
2. Scan the QR code on the Passport to join its open **Passport-XXXX** hotspot.
   Stay on the hotspot if your phone says it has no internet access.
3. Open the setup page from the phone's Wi-Fi sign-in prompt. If the prompt
   does not appear, browse to `http://192.168.4.1`. Passport scans nearby
   2.4 GHz networks when the page opens. Select one, enter its password and
   the Seeduplex API key, then select **连接并保存**. You can type a hidden network's
   name manually.
4. Passport saves the form after getting a Wi-Fi address, closes the hotspot,
   and restarts. Wait for the mode selection screen. The Seeduplex key is first used
   when you start a conversation.

Hold **OK** outside a conversation to reopen hotspot setup. Press **OK** to
leave setup, or select **清除设备配置** on the page to forget the saved settings.
An unused hotspot closes after five minutes. Setup uses local HTTP over the
open hotspot. This prototype stores the
configuration without encryption in the device's default NVS partition.
Use a dedicated test key.

Before starting, press **Up** for the English chat mode or **Down** for the teaching mode.
English Chat uses English throughout and gives brief grammar, pronunciation,
and spelling feedback in English. Teaching explains in Mandarin and uses
English for examples and pronunciation demonstrations. Before starting, hold
**Up** to speak faster or **Down** to speak slower. Speed is saved across
restarts and applies to new conversations in either mode. The default is
1.0×, and the available settings run from 0.7× to 1.3×.

Press **OK** once and wait for **正在听** (listening) before speaking. The device
streams audio as you speak. Press **OK** again to submit the turn and allow
the reply to play. Recording stays off during playback. When the screen
shows **轮到你了** (your turn), press **OK** to continue the same conversation. Hold
**OK** to end it. Reply volume starts at 100%. Press **Up** or **Down** to adjust it
during a conversation.

## Try it

- Ask `“这句话用英语怎么说？”`, then change the audience and ask how to phrase it.
- Switch to English and start talking. Try `I go to a concert last night` to
  check whether the tutor corrects the past tense and keeps chatting.

[Seeduplex API documentation](https://docs.volcengine.com/docs/doubao-voice/end-to-end-real-time-voice-full-duplex-version?lang=zh)

Provisioning references. [Passport SoftAP notes](docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md) ·
[ESP-IDF 5.5.3 captive portal example](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/protocols/http_server/captive_portal)
