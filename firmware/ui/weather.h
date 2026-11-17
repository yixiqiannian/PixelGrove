#ifndef BADGE_WEATHER_H
#define BADGE_WEATHER_H

#include "badge_ui.h"
#include "weather_config.h"

/* ---- 素材索引（weather_assets.c 由 generate_weather_assets.py 生成）---- */
enum { WX_ICON_SUN, WX_ICON_PARTLY, WX_ICON_CLOUD, WX_ICON_RAIN, WX_ICON_SNOW, WX_ICON_MOON };
/* 背景 8 张：0-3 四季白天，4-7 四季夜晚。顺序必须与 generate_weather_assets.py 一致。
 * 阴雨雪不再单独占一张图 —— 原阴天图里烘焙了一棵静态树，会与动画树重叠；
 * 改为「当季背景 + 灰蓝滤镜」，既保住季节感和会动的树，又省下 253 KiB。 */
enum { WX_BG_SPRING, WX_BG_SUMMER, WX_BG_AUTUMN, WX_BG_WINTER,
       WX_BG_NIGHT_SPRING, WX_BG_NIGHT_SUMMER, WX_BG_NIGHT_AUTUMN, WX_BG_NIGHT_WINTER };

extern const lv_image_dsc_t *const wx_bg[8];
extern const lv_image_dsc_t *const wx_icons[6];
extern const lv_image_dsc_t *const wx_rain[2];
extern const lv_image_dsc_t *const wx_snow[2];
/* 四季树木动画：[0-3]=白天 春/夏/秋/冬，[4-7]=夜晚 春/夏/秋/冬，每组 4 帧（64×96 透明）。 */
extern const lv_image_dsc_t *const *const wx_trees[8];

/* 进入 / 退出 / 每帧驱动。close 会停止定时器、取消在途请求并可重复调用。 */
bool weather_open(lv_obj_t *panel, void (*back)(void));
void weather_close(void);
void weather_tick(uint32_t now_ms, bool visible);

#endif
