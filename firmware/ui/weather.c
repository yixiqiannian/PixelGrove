/* 像素天气小屋：实时天气展示（和风天气 v7 API）。
 *
 * 分层结构与像素农场一致：**素材是静态 const 图，绝不预合成**。背景、图标、
 * 雨雪特效、全部文字分别在 LVGL 对象上绘制，这样温度/城市/日期都能实时更新。
 *
 * 线程模型：HTTP 在独立 task 里跑，绝不阻塞 LVGL；结果经 FreeRTOS 队列回 GUI。
 * 退出时置 abort 标志丢弃在途结果，task 常驻不反复创建。
 */
#include "weather.h"
#include "apps.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "wallpaper_service.h"
#include "zlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* ================= 布局（360x360 圆屏，圆心 180,180 半径 180） =================
 * 可用弦宽：y=42→x∈[63,297]，y=284→x∈[33,327]，y=316→x∈[62,298]
 */
#define WX_PAGE_COUNT 3
enum { WX_PAGE_MAIN, WX_PAGE_FORECAST, WX_PAGE_DETAIL };

#define BG_W 360
#define BG_H 360
/* 树木图层：素材指定的 64×96 小树左上方位置，与 128×192 大树共用同一个根部锚点 */
#define WX_TREE_X 257
#define WX_TREE_Y 207

/* ================= 数据模型 ================= */
typedef struct {
    int32_t temp;          /* 实况温度 */
    int32_t feels;         /* 体感 */
    int32_t humidity;      /* % */
    int32_t pressure;      /* hPa */
    int32_t vis;           /* km */
    int32_t wind_scale;    /* 风力等级 */
    int32_t icon;          /* 和风 icon code，未识别为 -1 */
    char wind[10];         /* 风向（接口 lang=en 时返回 E/NE/...） */
    int64_t obs_wall;      /* 观测时间 UTC 秒，0 = 未知 */
} wx_now_t;

typedef struct {
    int32_t tmax, tmin;
    int32_t icon_day;
    int32_t sunrise, sunset;   /* 当地当天分钟数，-1 = 未知 */
} wx_day_t;

typedef struct {
    uint32_t serial;
    bool ok;
    bool have_now, have_daily;
    char err[20];          /* 失败原因，显示在屏幕上便于排查 */
    wx_now_t now;
    wx_day_t day[3];
} wx_net_t;

/* GUI 侧持有的最近一次成功数据 */
static wx_net_t data;
static bool have_data;
static int64_t next_refresh_wall;   /* 下次自动刷新（UTC 秒） */
static int64_t last_manual_wall;    /* 上次手动刷新（UTC 秒） */
static int64_t last_fail_wall;      /* 上次失败（UTC 秒） */
static bool failed;                 /* 最近一次尝试失败 */
static bool offline;                /* 当前无 WiFi */
static char last_err[20];           /* 最近一次失败原因 */
static uint32_t request_serial;

/* ================= 线程共享 ================= */
static QueueHandle_t wx_results;
static TaskHandle_t wx_task;
static atomic_bool wx_abort;
static atomic_bool wx_busy;

/* ================= UI 状态 ================= */
static lv_obj_t *wx_root;
static lv_obj_t *wx_panel;          /* 应用面板，创建 wx_root 用 */
static lv_obj_t *wx_page[WX_PAGE_COUNT];
static lv_obj_t *wx_dot[WX_PAGE_COUNT];
static lv_obj_t *wx_bg_img;
static lv_obj_t *wx_tree_img;       /* 四季树木透明图层 */
static int wx_tree_set;             /* 0-3 白天四季 / 4-7 夜晚四季 */
static uint8_t wx_tree_frame;
static uint32_t wx_tree_frame_at;
static lv_obj_t *wx_fx;             /* 雨/雪覆盖层 */
static lv_obj_t *wx_icon_img;
static lv_obj_t *wx_city_lbl;
static lv_obj_t *wx_temp_lbl;
static lv_obj_t *wx_word_lbl;
static lv_obj_t *wx_highlow_lbl;
static lv_obj_t *wx_time_lbl;
static lv_obj_t *wx_date_lbl;
static lv_obj_t *wx_uptodate_lbl;
static lv_obj_t *wx_status_lbl;     /* 更新失败 / 离线 / 加载中 */
static lv_obj_t *wx_fc_row[3][4];   /* 日期 / 图标 / 高 / 低 */
static lv_obj_t *wx_detail_val[5];
static void (*wx_back)(void);
static int wx_page_idx;
static uint32_t wx_fx_until, wx_fx_frame;
static bool wx_showing;

/* ================= 时间 ================= */
static int64_t wall_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

/* 设备时区不可信：一律用配置里的地点偏移换算当地时间 */
static void local_broken(int64_t wall, struct tm *out)
{
    time_t t = (time_t)(wall + WX_TZ_OFFSET_SEC);
    gmtime_r(&t, out);
}

