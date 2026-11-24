# ESP32 + E5573S 短信转发器

ESP32 通过 2.4 GHz Wi-Fi 连接华为 E5573S，使用 HiLink 接口读取收件箱，再把未读短信转发给企业微信群机器人。ESP32 与热点之间只需 Wi-Fi。HiLink 协议参考 [e5573s-hilink-sms](https://github.com/kingmacth/e5573s-hilink-sms)。

## 接线与首次烧录

固件面向 ESP32-WROOM-32E-N4（4 MB Flash），使用 PlatformIO 的 `esp32dev` 配置。ESP32 使用稳定的 3.3 V 电源，转串口使用 3.3 V TTL 电平。

| ESP32 信号 | 接法 |
| --- | --- |
| TX0（GPIO1）和 RX0（GPIO3） | 分别接 USB 转串口的 RX 和 TX，两边 GND 共地 |
| BOOT（GPIO0） | 进入 ROM 下载模式时拉低 |
| EN | 短暂拉低后释放以复位 |
| GPIO26 | 状态 LED，高电平点亮，串联限流电阻 |
| GPIO27 | 可选常开按键接 GND，使用内部上拉 |

1. 接好串口，确认电脑分配的端口号。下文以 Windows 的 `COM5` 为例，替换成实际端口。关闭占用该端口的串口监视器。
2. 按住 BOOT，短按 EN 后释放 EN，再松开 BOOT。没有 EN 按键时，可以按住 BOOT 重新上电。芯片由此进入 ROM 下载模式。
3. 在仓库目录运行以下命令。PlatformIO 会编译并写入引导程序、分区表和应用。

```powershell
python -m pip install platformio pyserial
python -m platformio device list
python -m platformio run
python -m platformio run --target upload --upload-port COM5
```

看到写入校验成功后，释放 BOOT 并按一下 EN，或重新上电。以下命令打开串口监视器，设置为 **115200、8N1**，发送时以换行结束。

```powershell
python -m platformio device monitor --port COM5 --baud 115200
```

如果使用其他烧录工具，先在编译后运行 `python tools/package_firmware.py`。把生成的 **`dist/factory.bin` 写到地址 `0x0`**，Flash 选项为 ESP32、4 MB、DIO、40 MHz。整合镜像不要传给后续的串口更新脚本。

## 手机配网与测试

1. 手机连接 ESP32 的配网热点 **`E5573S-SMS-Setup`**，密码为 **`E5573sSetup`**。手机提示该网络没有互联网时，选择继续连接。
2. 浏览器打开 `http://192.168.4.1/`，点击“扫描附近 Wi-Fi”，选择 E5573S 的 **2.4 GHz** 热点并填写 Wi-Fi 密码。隐藏网络可以手动输入名称。
3. 填写 E5573S 管理地址（默认 `192.168.8.1`）、管理用户名（通常为 `admin`）和**管理页面登录密码**。请在 E5573S 的管理页面确认该密码，Wi-Fi 连接密码可能不同。
4. 在企业微信群中添加机器人，把它的 Webhook 地址填入配置页。公开版首次使用须填写自己的地址。默认消息前缀为 `【新短信】`，查询间隔为 **10 秒**，可设为 **2～3600 秒**。
5. 保存后 ESP32 会重启。发送串口命令 `IP` 查看它获得的局域网地址，`STATUS` 查看运行状态，`AUTH_DIAG` 查看登录状态。使用 `TEST` 测试机器人连接，使用 `SMS_TEST_ONE` 测试真实短信转发。

网页的密码和 Webhook 框留空时保留原值。配置页可通过 ESP32 的局域网 IP 访问。短按 GPIO27 或发送 `AP` 可重新开启配网热点。长按 GPIO27 **8 秒**会恢复出厂配置并清除转发记录，E5573S 上仍未读的短信会在重新配置后进入转发流程。

## 密码输错后热点锁定

某些 E5573S 固件连续输错 **两次管理密码**就会锁定登录。串口若显示 `108007`，刚改成正确密码仍可能收到相同错误。按实机经验，恢复步骤如下。

1. 向 ESP32 发送 `AUTH_PAUSE`，暂停自动登录和转发。该设置重启后仍有效。
2. **彻底关闭 E5573S，断电至少 10 秒后再开机。** 插着 USB 或电池还在供电时，拔掉一条线可能没有真正断电。只重启 ESP32 无法解除热点的登录限制。
3. 手机连回 E5573S，打开 `http://192.168.8.1/`，确认能用管理密码登录。若需要修改 ESP32 的配置，保存正确的管理密码。
4. 等 ESP32 重新拿到 IP 后，发送 `AUTH_DIAG` 查看剩余等待时间。退避结束后发送 `AUTH_RESUME`，再用 `STATUS` 或 `AUTH_DIAG` 确认已登录。

登录失败后，固件按 **5、10、20、30 分钟**逐步延长重试间隔。成功登录后清零失败计数。退避期间，手动查询、单条测试和恢复命令均遵守当前等待时间。当前实现支持 HiLink 的传统登录方式，包括 `password_type=4`。

## 串口命令

命令区分大小写。可在监视器中逐行发送，也可使用脚本，例如 `python tools/serial_debug.py COM5 IP STATUS AUTH_DIAG --seconds 5`。

| 命令 | 用途 |
| --- | --- |
| `HELP` | 查看全部命令。 |
| `IP` | 查看 Wi-Fi IP、配网 IP、网关和信号强度。 |
| `STATUS` | 查看网络、轮询间隔、存储、最近检查状态及本次开机推送统计。 |
| `AUTH_DIAG` | 查看最近登录状态、认证类型、是否暂停和退避剩余时间。 |
| `AUTH_PAUSE` | 暂停自动登录和转发，保存到设备配置。已开始的请求可能完成。 |
| `AUTH_RESUME` | 恢复自动登录和转发，保留当前退避时间。 |
| `AUTH_SELFTEST` | 本地验证 XML 取值、示例密码哈希及退避计算。 |
| `POLL` | 立即请求一轮常规检查。 |
| `TEST` | 向已配置的企业微信群机器人发送一条测试文本。 |
| `SMS_TEST_ONE` | 从热点读一条真实短信并推送。有未读时优先选未读，否则选择最新的已读短信。已读短信只在本次测试发送。 |
| `AP` | 开启配网热点。 |
| `REBOOT` | 等待当前工作退出后重启 ESP32。 |
| `UPDATE` / `BOOTLOADER` | 让运行中的固件等待串口更新脚本。60 秒无数据会退出。 |

## 后续固件更新

首次烧录完成后，只要现有固件能正常运行，就可通过 UART0 更新应用。关闭串口监视器，按以下顺序操作。脚本会自行发送 `UPDATE`，无需手动按 BOOT。

```powershell
python -m platformio run
python tools/serial_update.py COM5 .pio/build/esp32dev/firmware.bin
python tools/serial_debug.py COM5 IP STATUS --seconds 5
```

也可以将打包后的 **`dist/firmware.bin`** 作为脚本的第二个参数。脚本通过 115200 串口分块发送应用镜像，设备写入备用 OTA 分区，校验成功后切换并重启，保留配网配置和 LittleFS 转发队列。看到“固件校验通过”后再打开监视器。更新中断时可以重试。若现有程序无法启动或回应更新命令，按首次烧录的 BOOT/EN 步骤进入 ROM 下载模式。

## 转发行为与常见问题

- 每轮检查结束后等待配置的轮询间隔，实际间隔还包含网络请求耗时。自动流程只取未读短信，企业微信返回 HTTP 200 且 `errcode=0` 后保存成功记录，再将短信标为已读。发送失败会保留待推送状态。极端断电或响应丢失可能造成重复推送。
- 企业微信 HTTPS 依赖 NTP 校时和证书验证。若提示等待校时，检查 E5573S 是否能上网。若出现 `Brownout detector was triggered`，检查 ESP32 的 3.3 V 供电、接线压降和去耦。
- 默认 Wi-Fi 节能模式保持连接。可选轻度休眠会在空闲时关闭 Wi-Fi、定时唤醒重连，期间可用 GPIO27 或串口唤醒。用串口唤醒时，先发几次空行再发命令。
- GPIO26 每 1.6 秒双闪表示配网热点已开启，快闪表示正在连接，每 2 秒短闪表示 Wi-Fi 已连接，休眠时熄灭。
- Webhook 通过网页配置。也可把 `include/secrets.example.h` 复制为本地 `include/secrets.h`，指定自己的编译默认值。该文件和本地固件产物由 Git 忽略。若将密钥编译进固件，请妥善保管该镜像。

运行 `python -m unittest discover -s tests -v` 可验证串口上传脚本。
