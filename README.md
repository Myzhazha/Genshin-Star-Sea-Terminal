# 原神主题桌面挂件（ESP32-P4 + 4.3" MIPI DSI）

一个基于 **ESP32-P4** 的桌面小挂件：4.3 寸 480×800 MIPI DSI 屏、触摸操作、原神主题 UI，
支持开机动画、图片/视频轮播、双机靠近联动（BLE）、WiFi、电量监测与时钟。

> 纯 **ESP-IDF** 工程（无 Arduino），ESP-IDF **v5.5.x** 验证通过。

---

## 功能特性

| 功能 | 说明 |
|---|---|
| **开机动画** | 循环播放 SD 卡 `/start/start.mjpeg`（P4 硬件 JPEG 解码） |
| **按键切页** | 短按按键：启动视频 → 图片 → 视频 → 设置 → 循环 |
| **图片浏览** | `/pic/*.jpg`，左右滑动切换 |
| **视频播放** | `/video/*.mjpeg`，左右滑动切换 |
| **设置页** | 原神主题 UI：UID、可见性、扫描频率、喇叭、振动、Wi-Fi、时钟、亮度、播放模式 |
| **双机联动** | 两台挂件 BLE 互相发现 → 弹窗显示对方 UID，随距离变近提示音/震动变频（2s → 0.5s），关闭后该 UID 静默 1 小时 |
| **电量显示** | INA226 实时电压/电流/功率，显示在设置页右上角 |
| **时钟** | DS1302 RTC，媒体页下滑显示、上滑隐藏 |
| **Wi-Fi** | 通过 ESP32-C6（esp-hosted SDIO）扫描/连接 |
| **设置持久化** | 全部设置存 NVS，重启保留 |

---

## 硬件要求

### 主控与显示
| 部件 | 型号 |
|---|---|
| 主控 | **ESP32-P4**（16MB Flash + 32MB PSRAM） |
| 屏幕 | **ST7102** 4.3" 480×800，MIPI DSI 2-lane，DSI PHY 由 LDO_VO3 供 2.5V |
| 触摸 | **ST7123** 电容触摸（I2C） |
| 无线 | **ESP32-C6** 模组（esp-hosted 从机，SDIO 4-bit @ 40MHz） |

### 外设与引脚

| 外设 | 引脚 |
|---|---|
| **SD 卡**（SDMMC 4-bit） | DET=45, CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42 |
| **SD 卡供电** | IO13（低电平导通，高电平关闭） |
| **ST7123 触摸**（I2C1） | SDA=26, SCL=25, RST=27, INT=28 |
| **屏幕 RST** | IO24 |
| **背光 PWM** | IO23（LEDC TIMER_0 / CH0） |
| **INA226**（I2C0，地址 0x40，10mΩ 分流，外置 4.7K 上拉） | SDA=6, SCL=5 |
| **DS1302 RTC** | CE=10, IO=11, SCLK=12 |
| **NS4168 功放**（I2S1） | CTRL=4（高电平使能）, BCLK=2, LRCLK=3, DIN=1 |
| **振动马达**（TC118S H 桥） | INA=47, INB=46（震：INA=低 + INB=高；停：都为低） |
| **模式按键** | IO9（按下为低） |
| **ESP32-C6 SDIO** | D0=14, D1=15, D2=16, D3=17, CLK=18, CMD=19 |
| **C6 复位** | IO54 → C6 EN / CHIP_PU |
| **C6 启动模式** | IO0 → C6 IO9（高=正常运行，低=下载模式） |

### 关键外围 IC
| IC | 作用 | 备注 |
|---|---|---|
| **SY7200A** | 背光升压 LED 驱动 | PWM 调光频率要求 **20kHz ~ 1MHz**（本工程用 20kHz）；首个脉冲 >200ns |
| **TC118S** | 振动马达 H 桥驱动 | 双 GPIO 控制方向，可 PWM 调速 |
| **NS4168** | I2S 数字功放 | CTRL 高电平使能 |
| **INA226** | 电压/电流/功率监测 | I2C 地址 0x40 |
| **DS1302** | 实时时钟 | 3 线接口 |

> ⚠️ **SDIO 信号线（CMD/D0~D3）建议加 10kΩ~47kΩ 上拉到 3.3V**，否则可能出现枚举不稳定。

---

## 软件架构

```
main/
├── main.c              # 入口：初始化顺序、周期任务
├── app_config.h        # 全部引脚与常量定义
├── hw_board.c          # LCD(DSI) / 触摸 / 背光 / LVGL 初始化
├── sdcard.c            # SD 卡挂载（含重试与 CMD0 复位）+ 媒体扫描
├── ina226.c            # INA226 电源监测驱动
├── ds1302.c            # DS1302 RTC 驱动
├── mjpeg_player.c      # P4 硬件 JPEG/MJPEG 解码播放
├── audio_mp3.c         # Helix MP3 解码 + I2S 输出（NS4168）
├── hosted_wifi.c       # esp-hosted WiFi（标准 esp_wifi API）
├── proximity_ble.c     # NimBLE 广播/扫描（esp-hosted VHCI）
├── c6_ota.c            # 通过 SDIO 给 C6 刷固件（OTA + ROM 下载模式）
├── app_media.c         # 媒体页 / 页切换 / 接近检测 / 电池显示
├── app_settings.c      # 设置事件对接外设 + NVS 持久化
├── lvgl_ui/            # LVGL 原神主题 UI
├── assets/fonts/       # 嵌入固件的中文字库（.bin）
└── assets/boot_text.rgb565  # 开机文字位图

components/
├── esp_lcd_st7102/         # ST7102 MIPI DSI 面板驱动
└── esp_lcd_touch_st7123/   # ST7123 触摸驱动（含 I2C 异常自动复位）
```

