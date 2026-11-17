# firmware/

固件源码。**基于 [nbtca/CABadge](https://github.com/nbtca/CABadge) 二次开发**。

---

## 目录说明

```
firmware/
├── ui/                    LVGL 界面层（二开主要战场）
├── tools/                 素材与字模生成脚本
├── device/                ESP-IDF 工程（CMake / 分区表 / 驱动）
└── build.ps1              构建脚本
```

---

## ui/ —— 哪些是本项目新增的

| 文件 | 来源 | 说明 |
|---|---|---|
| `weather.c` / `weather.h` | 🆕 **本项目新增** | 像素天气应用全部逻辑（约 900 行） |
| `weather_config.h.example` | 🆕 **本项目新增** | API 配置模板，复制成 `weather_config.h` 使用 |
| `weather_assets.c` | 🆕 生成产物 | 天气素材 C 数组（8.5 MB，脚本生成） |
| `pixel_farm.c` / `pixel_farm.h` | 🆕 **本项目新增** | 像素农场应用 |
| `pixel_farm_assets.c` | 🆕 生成产物 | 农场素材 C 数组（1.2 MB） |
| `apps.c` / `apps.h` | ✏️ **改造** | 应用列表精简为 3 个应用；含已删应用的 stub |
| `badge_ui.c` / `badge_ui.h` | ✏️ **改造** | 移除开屏名片页（省 922 KiB） |
| `assets.c` | ✏️ 重新生成 | 中文字模（438 字形 × f14/f18/f24/f36） |
| `timetiles/` | 上游保留 | 时钟瓷砖应用 |
| 其余 | 上游原样 | 侧边栏、WiFi 面板、过渡动画等 |

### 依赖 ui/ 的上游系统文件

修改 `ui/` 时注意这些**上游文件**也在引用：

| 文件 | 依赖内容 |
|---|---|
| `device/src/main.c` | USB 命令分发，调用 `fluidbox_mode/reset` |
| `device/src/management.c` | 状态 JSON 上报，调用 `fluidbox_info` |
| `device/src/motion_service.c` | 姿态转发，调用 `fluidbox_motion` |
| `device/src/app_service.c` | `badge_apps_clock_sync()`、`lodepng_*` PSRAM 分配 |

> 已删除的 FluidBox 应用在这几处仍有调用，`ui/apps.c` 底部保留了空 stub 维持接口。

---

## tools/ —— 素材生成

| 脚本 | 作用 | 输出 |
|---|---|---|
| `generate_weather_assets.py` | 背景/树/图标/雨雪 PNG → C 数组 | `ui/weather_assets.c` |
| `generate_assets.py` | 扫描源码中的中文 → 字模子集 | `ui/assets.c` |
| `generate_pixel_farm_assets.py` | 农场素材 → C 数组 | `ui/pixel_farm_assets.c` |
| `generate_app_assets.py` | 应用图标素材 | `ui/app_assets.c` |

**素材源路径**在脚本顶部常量里，需指向仓库的 `assets/` 目录或本地素材路径。

### 字体子集的两个关键约束

1. **扫描列表是硬编码的** —— 新增含中文的 `.c` 文件必须加进列表，否则中文显示成框框
2. **f36 以上不要加太多字** —— `lv_font_fmt_txt_glyph_dsc_t.offset` 只有 20 位（上限 1,048,575），f56 加字必溢出

详见 [../docs/05-踩坑记录.md](../docs/05-踩坑记录.md) §1、§2。

---

## device/ —— ESP-IDF 工程

| 文件 | 说明 |
|---|---|
| `CMakeLists.txt` | 组件依赖（`espressif__cjson`、`espressif__zlib`、`esp_lcd_st77916` 等） |
| `partitions.csv` | 分区表（factory 7 MB） |
| `platformio.ini` | PlatformIO 配置（ESP32-S3，16 MB Flash） |
| `sdkconfig.defaults` | SDK 默认配置 |
| `src/` | 驱动、协议、主循环 |
| `src/idf_component.yml` | 托管组件清单 |

### LVGL 不在仓库里

`vendor/lvgl-9.4.0` 有 **168 MB**，未纳入版本控制。

构建脚本会从以下位置引用：

```
<repo>/firmware/vendor/lvgl-9.4.0
```

需要自行放置 LVGL **9.4.0** 源码，或修改 `build.ps1` 里的 `vendor\lvgl-9.4.0` 路径。

```bash
git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git vendor/lvgl-9.4.0
```

---

## build.ps1

构建入口。关键行为：

1. 在 `F:\CABadgeBuild\staging\<checkoutId>\<config>\build-<buildId>\` 创建**全新 staging 目录**
2. 从本仓库**显式复制**文件列表到 staging（不是 glob！`ui/` 根目录的文件必须逐个列出）
3. 复制 LVGL、device 源码、分区表
4. 调用 PlatformIO 编译
5. 产物输出到 `outputs/builds/<checkoutId>/...`

**新增 `ui/` 下的文件必须加进复制列表**，否则编译报找不到头文件。

详见 [../docs/02-构建与烧录.md](../docs/02-构建与烧录.md)。
