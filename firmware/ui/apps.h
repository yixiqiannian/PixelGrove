#ifndef BADGE_APPS_H
#define BADGE_APPS_H
#include "badge_ui.h"
void badge_apps_create(lv_obj_t *parent);
void badge_apps_open(void);
void badge_apps_hide(void);
void badge_apps_launch(int app); /* 0 list, 3 Grok, 5 FluidBox, 6 TimeTiles, 7 analog clock, 8 pixel farm, 9 weather */
bool badge_apps_clock_sync(void); /* Shared nonblocking SNTP start (TimeTiles, pixel farm). */
void badge_apps_motion(float x,float y,bool shake,bool available);
void badge_apps_status(int *page,int *loading,int *error,int *tiles,int *players);
void app_service_init(void);
void app_service_poll(void);
#endif
