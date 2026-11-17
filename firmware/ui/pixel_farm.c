#include "pixel_farm.h"
#include "apps.h"
#include "src/misc/cache/lv_cache.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include <sys/time.h>
#include "nvs.h"
#endif

/* ================= 布局（360x360 圆屏，圆心 180,180 半径 180） =================
 * 地块坐标取自素材包 layout.json 的 farm_plot_rects，作物锚点取自
 * crop_48_anchors（48x48 图放在 73x56 地块上居中）。工具行与金币条按圆内
 * 可用弦宽重新定位：y=340 处可用 x∈[97,263]，y=20 处可用 x∈[102,258]。
 */
static const int16_t plot_x[FARM_PLOT_COUNT] = { 62, 143, 224,  62, 143, 224 };
static const int16_t plot_y[FARM_PLOT_COUNT] = { 169, 169, 169, 232, 232, 232 };
static const int16_t crop_x[FARM_PLOT_COUNT] = { 75, 156, 237,  75, 156, 237 };
static const int16_t crop_y[FARM_PLOT_COUNT] = { 175, 175, 175, 237, 237, 237 };
#define PLOT_W 73
#define PLOT_H 56

static const int16_t tool_x[3] = { 100, 156, 212 };
#define TOOL_Y      292
#define TOOL_SIZE   48

#define BACK_X 88
#define BACK_Y 32

/* ================= 存档 ================= */
#define FARM_SAVE_MAGIC   0x4641524Du /* 'FARM' */
#define FARM_SAVE_VERSION 1u

typedef struct {
    uint8_t crop;      /* FARM_CROP_* */
    uint8_t watered;   /* 0/1 */
    uint8_t pad[2];
    uint32_t grown;    /* 已累计成长秒数（只在浇水后累加） */
} farm_plot_t;

typedef struct {
    uint32_t magic, version;
    int32_t coins;
    uint8_t selected, pad[3];
    int64_t saved_wall; /* 存档时刻 UTC 秒，0 = 当时时间不可信 */
    farm_plot_t plot[FARM_PLOT_COUNT];
} farm_save_t;

static farm_plot_t plots[FARM_PLOT_COUNT];
static int32_t coins;
static uint8_t selected_crop;
static int tool;            /* 0 播种 1 浇水 2 收获 */
static bool loaded, dirty, resume_warn, offline_ok;
static int store_error;
static uint32_t offline_seconds;
static uint32_t last_store_ms;

/* ================= 状态 ================= */
static lv_obj_t *farm;                       /* 本应用根容器 */
static lv_obj_t *crop_img[FARM_PLOT_COUNT];
static lv_obj_t *wet_badge[FARM_PLOT_COUNT];
static lv_obj_t *ripe_ring[FARM_PLOT_COUNT];
static lv_obj_t *plot_hit[FARM_PLOT_COUNT];
static lv_obj_t *tool_box[3];
static lv_obj_t *tool_icon[3];
static lv_obj_t *coin_label;
static lv_obj_t *seed_bar, *seed_btn[2], *seed_mark[2];
static lv_obj_t *hint_label;
static lv_obj_t *sparkle_img;
static void (*return_to_list)(void);
static bool showing;
static uint32_t hint_until, sparkle_until, pulse_ms;
static int64_t tick_wall;      /* 上次 tick 的可信 UTC 秒，0 = 当时不可信 */
static uint32_t tick_ms;       /* tick_wall 不可用时的单调时钟基准 */
static int sparkle_plot = -1;
static uint8_t shown_stage[FARM_PLOT_COUNT];

/* ================= 时间 ================= */
#define FARM_WALL_VALID(t) ((t) > INT64_C(1700000000))

static int64_t wall_now(void)
{
#ifdef ESP_PLATFORM
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
#else
    return (int64_t)time(NULL);
#endif
}

static uint32_t grow_seconds(uint8_t crop)
{
    return crop == FARM_CROP_TURNIP ? FARM_TURNIP_GROW_SECONDS
                                    : FARM_CARROT_GROW_SECONDS;
}

static int32_t crop_coins(uint8_t crop)
{
    return crop == FARM_CROP_TURNIP ? FARM_TURNIP_COINS : FARM_CARROT_COINS;
}

static bool is_ripe(const farm_plot_t *p)
{
    if(p->crop == FARM_CROP_NONE || !p->watered) return false;
    return p->grown >= grow_seconds(p->crop);
}