static bool wall_valid(int64_t w) { return w > INT64_C(1700000000); }

/* WiFi 是否就绪：工程既有的 IP 查询，比自己去翻 badge_state 更内聚 */
static bool net_online(void)
{
    char ip[24];
    wifi_wallpaper_address(ip, sizeof(ip));
    return ip[0] != 0;
}

/* ================= 天气映射 ================= */
/* 和风 icon code：100/150 晴，101-103 多云，104/154 阴，300-399 雨，
 * 400-499 雪，500-515 雾霾沙尘。未识别统一回退到「阴」。 */
static int icon_kind(int32_t code)
{
    if(code >= 400 && code < 500) return WX_ICON_SNOW;
    if(code >= 300 && code < 400) return WX_ICON_RAIN;
    if(code == 100 || code == 150) return WX_ICON_SUN;
    if((code >= 101 && code <= 103) || (code >= 151 && code <= 153)) return WX_ICON_PARTLY;
    return WX_ICON_CLOUD;
}

static const char *wx_word(int32_t code)
{
    if(code >= 400 && code < 500) return "雪";
    if(code >= 300 && code < 400) return "雨";
    if(code == 100 || code == 150) return "晴";
    if((code >= 101 && code <= 103) || (code >= 151 && code <= 153)) return "多云";
    if(code >= 500 && code <= 515) return "雾";
    return "阴";
}

