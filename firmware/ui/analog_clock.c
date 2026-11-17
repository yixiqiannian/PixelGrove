#include "analog_clock.h"
#include <time.h>

/* 表盘中心与半径（360x360 圆屏） */
#define CX 180
#define CY 180
#define R  138

static lv_obj_t *hour_hand, *minute_hand, *second_hand;
static lv_timer_t *tick_timer;

/* 一个以表盘中心为旋转轴的指针矩形：初始朝 12 点方向，尾部在圆心 */
static lv_obj_t *hand(lv_obj_t *parent, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, CX - w / 2, CY - h);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return o;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    struct tm tm;
    time_t now = time(NULL);
    if(now < 1700000000) {              /* SNTP 未对时：用经典展示姿势 10:09:36 */
        tm.tm_hour = 10; tm.tm_min = 9; tm.tm_sec = 36;
    } else {
        now += 8 * 3600;                 /* UTC+8 北京时间 */
        gmtime_r(&now, &tm);
    }
    int h = tm.tm_hour % 12, m = tm.tm_min, s = tm.tm_sec;
    /* transform_rotation 单位 0.1 度：秒 6°/s、分 6°/min+0.1°/s、时 30°/h+0.5°/min */
    lv_obj_set_style_transform_rotation(hour_hand,   h * 300 + m * 5, 0);
    lv_obj_set_style_transform_rotation(minute_hand, m * 60 + s, 0);
    lv_obj_set_style_transform_rotation(second_hand, s * 60, 0);
}

void analog_clock_open(lv_obj_t *panel)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    /* 表盘圆框 */
    lv_obj_t *face = lv_obj_create(panel);
    lv_obj_remove_style_all(face);
    lv_obj_set_pos(face, CX - R, CY - R);
    lv_obj_set_size(face, R * 2, R * 2);
    lv_obj_set_style_radius(face, R, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_border_opa(face, LV_OPA_COVER, 0);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 12 个小时刻度，整点刻度更长 */
    for(int i = 0; i < 12; i++) {
        int main = (i % 3 == 0);
        int len = main ? 20 : 12;
        int w   = main ? 4 : 2;
        lv_obj_t *tk = lv_obj_create(panel);
        lv_obj_remove_style_all(tk);
        lv_obj_set_pos(tk, CX - w / 2, CY - (R - 6) - len);
        lv_obj_set_size(tk, w, len);
        lv_obj_set_style_bg_color(tk, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(tk, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tk, w / 2, 0);
        lv_obj_set_style_transform_pivot_x(tk, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(tk, (R - 6) + len, 0);
        lv_obj_set_style_transform_rotation(tk, i * 300, 0);
        lv_obj_remove_flag(tk, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    /* 指针：时针 / 分针（白）/ 秒针（橙） */
    hour_hand   = hand(panel, 8,  72,  0xFFFFFF);
    minute_hand = hand(panel, 6, 112,  0xFFFFFF);
    second_hand = hand(panel, 3, 126,  0xFF9500);

    /* 中心点 */
    lv_obj_t *dot = lv_obj_create(panel);
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, CX - 6, CY - 6);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF9500), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, 6, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    tick(NULL);
    tick_timer = lv_timer_create(tick, 1000, NULL);
}

void analog_clock_close(void)
{
    if(tick_timer) { lv_timer_delete(tick_timer); tick_timer = NULL; }
}