/* 4 个成长阶段：0 刚播种/未浇水，1 已过 1/3，2 已过 2/3，3 成熟 */
static uint8_t stage_of(const farm_plot_t *p)
{
    if(p->crop == FARM_CROP_NONE) return 0;
    if(!p->watered) return 0;
    uint32_t need = grow_seconds(p->crop), g = p->grown;
    if(g >= need) return 3;
    if(g * 3u >= need * 2u) return 2;
    if(g * 3u >= need) return 1;
    return 0;
}

/* 最多推进到成熟，不会越过；重复调用安全（计时器不被重置） */
static void advance(uint32_t seconds)
{
    if(!seconds) return;
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        farm_plot_t *p = &plots[i];
        if(p->crop == FARM_CROP_NONE || !p->watered) continue;
        uint32_t need = grow_seconds(p->crop);
        if(p->grown >= need) continue;
        uint32_t g = p->grown + seconds;
        p->grown = g > need ? need : g;   /* 饱和加法，防溢出 */
        dirty = true;
    }
}

/* ================= 存档 ================= */
static bool save_valid(const farm_save_t *s)
{
    if(s->magic != FARM_SAVE_MAGIC || s->version != FARM_SAVE_VERSION) return false;
    if(s->coins < 0 || s->coins > 9999999) return false;
    if(s->selected != FARM_CROP_CARROT && s->selected != FARM_CROP_TURNIP) return false;
    if(s->saved_wall < 0) return false;
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        const farm_plot_t *p = &s->plot[i];
        if(p->crop > FARM_CROP_TURNIP || p->watered > 1) return false;
        if(p->crop == FARM_CROP_NONE && (p->watered || p->grown)) return false;
        if(p->crop != FARM_CROP_NONE && p->grown > grow_seconds(p->crop)) return false;
    }
    return true;
}

static void store(void)
{
    farm_save_t s;
    memset(&s, 0, sizeof(s));
    s.magic = FARM_SAVE_MAGIC;
    s.version = FARM_SAVE_VERSION;
    s.coins = coins;
    s.selected = selected_crop;
    int64_t w = wall_now();
    s.saved_wall = FARM_WALL_VALID(w) ? w : 0;
    memcpy(s.plot, plots, sizeof(plots));
#ifdef ESP_PLATFORM
    {
        nvs_handle_t h;
        store_error = nvs_open("badge_pixel_farm", NVS_READWRITE, &h);
        if(store_error == ESP_OK) {
            store_error = nvs_set_blob(h, "state_v1", &s, sizeof(s));
            if(store_error == ESP_OK) store_error = nvs_commit(h);
            nvs_close(h);
        }
    }
#else
    (void)s;
    store_error = 0;
#endif
    dirty = false;
    last_store_ms = lv_tick_get();
}

static void load(void)
{
    if(loaded) return;
    loaded = true;
    memset(plots, 0, sizeof(plots));
    coins = 0;
    selected_crop = FARM_CROP_CARROT;
    resume_warn = false;
    offline_ok = false;

    farm_save_t s;
    bool have = false;
#ifdef ESP_PLATFORM
    {
        nvs_handle_t h;
        size_t size = sizeof(s);
        store_error = nvs_open("badge_pixel_farm", NVS_READONLY, &h);
        if(store_error == ESP_OK) {
            store_error = nvs_get_blob(h, "state_v1", &s, &size);
            nvs_close(h);
            if(store_error == ESP_OK && size == sizeof(s) && save_valid(&s)) have = true;
            else if(store_error == ESP_OK) store_error = ESP_ERR_INVALID_ARG;
        }
        if(store_error == ESP_ERR_NVS_NOT_FOUND) store_error = ESP_OK;
    }
#endif
    if(!have) return;

    coins = s.coins;
    selected_crop = s.selected;
    memcpy(plots, s.plot, sizeof(plots));
    dirty = true;

    /* 断电/退出期间的成长：只有两端时间都可信才补算，绝不回退 */
    int64_t now = wall_now();
    if(FARM_WALL_VALID(now) && FARM_WALL_VALID(s.saved_wall)) {
        int64_t d = now - s.saved_wall;
        if(d < 0) {
            /* 时钟被调回过去：不推进，也不让 grown 减少 */
            resume_warn = true;
            return;
        }
        if(d > (int64_t)FARM_MAX_OFFLINE_SECONDS) d = FARM_MAX_OFFLINE_SECONDS;
        if(d > 0) {
            advance((uint32_t)d);
            offline_ok = true;
            offline_seconds = (uint32_t)d;
        }
    } else {
        /* 无可信时间：保留存档，成长暂停 */
        for(int i = 0; i < FARM_PLOT_COUNT; i++) {
            if(plots[i].crop != FARM_CROP_NONE && plots[i].watered && !is_ripe(&plots[i])) {
                resume_warn = true;
                break;
            }
        }
    }
}

