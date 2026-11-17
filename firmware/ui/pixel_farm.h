#ifndef BADGE_PIXEL_FARM_H
#define BADGE_PIXEL_FARM_H

#include "badge_ui.h"

/* ---- 规则集中配置：改这里即可调时长与收益（便于测试） ---- */
#define FARM_CARROT_GROW_SECONDS 180   /* 胡萝卜 3 分钟 */
#define FARM_TURNIP_GROW_SECONDS 300   /* 紫萝卜 5 分钟 */
#define FARM_CARROT_COINS        5     /* 胡萝卜收获收益 */
#define FARM_TURNIP_COINS        8     /* 紫萝卜收获收益 */
#define FARM_WATER_REQUIRED      1     /* 1 = 播种后必须浇水才开始成长 */
#define FARM_MAX_OFFLINE_SECONDS 604800u /* 断电最多补算 7 天，防溢出 */

#define FARM_PLOT_COUNT 6
#define FARM_CROP_NONE   0
#define FARM_CROP_CARROT 1
#define FARM_CROP_TURNIP 2

/* 图标索引（对应 pf_icons[]） */
enum { PF_ICON_SEED, PF_ICON_WATER, PF_ICON_BASKET, PF_ICON_COIN,
       PF_ICON_SOIL_WET, PF_ICON_SOIL_DRY };

/* 资源（pixel_farm_assets.c 由 generate_pixel_farm_assets.py 生成） */
extern const lv_image_dsc_t pf_bg;
extern const lv_image_dsc_t *const pf_crops[8];
extern const lv_image_dsc_t *const pf_icons[6];
extern const lv_image_dsc_t *const pf_sparkle[4];

/* 进入 / 退出 / 每帧驱动。close 会落盘并释放定时器，可重复调用。 */
bool pixel_farm_open(lv_obj_t *panel, void (*back)(void));
void pixel_farm_close(void);
void pixel_farm_tick(uint32_t now_ms, bool visible);

#endif