---

## 构建

### 环境
- **ESP-IDF v5.5.x**（`idf.py --version` 确认）
- Git + 网络（首次构建自动拉取组件依赖）

### ⚠️ 路径必须为纯英文
ESP-IDF 的 GCC 工具链**不支持中文/空格路径**（会报 `Illegal byte sequence`）。
请把工程放在例如 `E:\esp32p4_genshin_ui` 这样的纯英文路径下再构建。

### 编译
```bash
idf.py set-target esp32p4
idf.py build
```

首次构建会自动下载依赖组件：
`esp_lvgl_port`、`lvgl 9.4`、`esp_hosted`、`esp_wifi_remote`、`esp_lcd_touch`、
`esp-serial-flasher`、`esp-libhelix-mp3`，以及本地 `components/` 下的两个驱动。

> 构建产物约 **5.9MB**（含嵌入的中文字库 3.7MB），分区表已设为 **factory 12MB**。

### 一键脚本（Windows）
仓库根的 `build_idf.bat` 会：设置 IDF 环境 → 编译 → 把三个 bin 复制到 `固件\`
并按烧录地址命名。**请确保该 bat 为 GBK 编码 + CRLF 换行**（否则 cmd 解析异常）。

---

## 烧录

### 烧录地址（ESP32-P4，16MB Flash）
| 文件 | 地址 | 说明 |
|---|---|---|
| `bootloader.bin` | `0x2000` | 二级引导 |
| `partition-table.bin` | `0x8000` | 分区表 |
| `esp32p4_st7102_genshin_ui.bin` | `0x10000` | 主程序（factory，12MB） |

```bash
idf.py -p COMx flash monitor
```

或手动：
```bash
esptool --chip esp32p4 -p COMx -b 460800 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x2000  bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 esp32p4_st7102_genshin_ui.bin
```

---

## ESP32-C6 从机固件（WiFi / BLE 必需）

P4 本身没有无线，WiFi 与 BLE 全部由外挂 **C6** 提供（esp-hosted，SDIO 传输）。

### 1. 生成 C6 从机固件
```bash
# 用 esp-hosted 官方模板创建 slave 工程
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave
idf.py set-target esp32c6
# 保持传输层为 SDIO，并确认以下配置（否则 BLE 会 HCI 超时）：
#   CONFIG_BT_ENABLED=y
#   CONFIG_BT_CONTROLLER_ONLY=y
#   CONFIG_BT_LE_HCI_INTERFACE_USE_RAM=y
idf.py build
```

### 2. 首次烧录（空白 C6 必须用串口）
C6 需通过 **UART（GPIO16/17）+ USB-TTL** 首次刷入（空白芯片上电会自动进入下载模式；
若已有固件则需将 **GPIO9 拉低 + 复位** 进下载模式）：

```bash
esptool --chip esp32c6 -p COMx -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size detect \
  0x0     bootloader/bootloader.bin \
  0x8000  partition_table/partition-table.bin \
  0x10000 network_adapter.bin
```

### 3. 后续升级（P4 通过 SDIO 刷 C6）
把 `network_adapter.bin` 放到 SD 卡 **`/c6fw/network_adapter.bin`**，
在设置页点击"C6 固件更新"（或开机自动检测），P4 会通过 SDIO 完成 OTA。

> 若 C6 无响应（如 `sdio_card_fn_init failed`），一般是 C6 空白/损坏或 SDIO 接线问题。
> 工程内的 `c6_ota.c` 还支持让 P4 直接以 **ROM 下载模式** 刷写空白 C6
> （需 C6 的 IO9 接 P4 的 IO0，C6 EN 接 P4 IO54）。

### C6 ↔ P4 SDIO 接线
| 信号 | P4 | C6 |
|---|---|---|
| D0 | 14 | 对应 SDIO 数据脚 |
| D1 | 15 | |
| D2 | 16 | |
| D3 | 17 | |
| CLK | 18 | |
| CMD | 19 | |
| RESET | 54 | EN / CHIP_PU |
| BOOT | 0 | IO9 |

---

## SD 卡内容（FAT32）

```
/sdcard/
├── start/
│   └── start.mjpeg      # 开机动画（480×800 MJPEG，循环播放）
├── pic/
│   ├── 01.jpg           # 图片（.jpg/.jpeg，建议 480×800）
│   └── 02.jpg
├── video/
│   └── 1.mjpeg          # 视频（.mjpeg，480×800）
├── alert/
│   └── alert.mp3        # UID 弹窗提示音（建议 ≤1 秒，短提示音）
└── c6fw/
    └── network_adapter.bin   # 可选：C6 固件（P4 开机自动 OTA）