/* ================= 视觉 ================= */
static lv_obj_t *frame_obj(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, int radius, int opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                          LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

static void hint(const char *text, uint32_t color)
{
    if(!hint_label) return;
    lv_label_set_text(hint_label, text);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(color), 0);
    lv_obj_remove_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    hint_until = lv_tick_get() + 1700u;
}

static void refresh_coins(void)
{
    if(coin_label) lv_label_set_text_fmt(coin_label, "%ld", (long)coins);
}

static void refresh_tools(void)
{
    for(int i = 0; i < 3; i++) {
        bool on = (i == tool);
        lv_obj_set_style_border_width(tool_box[i], on ? 3 : 0, 0);
        lv_obj_set_style_border_color(tool_box[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_set_style_border_opa(tool_box[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(tool_box[i], on ? 220 : 140, 0);
        lv_obj_set_style_bg_color(tool_box[i], lv_color_hex(on ? 0x3B2E1E : 0x241B12), 0);
    }
    /* 播种位图标跟随当前种子类型，让"将要种什么"始终可见 */
    if(tool_icon[0])
        lv_image_set_src(tool_icon[0], pf_crops[selected_crop == FARM_CROP_TURNIP ? 7 : 3]);
    /* 作物选择条只在播种工具选中时出现 */
    if(seed_bar) {
        if(tool == 0) lv_obj_remove_flag(seed_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(seed_bar, LV_OBJ_FLAG_HIDDEN);
    }
    for(int i = 0; i < 2; i++) {
        if(!seed_btn[i]) continue;
        uint8_t crop = i == 0 ? FARM_CROP_CARROT : FARM_CROP_TURNIP;
        bool on = selected_crop == crop;
        lv_obj_set_style_border_width(seed_btn[i], on ? 2 : 0, 0);
        lv_obj_set_style_border_color(seed_btn[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_set_style_border_opa(seed_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(seed_btn[i], on ? 210 : 130, 0);
        if(seed_mark[i])
            lv_label_set_text(seed_mark[i], on ? "●" : "○");
    }
}

static void refresh_plot(int i)
{
    farm_plot_t *p = &plots[i];
    uint8_t stage = stage_of(p);
    if(p->crop == FARM_CROP_NONE) {
        lv_obj_add_flag(crop_img[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(crop_img[i],
                         pf_crops[(p->crop == FARM_CROP_TURNIP ? 4 : 0) + stage]);
        lv_obj_remove_flag(crop_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(p->crop != FARM_CROP_NONE && p->watered)
        lv_obj_remove_flag(wet_badge[i], LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(wet_badge[i], LV_OBJ_FLAG_HIDDEN);

    if(is_ripe(p)) lv_obj_remove_flag(ripe_ring[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ripe_ring[i], LV_OBJ_FLAG_HIDDEN);
    shown_stage[i] = stage;
}

static void refresh_all(void)
{
    for(int i = 0; i < FARM_PLOT_COUNT; i++) refresh_plot(i);
    refresh_coins();
    refresh_tools();
}

static void play_sparkle(int i)
{
    if(!sparkle_img) return;
    sparkle_plot = i;
    lv_image_set_src(sparkle_img, pf_sparkle[0]);
    lv_obj_set_pos(sparkle_img, plot_x[i] + PLOT_W / 2 - 32,
                   plot_y[i] + PLOT_H / 2 - 32);
    lv_obj_remove_flag(sparkle_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sparkle_img);
    sparkle_until = lv_tick_get() + 420u;
}

/* ================= 交互 ================= */
static void act_plot(int i)
{
    farm_plot_t *p = &plots[i];
    if(tool == 0) {                     /* 播种 */
        if(p->crop != FARM_CROP_NONE) {
            hint("这块地已经有作物了", 0xFF8A65);
            return;
        }
        p->crop = selected_crop;
        p->watered = 0;
        p->grown = 0;
        dirty = true;
        hint(FARM_WATER_REQUIRED ? "已播种 · 记得浇水" : "已播种", 0xFFE082);
        refresh_plot(i);
        store();
        return;
    }
    if(tool == 1) {                     /* 浇水 */
        if(p->crop == FARM_CROP_NONE) {
            hint("空地不需要浇水", 0xFF8A65);
            return;
        }
        if(p->watered) {
            if(is_ripe(p)) hint("已经成熟 · 可以收获", 0xFFE082);
            else hint("已经浇过水了", 0xFF8A65);
            return;
        }
        p->watered = 1;
        dirty = true;
        hint("浇水完成 · 开始成长", 0x9BE08A);
        refresh_plot(i);
        store();
        return;
    }
    /* 收获 */
    if(p->crop == FARM_CROP_NONE) {
        hint("空地没有可收获的作物", 0xFF8A65);
        return;
    }
    if(!is_ripe(p)) {
        hint(p->watered ? "还没成熟" : "还没浇水", 0xFF8A65);
        return;
    }
    int32_t gain = crop_coins(p->crop);
    coins += gain;
    play_sparkle(i);
    memset(p, 0, sizeof(*p));
    dirty = true;
    char msg[40];
    snprintf(msg, sizeof(msg), "收获 +%ld 金币", (long)gain);
    hint(msg, 0x9BE08A);
    refresh_plot(i);
    refresh_coins();
    store();
}

static void plot_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    act_plot((int)(intptr_t)lv_event_get_user_data(e));
}

static void tool_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    tool = (int)(intptr_t)lv_event_get_user_data(e);
    refresh_tools();
    if(!hint_until)
        hint(tool == 0 ? "选作物后点地块播种" : tool == 1 ? "点已播种的地块浇水"
                                                          : "点成熟作物收获",
             0xFFF3C4);
}

static void seed_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    selected_crop = (int)(intptr_t)lv_event_get_user_data(e) ? FARM_CROP_TURNIP
                                                             : FARM_CROP_CARROT;
    refresh_tools();
    hint(selected_crop == FARM_CROP_TURNIP ? "已选紫萝卜 · 5 分钟成熟"
                                           : "已选胡萝卜 · 3 分钟成熟",
         0xFFF3C4);
}

static void back_event(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    if(return_to_list) return_to_list();
}

/* ================= 生命周期 ================= */
bool pixel_farm_open(lv_obj_t *panel, void (*back)(void))
{
    pixel_farm_close();
    return_to_list = back;
    load();

    farm = lv_obj_create(panel);
    lv_obj_remove_style_all(farm);
    lv_obj_set_pos(farm, 0, 0);
    lv_obj_set_size(farm, 360, 360);
    lv_obj_remove_flag(farm, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                             LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 1. 背景（静态 const，零解码） */
    lv_obj_t *bg = lv_image_create(farm);
    lv_image_set_src(bg, &pf_bg);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 2. 地块：成熟金环 + 作物 + 浇水角标 + 触摸热区（热区最上层，保证点击） */
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        ripe_ring[i] = frame_obj(farm, plot_x[i], plot_y[i], PLOT_W, PLOT_H, 0x000000, 6, 0);
        lv_obj_set_style_border_width(ripe_ring[i], 2, 0);
        lv_obj_set_style_border_color(ripe_ring[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_set_style_border_opa(ripe_ring[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(ripe_ring[i], 40, 0);
        lv_obj_set_style_bg_color(ripe_ring[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_add_flag(ripe_ring[i], LV_OBJ_FLAG_HIDDEN);

        crop_img[i] = lv_image_create(farm);
        lv_obj_set_pos(crop_img[i], crop_x[i], crop_y[i]);
        lv_obj_remove_flag(crop_img[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(crop_img[i], LV_OBJ_FLAG_HIDDEN);

        wet_badge[i] = lv_image_create(farm);
        lv_image_set_src(wet_badge[i], pf_icons[PF_ICON_SOIL_WET]);
        lv_image_set_scale(wet_badge[i], 107);   /* 48 * 107/256 ≈ 20px */
        lv_obj_set_pos(wet_badge[i], plot_x[i] + PLOT_W - 22, plot_y[i] + PLOT_H - 22);
        lv_obj_remove_flag(wet_badge[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(wet_badge[i], LV_OBJ_FLAG_HIDDEN);
    }
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        plot_hit[i] = frame_obj(farm, plot_x[i], plot_y[i], PLOT_W, PLOT_H, 0x000000, 0, 0);
        lv_obj_add_flag(plot_hit[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(plot_hit[i], badge_ui_click_guard, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(plot_hit[i], plot_event, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* 3. 顶部金币 */
    lv_obj_t *coin_bg = frame_obj(farm, 130, 18, 100, 28, 0x1A1208, 14, 150);
    lv_obj_remove_flag(coin_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *coin = lv_image_create(farm);
    lv_image_set_src(coin, pf_icons[PF_ICON_COIN]);
    lv_image_set_scale(coin, 96);                /* 48 -> 18px */
    lv_obj_set_pos(coin, 138, 23);
    lv_obj_remove_flag(coin, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    coin_label = lv_label_create(farm);
    lv_label_set_text(coin_label, "0");
    lv_obj_set_pos(coin_label, 160, 21);
    lv_obj_set_style_text_font(coin_label, &font18, 0);
    lv_obj_set_style_text_color(coin_label, lv_color_hex(0xFFD24A), 0);
    lv_obj_remove_flag(coin_label, LV_OBJ_FLAG_CLICKABLE);

    /* 4. 作物选择条（仅播种工具选中时显示） */
    seed_bar = frame_obj(farm, 92, 106, 176, 34, 0x1A1208, 17, 170);
    const char *names[2] = { "胡萝卜", "紫萝卜" };
    for(int i = 0; i < 2; i++) {
        seed_btn[i] = frame_obj(seed_bar, 4 + i * 86, 3, 82, 28, 0x3B2E1E, 14, 130);
        lv_obj_add_flag(seed_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(seed_btn[i], badge_ui_click_guard, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(seed_btn[i], seed_event, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *im = lv_image_create(seed_btn[i]);
        lv_image_set_src(im, pf_crops[i == 0 ? 3 : 7]);
        lv_image_set_scale(im, 110);             /* 48 -> 20px */
        lv_obj_set_pos(im, 4, 4);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lb = lv_label_create(seed_btn[i]);
        lv_label_set_text(lb, names[i]);
        lv_obj_set_pos(lb, 26, 4);
        lv_obj_set_style_text_font(lb, &font14, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(0xFFF3C4), 0);
        lv_obj_remove_flag(lb, LV_OBJ_FLAG_CLICKABLE);
        seed_mark[i] = lv_label_create(seed_btn[i]);
        lv_label_set_text(seed_mark[i], "○");
        lv_obj_set_pos(seed_mark[i], 66, 4);
        lv_obj_set_style_text_font(seed_mark[i], &font14, 0);
        lv_obj_set_style_text_color(seed_mark[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_remove_flag(seed_mark[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* 5. 底部工具行 */
    const lv_image_dsc_t *icons[3] = { pf_icons[PF_ICON_SEED],
                                       pf_icons[PF_ICON_WATER],
                                       pf_icons[PF_ICON_BASKET] };
    for(int i = 0; i < 3; i++) {
        tool_box[i] = frame_obj(farm, tool_x[i], TOOL_Y, TOOL_SIZE, TOOL_SIZE,
                                0x241B12, TOOL_SIZE / 2, 140);
        lv_obj_add_flag(tool_box[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tool_box[i], badge_ui_click_guard, LV_EVENT_ALL, NULL);
        lv_obj_add_event_cb(tool_box[i], tool_event, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)i);
        tool_icon[i] = lv_image_create(tool_box[i]);
        lv_image_set_src(tool_icon[i], icons[i]);
        lv_obj_center(tool_icon[i]);
        lv_obj_remove_flag(tool_icon[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 6. 提示条 */
    hint_label = lv_label_create(farm);
    lv_label_set_text(hint_label, "");
    lv_obj_set_pos(hint_label, 70, 144);
    lv_obj_set_width(hint_label, 220);
    lv_obj_set_style_text_font(hint_label, &font14, 0);
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0xFFF3C4), 0);
    lv_obj_set_style_bg_color(hint_label, lv_color_hex(0x1A1208), 0);
    lv_obj_set_style_bg_opa(hint_label, 170, 0);
    lv_obj_set_style_radius(hint_label, 10, 0);
    lv_obj_set_style_pad_all(hint_label, 4, 0);
    lv_obj_remove_flag(hint_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);

    /* 7. 收获闪光 */
    sparkle_img = lv_image_create(farm);
    lv_obj_remove_flag(sparkle_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sparkle_img, LV_OBJ_FLAG_HIDDEN);

    /* 8. 返回 */
    lv_obj_t *back_btn = frame_obj(farm, BACK_X, BACK_Y, 40, 40, 0x1A1208, 20, 160);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, badge_ui_click_guard, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(back_btn, back_event, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(back_btn);
    lv_label_set_text(arrow, "‹");
    lv_obj_set_style_text_font(arrow, &font24, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xFFF3C4), 0);
    lv_obj_center(arrow);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);

    tool = 0;
    hint_until = 0;
    sparkle_until = 0;
    sparkle_plot = -1;
    pulse_ms = lv_tick_get();
    { int64_t w0 = wall_now(); tick_wall = FARM_WALL_VALID(w0) ? w0 : 0; }
    tick_ms = lv_tick_get();
    for(int i = 0; i < FARM_PLOT_COUNT; i++) shown_stage[i] = 0xFF;
    refresh_all();
    showing = true;

    /* 时间提示：只在真的影响了成长时才提示，避免每进一次都报 */
    if(resume_warn)
        hint("时间未校准 · 离线期间作物暂停成长", 0xFF8A65);
    else if(offline_ok && offline_seconds >= 60)
        hint(offline_seconds >= 3600 ? "离线期间作物长大了" : "作物继续成长中", 0x9BE08A);
    else
        hint("选作物 → 播种 → 浇水 → 收获", 0xFFF3C4);

    /* 尝试 SNTP：非阻塞，成功后下次进入就能补算离线成长 */
#ifdef ESP_PLATFORM
    badge_apps_clock_sync();
#endif
    return true;
}

void pixel_farm_close(void)
{
    if(farm) {
        if(showing) store();
        lv_obj_delete(farm);    /* 整个子树一起删，避免残留子对象与句柄 */
        farm = NULL;
    }
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        crop_img[i] = wet_badge[i] = ripe_ring[i] = plot_hit[i] = NULL;
    }
    for(int i = 0; i < 3; i++) { tool_box[i] = tool_icon[i] = NULL; }
    seed_bar = seed_btn[0] = seed_btn[1] = seed_mark[0] = seed_mark[1] = NULL;
    coin_label = hint_label = sparkle_img = NULL;
    hint_until = sparkle_until = 0;
    sparkle_plot = -1;
    showing = false;
    /* 下次进入必须重新读存档并补算离线时间，否则退出期间的成长会丢 */
    loaded = false;
}

void pixel_farm_tick(uint32_t now_ms, bool visible)
{
    if(!farm || !showing) return;
    (void)visible;

    /* 时间推进优先用可信的 UTC 秒：这样息屏/切后台/被抢占漏掉的 tick 也能补回来。
     * 没有可信时间时退回单调时钟（unsigned 差值天然处理 49 天回绕）。 */
    int64_t w = wall_now();
    if(FARM_WALL_VALID(w)) {
        if(FARM_WALL_VALID(tick_wall) && w > tick_wall) {
            int64_t d = w - tick_wall;
            if(d > (int64_t)FARM_MAX_OFFLINE_SECONDS) d = FARM_MAX_OFFLINE_SECONDS;
            advance((uint32_t)d);
        }
        tick_wall = w;
        tick_ms = now_ms;
    } else {
        if(!tick_ms) tick_ms = now_ms;
        uint32_t delta = now_ms - tick_ms;
        if(delta >= 1000u) {
            advance(delta / 1000u);
            tick_ms += (delta / 1000u) * 1000u;
        }
        tick_wall = 0;
    }

    /* 阶段变化才动 UI */
    for(int i = 0; i < FARM_PLOT_COUNT; i++) {
        if(stage_of(&plots[i]) != shown_stage[i]) refresh_plot(i);
    }

    /* 成熟提示：作物轻微上下浮动，纯 set_pos，不影响触摸 */
    if(now_ms - pulse_ms >= 480u) {
        pulse_ms = now_ms;
        static bool up;
        up = !up;
        for(int i = 0; i < FARM_PLOT_COUNT; i++) {
            if(plots[i].crop == FARM_CROP_NONE) continue;
            lv_obj_set_y(crop_img[i], crop_y[i] + (is_ripe(&plots[i])
                                                   ? (up ? -2 : 0) : 0));
        }
    }

    /* 闪光 4 帧，约 420ms */
    if(sparkle_until) {
        if(now_ms >= sparkle_until) {
            sparkle_until = 0;
            sparkle_plot = -1;
            lv_obj_add_flag(sparkle_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            uint32_t age = 420u - (sparkle_until - now_ms);
            int f = (int)(age * 4u / 420u);
            if(f > 3) f = 3;
            lv_image_set_src(sparkle_img, pf_sparkle[f]);
        }
    }

    if(hint_until && now_ms >= hint_until) {
        hint_until = 0;
        lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* 只在有变化时落盘：每 60 秒兜底一次，避免每秒写 Flash */
    if(dirty && now_ms - last_store_ms >= 60000u) store();
}
