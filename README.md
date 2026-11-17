# PixelGrove

**ESP32-S3 圆形屏徽章的像素风天气与农场应用**

> ⚠️ **本项目是基于 [nbtca/CABadge](https://github.com/nbtca/CABadge) 的二次开发（fork）**
>
> 底层固件框架、屏幕驱动、WiFi/BLE 服务、LVGL 集成、构建系统均来自上游 CABadge 项目。
> 本项目在其基础上**新增了像素天气应用、像素农场应用**，并对应用框架做了精简与改造。
> 上游项目版权归 nbtca 及其贡献者所有，详见 [LICENSE](LICENSE) 与文末「致谢」。

---

## 这个项目做了什么

在 CABadge 固件上新增两个完整应用，并接入真实网络天气数据：

### 🌤 像素天气（本项目核心）

一个真正联网的实时天气应用，**不是播放预先渲染好的动画**——背景、天气图标、树木、雨雪特效分别独立绘制，每层都由接口数据驱动。

| 特性 | 实现 |
|---|---|
| **真实天气数据** | 和风天气（QWeather）`/v7/weather/now` + `/v7/weather/3d`，HTTPS |
| **三页触摸 UI** | 左右滑动切换：实况 / 未来三天 / 详细参数 |
| **四季背景** | 按查询城市**当地月份**自动切换春/夏/秋/冬，白天夜晚各一套 |
| **四季树木动画** | 8 组 × 4 帧，64×96 透明图层，450ms/帧；白天夜晚成对切换 |
| **天气特效** | 雨/雪覆盖层，A8 透明通道 + 运行时染色 |
| **阴天处理** | 当季背景叠加灰蓝滤镜（`image_recolor`），保留季节感与动画树 |
| **昼夜自动判断** | 用接口返回的日出日落时间，不依赖设备时区 |
| **失败可诊断** | 屏幕直接显示「请求失败/解压失败/解析失败/接口xxx/数据缺失」 |
| **刷新策略** | 自动 20 分钟；手动节流 60 秒；失败指数退避（40s → 1200s） |
| **离线降级** | 保留上次成功数据并标注更新时间；无数据显示占位符，不把 0°C 当实况 |

### 🌱 像素农场

触摸种植/浇水/收获的像素风小游戏，自带昼夜与作物生长阶段。

### 🕐 TimeTiles

数字时钟瓷砖界面（继承自上游）。

---

## 硬件要求

| 项 | 规格 |
|---|---|
| 开发板 | DFRobot **DFR1221**（ESP32-S3 1.85" 圆形屏） |
| 主控 | ESP32-S3R8（16 MB Flash / 8 MB Octal PSRAM） |
| 屏幕 | **ST77916**，QSPI @ 40 MHz，**360×360** |
| 触摸 | 电容触摸，坐标范围 360×360，无独立 RST/INT |
| 连接 | USB-CDC 串口 |

> 换板子需要改 `firmware/device/src/physical_display.c` 里的 QSPI 引脚与屏参。

---

## 快速开始

### 1. 准备配置（含你自己的 API 密钥）

```bash
cd firmware/ui
cp weather_config.h.example weather_config.h
```

编辑 `weather_config.h`，填入你在 [和风天气控制台](https://console.qweather.com/) 申请的：

```c
#define WX_API_HOST   "your-host.qweatherapi.com"   // 控制台 → 设置 → API Host
#define WX_API_KEY    "YOUR_API_KEY_HERE"           // 控制台 → 设置 → API KEY
#define WX_LOCATION_ID "101210101"                  // 杭州；换城市见 docs/03
#define WX_CITY_LABEL  "杭州"
```

> `weather_config.h` 已被 `.gitignore` 忽略，**不会**被提交。

### 2. 编译

```powershell
# 需要 ESP-IDF 6.1 环境（PlatformIO framework-espidf@4.60100.0）
pwsh -File firmware/build.ps1
```

首次编译约 **25–35 分钟**（含 LVGL 全量编译）。

### 3. 烧录

```bash
python -m esptool --chip esp32s3 --port COM6 --baud 460800 \
  --before default_reset --after hard_reset write-flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

详细步骤、环境依赖、常见错误见 **[docs/02-构建与烧录.md](docs/02-构建与烧录.md)**。

---

## 目录结构

```
PixelGrove/
├── firmware/
│   ├── ui/                 二开核心源码（LVGL 界面层）
│   │   ├── weather.c/.h           像素天气主逻辑（约 900 行）
│   │   ├── weather_config.h.example   API 配置模板（复制成 .h 使用）
│   │   ├── pixel_farm.c/.h        像素农场
│   │   ├── apps.c/.h              应用列表与启动调度
│   │   ├── badge_ui.c/.h          主界面与手势
│   │   └── *_assets.c             素材数组（由 tools/ 脚本生成）
│   ├── tools/              素材与字模生成脚本
│   │   ├── generate_weather_assets.py   天气素材 → C 数组
│   │   ├── generate_assets.py           中文字模子集
│   │   └── generate_pixel_farm_assets.py
│   ├── device/             ESP-IDF 工程（CMakeLists / 分区表 / 驱动）
│   └── build.ps1           构建脚本
├── assets/                 全部素材 PNG（375 张，33 MB）
└── docs/                   规格与实现文档
```

> `vendor/lvgl-9.4.0`（168 MB）未包含在仓库中，构建脚本会自动引用；
> LVGL 版本固定为 **9.4.0**。

---

## 文档

| 文档 | 内容 |
|---|---|
| [01-规格说明](docs/01-规格说明.md) | 硬件规格、功能规格、Flash/RAM 预算、分区表 |
| [02-构建与烧录](docs/02-构建与烧录.md) | 环境搭建、编译流程、烧录、回滚、常见错误 |
| [03-天气接口](docs/03-天气接口.md) | 和风天气 API、gzip 坑、城市 ID 查询、字段映射 |
| [04-素材规格](docs/04-素材规格.md) | 素材分类、尺寸、颜色格式、在固件中的用途 |
| [05-踩坑记录](docs/05-踩坑记录.md) | 字体溢出、ARGB8565 不可用、静态树重叠等实战问题 |
| [assets/README](assets/README.md) | 375 张素材的逐类说明 |

---

## Flash 空间现状

应用分区 **7 MB**，当前占用 **91.5%**：

```
Flash: [========= ]  91.5% (used 6,714,099 bytes from 7,340,032 bytes)
RAM:   [=         ]  15.0% (used 49,056 bytes from 327,680 bytes)
```

**只剩 611 KiB**，新增素材前必须先删等量内容。这也是为什么：

- 雨雪特效只保留 2 帧（而非素材的 4 帧）
- 树木动画用 64×96 而非 128×192（后者要 1.13 MiB）
- 开屏名片页（`font56` + logo 共 922 KiB）已移除
- 阴天独立背景图（253 KiB，且自带静态树）已移除

---

## 致谢

本项目站在这些项目的肩膀上：

- **[nbtca/CABadge](https://github.com/nbtca/CABadge)** —— 上游固件框架（本项目的基础）
- **[LVGL 9.4.0](https://github.com/lvgl/lvgl)** —— 嵌入式图形库
- **[和风天气 QWeather](https://www.qweather.com/)** —— 天气数据接口（需遵守其署名与调用规范）
- **[ESP-IDF 6.1](https://github.com/espressif/esp-idf)** —— 官方 SDK

素材为像素风格的自制/整理素材，未使用第三方版权图。

---

## 许可

[MIT](LICENSE) —— 与上游 CABadge 保持一致。

二次开发部分（`weather.c`、`pixel_farm.c`、素材生成脚本等）同样以 MIT 发布。