static const char *const week_names[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static bool is_daytime(const struct tm *lt)
{
    int now_min = lt->tm_hour * 60 + lt->tm_min;
    if(data.have_daily && data.day[0].sunrise >= 0 && data.day[0].sunset > data.day[0].sunrise)
        return now_min >= data.day[0].sunrise && now_min < data.day[0].sunset;
    /* 没有日出日落时的可配置兜底 */
    return now_min >= 6 * 60 && now_min < 18 * 60;
}

/* 季节索引 0=春 1=夏 2=秋 3=冬（对应素材包 season_mapping.json 的北半球规则）。
 * WX_SEASON_OVERRIDE 可以强制指定，便于实板逐个查看四季画风。 */
static int season_index(const struct tm *lt)
{
    if(WX_SEASON_OVERRIDE >= 1 && WX_SEASON_OVERRIDE <= 4)
        return WX_SEASON_OVERRIDE - 1;
    int m = lt->tm_mon + 1;
    if(m >= 3 && m <= 5) return 0;
    if(m >= 6 && m <= 8) return 1;
    if(m >= 9 && m <= 11) return 2;
    return 3;
}

/* 本次渲染是否需要阴天灰化：雨/雪/阴天时给当季背景加灰蓝滤镜 */
static bool wx_gloomy;

static int bg_for(bool day, int32_t code, const struct tm *lt)
{
    int si = lt ? season_index(lt) : 0;
    /* 夜晚用四季夜色背景。素材说明明确：昼夜要连树木/整体一起换，
     * 不能把白天背景直接变暗，因此夜有独立的四张图。 */
    if(!day) { wx_gloomy = false; return WX_BG_NIGHT_SPRING + si; }
    int k = icon_kind(code);
    /* 阴/雨/雪：白天仍用当季背景（保住季节感和会动的树），只加灰蓝滤镜压暗。
     * 不再用那张自带静态树的阴天图，否则会和动画树重叠。 */
    wx_gloomy = (k == WX_ICON_RAIN || k == WX_ICON_SNOW || k == WX_ICON_CLOUD);
    return WX_BG_SPRING + si;
}

/* ================= UI 小工具 ================= */
static lv_obj_t *wx_label(lv_obj_t *p, const char *text, int x, int y, int w,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *wx_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, 255, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                          LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

static lv_obj_t *wx_icon(lv_obj_t *p, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *o = lv_image_create(p);
    lv_image_set_src(o, src);
    lv_obj_set_pos(o, x, y);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ================= 页面切换 ================= */
static void page_show(int idx)
{
    if(idx < 0) idx = WX_PAGE_COUNT - 1;
    if(idx >= WX_PAGE_COUNT) idx = 0;
    wx_page_idx = idx;
    for(int i = 0; i < WX_PAGE_COUNT; i++) {
        if(!wx_page[i]) continue;
        if(i == idx) lv_obj_remove_flag(wx_page[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wx_page[i], LV_OBJ_FLAG_HIDDEN);
    }
    for(int i = 0; i < WX_PAGE_COUNT; i++) {
        if(!wx_dot[i]) continue;
        lv_obj_set_style_bg_opa(wx_dot[i], i == idx ? 230 : 90, 0);
        lv_obj_set_style_bg_color(wx_dot[i], lv_color_hex(i == idx ? 0xFFD24A : 0xFFFFFF), 0);
    }
}

/* ================= 渲染 ================= */
static void render_main(void)
{
    int64_t w = wall_now();
    struct tm lt;
    bool time_ok = wall_valid(w);
    if(time_ok) local_broken(w, &lt);

    lv_label_set_text(wx_city_lbl, WX_CITY_LABEL);

    if(!have_data) {
        /* 首次没有数据：占位符，绝不把 0°C 当实况 */
        lv_label_set_text(wx_temp_lbl, "--");
        lv_label_set_text(wx_word_lbl, "...");
        lv_label_set_text(wx_highlow_lbl, "-- / --");
        lv_obj_add_flag(wx_icon_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d°", (int)data.now.temp);
        lv_label_set_text(wx_temp_lbl, buf);
        lv_label_set_text(wx_word_lbl, wx_word(data.now.icon));
        if(data.have_daily)
            snprintf(buf, sizeof(buf), "%d° / %d°", (int)data.day[0].tmin, (int)data.day[0].tmax);
        else
            snprintf(buf, sizeof(buf), "-- / --");
        lv_label_set_text(wx_highlow_lbl, buf);
        lv_obj_remove_flag(wx_icon_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* 背景 / 树 / 图标 / 雨雪：全部由数据决定，不预合成 */
    bool day = time_ok ? is_daytime(&lt) : true;
    int32_t code = have_data ? data.now.icon : 104;
    int bg_idx = bg_for(day, code, time_ok ? &lt : NULL);
    lv_image_set_src(wx_bg_img, wx_bg[bg_idx]);
    /* 阴雨雪：当季背景叠灰蓝滤镜，压暗又保留季节感；晴天/多云不加滤镜 */
    lv_obj_set_style_image_recolor(wx_bg_img, lv_color_hex(0x8090B0), 0);
    lv_obj_set_style_image_recolor_opa(wx_bg_img, wx_gloomy ? 110 : 0, 0);

    /* 树与背景季节同步：白天 0-3，夜晚 4-7。阴雨雪也照常显示动画树。 */
    int si = time_ok ? season_index(&lt) : 0;
    int tree_set = day ? si : (4 + si);
    if(wx_tree_img && tree_set != wx_tree_set) {
        wx_tree_set = tree_set;
        wx_tree_frame = 0;
        lv_image_set_src(wx_tree_img, wx_trees[wx_tree_set][0]);
        lv_obj_remove_flag(wx_tree_img, LV_OBJ_FLAG_HIDDEN);
    }

    if(have_data) {
        int kind = icon_kind(data.now.icon);
        if(!day && (data.now.icon == 100 || data.now.icon == 150)) kind = WX_ICON_MOON;
        lv_image_set_src(wx_icon_img, wx_icons[kind]);

        if(kind == WX_ICON_RAIN || kind == WX_ICON_SNOW) {
            lv_obj_set_style_image_recolor(wx_fx, lv_color_hex(kind == WX_ICON_RAIN ? 0xAFCBEE : 0xFFFFFF), 0);
            lv_obj_set_style_image_recolor_opa(wx_fx, LV_OPA_COVER, 0);
            lv_image_set_src(wx_fx, kind == WX_ICON_RAIN ? wx_rain[0] : wx_snow[0]);
            lv_obj_remove_flag(wx_fx, LV_OBJ_FLAG_HIDDEN);
            wx_fx_until = lv_tick_get() + 4000u;
        } else {
            lv_obj_add_flag(wx_fx, LV_OBJ_FLAG_HIDDEN);
            wx_fx_until = 0;
        }
    } else {
        lv_obj_add_flag(wx_fx, LV_OBJ_FLAG_HIDDEN);
        wx_fx_until = 0;
    }

    if(time_ok) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
        lv_label_set_text(wx_time_lbl, buf);
        snprintf(buf, sizeof(buf), "%d月%d日 %s", lt.tm_mon + 1, lt.tm_mday,
                 week_names[lt.tm_wday % 7]);
        lv_label_set_text(wx_date_lbl, buf);
    } else {
        lv_label_set_text(wx_time_lbl, "--:--");
        lv_label_set_text(wx_date_lbl, "时间未校准");
    }

    if(have_data && wall_valid(data.now.obs_wall)) {
        struct tm ot;
        local_broken(data.now.obs_wall, &ot);
        char buf[40];
        snprintf(buf, sizeof(buf), "%02d:%02d 更新", ot.tm_hour, ot.tm_min);
        lv_label_set_text(wx_uptodate_lbl, buf);
    } else {
        lv_label_set_text(wx_uptodate_lbl, have_data ? "更新时间未知" : "--");
    }

    if(offline) lv_label_set_text(wx_status_lbl, "离线 · 无 Wi-Fi");
    else if(failed) lv_label_set_text(wx_status_lbl,
        have_data ? "更新失败 · 显示上次数据" : (last_err[0] ? last_err : "更新失败"));
    else lv_label_set_text(wx_status_lbl, "");
}

static void render_forecast(void)
{
    for(int d = 0; d < 3; d++) {
        if(!wx_fc_row[d][0]) continue;
        if(!data.have_daily) {
            lv_label_set_text(wx_fc_row[d][0], "--");
            lv_label_set_text(wx_fc_row[d][2], "--");
            lv_label_set_text(wx_fc_row[d][3], "--");
            lv_obj_add_flag(wx_fc_row[d][1], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int64_t w = wall_now();
        struct tm lt;
        const char *name = "--";
        if(wall_valid(w)) {
            time_t t = (time_t)(w + WX_TZ_OFFSET_SEC + (int64_t)d * 86400);
            gmtime_r(&t, &lt);
            name = week_names[lt.tm_wday % 7];
        }
        lv_label_set_text(wx_fc_row[d][0], name);
        lv_image_set_src(wx_fc_row[d][1], wx_icons[icon_kind(data.day[d].icon_day)]);
        lv_obj_remove_flag(wx_fc_row[d][1], LV_OBJ_FLAG_HIDDEN);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d°", (int)data.day[d].tmax);
        lv_label_set_text(wx_fc_row[d][2], buf);
        snprintf(buf, sizeof(buf), "%d°", (int)data.day[d].tmin);
        lv_label_set_text(wx_fc_row[d][3], buf);
    }
}

static void render_detail(void)
{
    char buf[24];
    if(!have_data) {
        for(int i = 0; i < 5; i++) if(wx_detail_val[i]) lv_label_set_text(wx_detail_val[i], "--");
        return;
    }
    if(!wx_detail_val[0]) return;
    snprintf(buf, sizeof(buf), "%d%%", (int)data.now.humidity);
    lv_label_set_text(wx_detail_val[0], buf);
    if(data.now.wind[0]) snprintf(buf, sizeof(buf), "%s %d", data.now.wind, (int)data.now.wind_scale);
    else snprintf(buf, sizeof(buf), "%d", (int)data.now.wind_scale);
    lv_label_set_text(wx_detail_val[1], buf);
    snprintf(buf, sizeof(buf), "%d hPa", (int)data.now.pressure);
    lv_label_set_text(wx_detail_val[2], buf);
    snprintf(buf, sizeof(buf), "%d km", (int)data.now.vis);
    lv_label_set_text(wx_detail_val[3], buf);
    snprintf(buf, sizeof(buf), "%d°", (int)data.now.feels);
    lv_label_set_text(wx_detail_val[4], buf);
}

static void render_all(void)
{
    render_main();
    render_forecast();
    render_detail();
}

/* ================= 网络层 ================= */
static int http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = WX_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if(!c) return -1;
    int total = -1;
    if(esp_http_client_open(c, 0) == ESP_OK) {
        if(esp_http_client_fetch_headers(c) >= 0) {
            int n = 0;
            total = 0;
            while((size_t)total < cap - 1) {
                n = esp_http_client_read(c, buf + total, cap - 1 - total);
                if(n <= 0) break;
                total += n;
            }
            buf[total] = 0;
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return total;
}

/* 和风天气**无条件**返回 Content-Encoding: gzip（连 Accept-Encoding: identity 都无视），
 * 而 esp_http_client 不会自动解压。这里按 gzip 魔数判断并 inflate。
 * 15+32 让 zlib 自动识别 gzip/zlib 头。明文响应原样透传。
 * 注意：入参 buf 会被原地覆盖为解压后的正文。 */
static int gunzip_if_needed(char *buf, int len, size_t cap)
{
    if(len < 18 || (uint8_t)buf[0] != 0x1f || (uint8_t)buf[1] != 0x8b)
        return len;

    static char plain[4096];   /* net_task 是单实例，静态避免大栈 */
    z_stream s;
    memset(&s, 0, sizeof(s));
    if(inflateInit2(&s, 15 + 32) != Z_OK) return -1;

    s.next_in = (Bytef *)buf;
    s.avail_in = (uInt)len;
    s.next_out = (Bytef *)plain;
    s.avail_out = (uInt)(sizeof(plain) - 1);

    int r = inflate(&s, Z_FINISH);
    int n = (int)s.total_out;
    inflateEnd(&s);

    if((r != Z_STREAM_END && r != Z_OK) || n <= 0) return -1;
    if((size_t)n > cap - 1) return -1;
    plain[n] = 0;
    memcpy(buf, plain, (size_t)n + 1);
    return n;
}

static int32_t jnum(const cJSON *o, const char *k, int32_t dflt)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if(!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) return dflt;
    return (int32_t)atoi(it->valuestring);
}

static void jstr(const cJSON *o, const char *k, char *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    out[0] = 0;
    if(cJSON_IsString(it) && it->valuestring)
        snprintf(out, cap, "%s", it->valuestring);
}

/* "06:25" -> 385 */
static int32_t hhmm(const char *s)
{
    if(!s || !s[0]) return -1;
    int h = 0, m = 0;
    if(sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    return h * 60 + m;
}

/* 公历 -> 1970-01-01 起的天数（Howard Hinnant 算法）。
 * 不用 timegm()：newlib 对该 BSD 扩展的支持随 IDF 版本而异。 */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* 解析 "2026-11-16T21:25+08:00" -> UTC 秒（用配置的地点偏移反算） */
static int64_t iso_local_to_utc(const char *s)
{
    if(!s || strlen(s) < 16) return 0;
    int Y = 0, M = 0, D = 0, h = 0, mi = 0;
    if(sscanf(s, "%4d-%2d-%2dT%2d:%2d", &Y, &M, &D, &h, &mi) != 5) return 0;
    if(M < 1 || M > 12 || D < 1 || D > 31) return 0;
    /* 先把「当地墙钟」当成 UTC 秒数，再减去时区偏移得到真实 UTC */
    return days_from_civil(Y, M, D) * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60
           - WX_TZ_OFFSET_SEC;
}

static bool fetch_all(wx_net_t *r)
{
    static char buf[4096];   /* task 单实例，静态避免 4 KB 栈占用 */
    char url[192];

    snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s&lang=en",
             WX_API_HOST, WX_LOCATION_ID, WX_API_KEY);
    int n = http_get(url, buf, sizeof(buf));
    if(n <= 0) { snprintf(r->err, sizeof(r->err), "请求失败"); return false; }
    n = gunzip_if_needed(buf, n, sizeof(buf));
    if(n <= 0) { snprintf(r->err, sizeof(r->err), "解压失败"); return false; }

    cJSON *root = cJSON_Parse(buf);
    if(!root) { snprintf(r->err, sizeof(r->err), "解析失败"); return false; }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = cJSON_IsString(code) && code->valuestring && !strcmp(code->valuestring, "200");
    if(!ok) {
        if(cJSON_IsString(code) && code->valuestring)
            snprintf(r->err, sizeof(r->err), "接口%s", code->valuestring);
        else
            snprintf(r->err, sizeof(r->err), "接口异常");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(root, "now");
    if(ok && cJSON_IsObject(now)) {
        r->now.temp = jnum(now, "temp", INT32_MIN);
        if(r->now.temp != INT32_MIN) {
            r->have_now = true;
            r->now.feels = jnum(now, "feelsLike", r->now.temp);
            r->now.humidity = jnum(now, "humidity", 0);
            r->now.pressure = jnum(now, "pressure", 0);
            r->now.vis = jnum(now, "vis", 0);
            r->now.wind_scale = jnum(now, "windScale", 0);
            r->now.icon = jnum(now, "icon", -1);
            jstr(now, "windDir", r->now.wind, sizeof(r->now.wind));
            r->now.obs_wall = iso_local_to_utc(cJSON_GetObjectItemCaseSensitive(now, "obsTime")
                                                   ? cJSON_GetObjectItemCaseSensitive(now, "obsTime")->valuestring
                                                   : NULL);
        }
    }
    cJSON_Delete(root);
    if(!r->have_now) { snprintf(r->err, sizeof(r->err), "数据缺失"); return false; }

    snprintf(url, sizeof(url), "https://%s/v7/weather/3d?location=%s&key=%s&lang=en",
             WX_API_HOST, WX_LOCATION_ID, WX_API_KEY);
    n = http_get(url, buf, sizeof(buf));
    if(n <= 0) return true;   /* 实况已成功，预报失败不影响主页面 */
    n = gunzip_if_needed(buf, n, sizeof(buf));
    if(n <= 0) return true;

    root = cJSON_Parse(buf);
    if(!root) return true;
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if(cJSON_IsArray(daily)) {
        int i = 0;
        const cJSON *d;
        cJSON_ArrayForEach(d, daily) {
            if(i >= 3) break;
            wx_day_t *t = &r->day[i++];
            t->tmax = jnum(d, "tempMax", 0);
            t->tmin = jnum(d, "tempMin", 0);
            t->icon_day = jnum(d, "iconDay", -1);
            t->sunrise = hhmm(cJSON_GetObjectItemCaseSensitive(d, "sunrise") ? cJSON_GetObjectItemCaseSensitive(d, "sunrise")->valuestring : NULL);
            t->sunset = hhmm(cJSON_GetObjectItemCaseSensitive(d, "sunset") ? cJSON_GetObjectItemCaseSensitive(d, "sunset")->valuestring : NULL);
            r->have_daily = true;
        }
    }
    cJSON_Delete(root);
    return true;
}

static void net_task(void *arg)
{
    (void)arg;
    static wx_net_t out;   /* 只有一个 task，静态避免大栈 */
    for(;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if(atomic_load(&wx_abort)) { atomic_store(&wx_busy, false); continue; }

        memset(&out, 0, sizeof(out));
        out.serial = ++request_serial;
        out.ok = fetch_all(&out);

        /* 退出页面后结果直接丢弃，绝不访问已销毁的控件 */
        if(!atomic_load(&wx_abort) && wx_results)
            xQueueOverwrite(wx_results, &out);

        atomic_store(&wx_busy, false);
    }
}

static void request_refresh(bool manual)
{
    if(!wx_task) return;
    if(atomic_load(&wx_busy)) return;
    int64_t w = wall_now();
    if(manual) {
        if(wall_valid(w) && wall_valid(last_manual_wall) && w - last_manual_wall < WX_MANUAL_MIN_SEC)
            return;                                    /* 节流：60 秒内只手动一次 */
        last_manual_wall = w;
    }
    atomic_store(&wx_busy, true);
    xTaskNotifyGive(wx_task);
}

/* ================= 交互 ================= */
static void back_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    if(wx_back) wx_back();
}

static void refresh_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    request_refresh(true);
    if(wx_status_lbl) lv_label_set_text(wx_status_lbl, atomic_load(&wx_busy) ? "更新中..." : "");
}

/* 左右滑动切页：复用工程已有 PRESSED/PRESSING/RELEASED 模式（地图拖动同款），
 * 不走 LV_EVENT_GESTURE，避免与 badge_ui_click_guard 的点击吞噬互相干扰。 */
static lv_point_t swipe_origin;
static bool swipe_active;

static void swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if(!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);

    if(code == LV_EVENT_PRESSED) {
        swipe_origin = p;
        swipe_active = true;
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(!swipe_active) return;
        swipe_active = false;
        int dx = p.x - swipe_origin.x, dy = p.y - swipe_origin.y;
        if(abs(dx) < 40 || abs(dx) < abs(dy)) return;   /* 竖向滑动不切页 */
        page_show(wx_page_idx + (dx < 0 ? 1 : -1));
    }
}

/* ================= UI 构建 ================= */
static void build_main_page(void)
{
    lv_obj_t *p = wx_page[WX_PAGE_MAIN];

    wx_city_lbl = wx_label(p, WX_CITY_LABEL, 60, 40, 240, &font18, 0xFFFFFF);
    lv_obj_set_style_text_color(wx_city_lbl, lv_color_hex(0xFFF3C4), 0);

    wx_icon_img = wx_icon(p, wx_icons[WX_ICON_CLOUD], 156, 68);

    wx_temp_lbl = wx_label(p, "--", 80, 118, 200, &font36, 0xFFFFFF);
    wx_word_lbl = wx_label(p, "--", 80, 168, 200, &font18, 0xFFF3C4);
    wx_highlow_lbl = wx_label(p, "-- / --", 80, 196, 200, &font14, 0xDCE8F5);

    wx_time_lbl = wx_label(p, "--:--", 80, 228, 200, &font24, 0xFFFFFF);
    wx_date_lbl = wx_label(p, "", 60, 264, 240, &font14, 0xE6EEF7);
    wx_uptodate_lbl = wx_label(p, "", 60, 284, 240, &font14, 0xAFC4D8);

    wx_status_lbl = wx_label(p, "", 40, 306, 280, &font14, 0xFFB74D);
    lv_obj_set_style_bg_color(wx_status_lbl, lv_color_hex(0x1A1208), 0);
    lv_obj_set_style_bg_opa(wx_status_lbl, 150, 0);
    lv_obj_set_style_radius(wx_status_lbl, 8, 0);
}

static void build_forecast_page(void)
{
    lv_obj_t *p = wx_page[WX_PAGE_FORECAST];
    wx_label(p, "未来三天", 80, 42, 200, &font24, 0xFFF3C4);

    for(int d = 0; d < 3; d++) {
        int y = 104 + d * 58;
        lv_obj_t *card = wx_box(p, 48, y, 264, 50, 0x1A1208, 14);
        lv_obj_set_style_bg_opa(card, 140, 0);

        wx_fc_row[d][0] = wx_label(card, "--", 8, 13, 70, &font18, 0xFFFFFF);
        wx_fc_row[d][1] = wx_icon(card, wx_icons[WX_ICON_CLOUD], 86, 1);
        wx_fc_row[d][2] = wx_label(card, "--", 150, 8, 50, &font18, 0xFFD24A);
        wx_fc_row[d][3] = wx_label(card, "--", 200, 8, 50, &font18, 0xAFC4D8);
    }
}

static void build_detail_page(void)
{
    lv_obj_t *p = wx_page[WX_PAGE_DETAIL];
    wx_label(p, "详细数据", 80, 42, 200, &font24, 0xFFF3C4);

    const char *names[5] = {"湿度", "风力", "气压", "能见度", "体感"};
    for(int i = 0; i < 5; i++) {
        int y = 98 + i * 42;
        wx_label(p, names[i], 62, y + 6, 96, &font18, 0xDCE8F5);
        wx_detail_val[i] = wx_label(p, "--", 158, y + 6, 140, &font18, 0xFFFFFF);
    }
}

static void build_ui(void)
{
    wx_root = lv_obj_create(wx_panel);
    lv_obj_remove_style_all(wx_root);
    lv_obj_set_pos(wx_root, 0, 0);
    lv_obj_set_size(wx_root, BG_W, BG_H);
    lv_obj_remove_flag(wx_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                                  LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 背景（静态 const，零解码） */
    wx_bg_img = wx_icon(wx_root, wx_bg[WX_BG_SPRING], 0, 0);

    /* 四季树木：独立透明图层叠在背景之上（素材包背景已移除树木，
     * 这样昼夜/季节切换只换图，不需要重新合成整屏） */
    wx_tree_img = wx_icon(wx_root, wx_trees[0][0], WX_TREE_X, WX_TREE_Y);
    wx_tree_set = 0;
    wx_tree_frame = 0;
    wx_tree_frame_at = lv_tick_get();

    /* 雨雪覆盖层：A8 单通道 + recolor 上色 */
    wx_fx = lv_image_create(wx_root);
    lv_obj_set_pos(wx_fx, 0, 0);
    lv_obj_remove_flag(wx_fx, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wx_fx, LV_OBJ_FLAG_HIDDEN);

    /* 三个页面容器（背景与特效共用，页面只装文字与图标） */
    for(int i = 0; i < WX_PAGE_COUNT; i++) {
        wx_page[i] = lv_obj_create(wx_root);
        lv_obj_remove_style_all(wx_page[i]);
        lv_obj_set_pos(wx_page[i], 0, 0);
        lv_obj_set_size(wx_page[i], BG_W, BG_H);
        lv_obj_remove_flag(wx_page[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                                        LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(wx_page[i], LV_OBJ_FLAG_HIDDEN);
    }
    build_main_page();
    build_forecast_page();
    build_detail_page();

    /* 滑动热区：铺在整个圆屏上，位于页面之下、背景之上 */
    lv_obj_t *swipe = lv_obj_create(wx_root);
    lv_obj_remove_style_all(swipe);
    lv_obj_set_pos(swipe, 0, 0);
    lv_obj_set_size(swipe, BG_W, BG_H);
    lv_obj_set_style_bg_opa(swipe, 0, 0);
    lv_obj_add_flag(swipe, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swipe, swipe_cb, LV_EVENT_ALL, NULL);

    /* 返回 / 刷新（放在滑动热区之上，保证点击优先） */
    lv_obj_t *back_btn = wx_box(wx_root, 76, 26, 44, 44, 0x1A1208, 22);
    lv_obj_set_style_bg_opa(back_btn, 160, 0);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, badge_ui_click_guard, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back_btn);
    lv_label_set_text(arrow, "‹");
    lv_obj_set_style_text_font(arrow, &font24, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFF3C4), 0);
    lv_obj_center(arrow);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *refresh_btn = wx_box(wx_root, 240, 26, 44, 44, 0x1A1208, 22);
    lv_obj_set_style_bg_opa(refresh_btn, 160, 0);
    lv_obj_add_flag(refresh_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh_btn, badge_ui_click_guard, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(refresh_btn, refresh_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *rlbl = lv_label_create(refresh_btn);
    /* 没用 U+21BB "↻"：源字体没有该字形，fonttools 会导出空 glyph，实机画成方框+叉。
     * 改用中文，字形一定存在。 */
    lv_label_set_text(rlbl, "刷新");
    lv_obj_set_style_text_font(rlbl, &font14, 0);
    lv_obj_set_style_text_color(rlbl, lv_color_hex(0xFFF3C4), 0);
    lv_obj_center(rlbl);
    lv_obj_remove_flag(rlbl, LV_OBJ_FLAG_CLICKABLE);

    /* 页面指示点 */
    for(int i = 0; i < WX_PAGE_COUNT; i++) {
        wx_dot[i] = wx_box(wx_root, 166 + i * 14, 330, 8, 8, 0xFFFFFF, 4);
        lv_obj_set_style_bg_opa(wx_dot[i], 90, 0);
    }
}

/* ================= 生命周期 ================= */
bool weather_open(lv_obj_t *panel, void (*back)(void))
{
    weather_close();
    wx_back = back;
    wx_panel = panel;
    wx_showing = true;
    wx_page_idx = WX_PAGE_MAIN;

    if(!wx_results) wx_results = xQueueCreate(1, sizeof(wx_net_t));
    if(!wx_results) { wx_showing = false; return false; }
    atomic_store(&wx_abort, false);

    if(!wx_task) {
        if(xTaskCreate(net_task, "wx_net", 8192, NULL, 3, &wx_task) != pdPASS) {
            wx_task = NULL;
            wx_showing = false;
            return false;
        }
    }

    build_ui();
    page_show(WX_PAGE_MAIN);
    render_all();

    /* 时间源复用工程既有 SNTP（TimeTiles / 像素农场同一条通道） */
    badge_apps_clock_sync();

    /* 首次进入或数据过期才发请求；退出期间不请求 */
    int64_t w = wall_now();
    bool stale = !have_data || !wall_valid(w) || w >= next_refresh_wall;
    if(stale) request_refresh(false);
    return true;
}

void weather_close(void)
{
    if(wx_root) {
        atomic_store(&wx_abort, true);   /* 丢弃在途结果，避免访问已销毁控件 */
        lv_obj_delete(wx_root);
        wx_root = NULL;
    }
    wx_page[0] = wx_page[1] = wx_page[2] = NULL;
    wx_dot[0] = wx_dot[1] = wx_dot[2] = NULL;
    wx_bg_img = wx_tree_img = wx_fx = wx_icon_img = NULL;
    wx_tree_frame_at = 0;
    wx_city_lbl = wx_temp_lbl = wx_word_lbl = wx_highlow_lbl = NULL;
    wx_time_lbl = wx_date_lbl = wx_uptodate_lbl = wx_status_lbl = NULL;
    memset(wx_fc_row, 0, sizeof(wx_fc_row));
    memset(wx_detail_val, 0, sizeof(wx_detail_val));
    wx_fx_until = 0;
    wx_fx_frame = 0;
    swipe_active = false;
    wx_showing = false;
    wx_panel = NULL;
}

void weather_tick(uint32_t now_ms, bool visible)
{
    if(!wx_root || !wx_showing) return;
    (void)visible;

    /* 1. 收结果（GUI task 上下文） */
    wx_net_t r;
    if(wx_results && xQueueReceive(wx_results, &r, 0) == pdTRUE) {
        int64_t w = wall_now();
        if(r.ok) {
            /* 实况只接受比已有数据更新的观测时间，避免乱序覆盖 */
            if(!have_data || !wall_valid(data.now.obs_wall) || !wall_valid(r.now.obs_wall) ||
               r.now.obs_wall >= data.now.obs_wall) {
                data = r;
                have_data = true;
                failed = false;
                if(wall_valid(w)) next_refresh_wall = w + WX_REFRESH_SEC;
                render_all();
            }
        } else {
            failed = true;
            snprintf(last_err, sizeof(last_err), "%s", r.err);
            if(wall_valid(w)) {
                int64_t back_off = WX_RETRY_BASE_SEC;
                if(wall_valid(last_fail_wall)) {
                    int64_t gap = w - last_fail_wall;
                    if(gap > 0 && gap < WX_RETRY_MAX_SEC) back_off = gap * 2;
                }
                if(back_off > WX_RETRY_MAX_SEC) back_off = WX_RETRY_MAX_SEC;
                next_refresh_wall = w + back_off;
                last_fail_wall = w;
            }
            render_main();
        }
    }

    /* 2. 雨雪帧循环：4 fps，纯 set_src，不触发布局 */
    if(wx_fx_until && wx_fx && !lv_obj_has_flag(wx_fx, LV_OBJ_FLAG_HIDDEN)) {
        if(now_ms >= wx_fx_until) {
            lv_obj_add_flag(wx_fx, LV_OBJ_FLAG_HIDDEN);
            wx_fx_until = 0;
        } else {
            static uint32_t last_swap;
            if(now_ms - last_swap >= 250u) {
                last_swap = now_ms;
                wx_fx_frame = (wx_fx_frame + 1) & 1;
                bool rain = data.now.icon >= 300 && data.now.icon < 400;
                lv_image_set_src(wx_fx, rain ? wx_rain[wx_fx_frame] : wx_snow[wx_fx_frame]);
            }
        }
    }

    /* 3. 树木帧循环：素材建议 450ms/帧 */
    if(wx_tree_img && wx_tree_frame_at && now_ms - wx_tree_frame_at >= 450u) {
        wx_tree_frame_at = now_ms;
        wx_tree_frame = (wx_tree_frame + 1) & 3;
        lv_image_set_src(wx_tree_img, wx_trees[wx_tree_set][wx_tree_frame]);
    }

    /* 3. 时间每 10 秒刷新一次（时钟走字 + 昼夜可能跨界） */
    static uint32_t last_clock;
    if(now_ms - last_clock >= 10000u) {
        last_clock = now_ms;
        render_main();
    }

    /* 4. 到期自动刷新 */
    int64_t w = wall_now();
    offline = !net_online();
    if(wall_valid(w) && w >= next_refresh_wall && !atomic_load(&wx_busy)) {
        if(!offline) request_refresh(false);
        else next_refresh_wall = w + WX_RETRY_BASE_SEC;
    }
}