```

### 媒体格式要求
- **MJPEG**：每帧必须是**同分辨率**的标准 JPEG（480×800 最佳）；若混入不同尺寸或损坏帧，
  硬件解码器会跳过该帧（日志报 `decode failed`），但播放可继续。
- **提示音 MP3**：建议 **≤1 秒**。Helix 解码器对超长 MP3 的个别帧存在死循环风险
  （工程内已加"播放卡死 5 秒自动重启"的兜底保护）。

### 中文字体
字库已**嵌入固件**（`main/assets/fonts/*.bin`，通过 `EMBED_FILES` + `lv_binfont_create_from_buffer`
从 flash 直接加载），**不再需要放 SD 卡**，开机加载耗时从数十秒降到毫秒级。

如需更换字体，用 `lv_font_conv` 重新生成：
```bash
npx lv_font_conv --bpp 4 --size 16 --font 你的字体.ttf \
  -r 0x20-0x7F --range 0x4E00-0x9FFF --range 0x3000-0x303F --format bin \
  -o hywenhei_cjk_16.bin
```
> 修改字库后需同步 `main/CMakeLists.txt` 的 `EMBED_FILES`。

---

## 配置项（menuconfig → Genshin UI Hardware）

| 配置 | 默认 | 说明 |
|---|---|---|
| `GENSHIN_BACKLIGHT_PIN` | 23 | 背光 PWM 引脚 |
| `GENSHIN_BACKLIGHT_PWM_FREQ_HZ` | **20000** | 背光 PWM 频率（SY7200A 要求 20kHz~1MHz） |
| `GENSHIN_BACKLIGHT_DEFAULT_PERCENT` | — | 开机默认亮度 |
| `GENSHIN_BUTTON_PIN` | 9 | 模式按键 |
| `GENSHIN_VIBRATION_INA_PIN` | 47 | 马达 H 桥 INA |
| `GENSHIN_VIBRATION_INB_PIN` | 46 | 马达 H 桥 INB |
| `GENSHIN_SD_PWR_GPIO` | 13 | SD 卡供电控制（低电平导通） |

其它重要 sdkconfig：
- `CONFIG_LV_USE_FS_MEMFS=y` + `CONFIG_LV_FS_MEMFS_LETTER=65`：字体内存加载（必需）
- `CONFIG_SERIAL_FLASHER_PORT_SDIO=y`：P4 通过 SDIO 刷 C6（必需）
- `CONFIG_LVGL_PORT_ENABLE_PPA=n`：关闭 PPA（避免偶发渲染死锁）

---

## 使用说明

- **按键**：短按切换 启动视频 → 图片 → 视频 → 设置
- **滑动**：图片/视频页左右滑动切换；媒体页上下滑动显示/隐藏时钟
- **双机联动**：两台设备都开启"对附近设备可见"后靠近 →
  弹窗显示对方 UID + 提示音 + 震动，距离越近节奏越快（2s → 0.5s）、最近时连续震动；
  点"关闭"后该 UID 静默 1 小时
- **设置**：全部保存在 NVS（namespace `genshin`），重启保留

---

## 已知限制

1. **Helix MP3 解码器**对个别超长 MP3 帧存在死循环 → 提示音请使用 ≤1 秒的短音；
   工程已内置"解码卡死 5 秒自动重启"兜底。
2. **MJPEG 帧需同分辨率**，混入异常帧会丢帧（播放继续）。
3. **首次刷 C6 必须用串口/UART**（空白芯片无法通过 SDIO OTA 刷入）。
4. 触摸芯片偶发 I2C 异常时，驱动会在连续失败 3 次后自动复位 ST7123。
5. 工程路径**不能包含中文**（ESP-IDF 工具链限制）。

---

## 字体与版权说明 ⚠️

本工程嵌入的中文字库（`main/assets/fonts/*.bin`）由 **HYWenHei-85W**（汉仪文黑）转换而来。

> **HYWenHei 为商业字体**，其字形位图属于字体的衍生作品。
> **若用于开源发布，请务必替换为可商用/开源字体**（如 **思源黑体 Source Han Sans / Noto Sans CJK**，SIL OFL 许可），
> 或**不随仓库分发字体 bin**（让使用者自行生成）。
> 替换方法见上文"中文字体"章节。

其余代码如无特别说明，可按你选择的许可证发布（建议 MIT 或 Apache-2.0）。

---

## 致谢

- [ESP-IDF](https://github.com/espressif/esp-idf) / [LVGL](https://lvgl.io/)
- [esp-hosted](https://github.com/espressif/esp-hosted)（P4↔C6 SDIO 无线桥接）
- [esp-serial-flasher](https://github.com/espressif/esp-serial-flasher)（P4 直刷 C6）
- [esp-libhelix-mp3](https://github.com/chmorgan/esp-libhelix-mp3)（MP3 解码）
- [LVGL_UI 原神主题界面](https://github.com/)（作者原项目）
