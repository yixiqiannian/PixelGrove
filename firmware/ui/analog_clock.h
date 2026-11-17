#ifndef BADGE_ANALOG_CLOCK_H
#define BADGE_ANALOG_CLOCK_H
#include "lvgl.h"

/* 模拟时钟（指针表盘）。open 在 panel 上创建表盘并启动每秒刷新，close 停表。 */
void analog_clock_open(lv_obj_t *panel);
void analog_clock_close(void);

#endif
