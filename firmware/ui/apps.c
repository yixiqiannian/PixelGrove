#include "apps.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "ui_transition_cache.h"
extern void badge_panel_cancel(lv_obj_t*);
#include "timetiles.h"
#include "pixel_farm.h"
#include "fluidbox.h"   /* 只用来声明下面的 stub：FluidBox 应用已移除，但系统代码仍在调这些接口 */
#include "weather.h"
#include "motion.h"
#include "src/misc/cache/lv_cache.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { APP_LIST,APP_TIMETILES,APP_PIXEL_FARM,APP_WEATHER };
static lv_obj_t *panel,*parent;
static int screen;
static bool visible;
static void show_list(void);
static lv_obj_t *box(lv_obj_t *p,int x,int y,int w,int h,uint32_t color,int radius){
    lv_obj_t *o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,255,0);lv_obj_set_style_radius(o,radius,0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);return o;
}
static lv_obj_t *label(lv_obj_t *p,const char *text,int x,int y,int w,const lv_font_t *font,uint32_t color){
    lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,text);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
static lv_obj_t *button(lv_obj_t *p,const char *text,int x,int y,int w,int h,lv_event_cb_t cb,int id){
    lv_obj_t *o=box(p,x,y,w,h,UI_SURFACE,14);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o,lv_color_hex(UI_SELECTED),LV_STATE_PRESSED);
    lv_obj_t *l=label(o,text,0,0,w,&font18,UI_TEXT);lv_obj_center(l);
    lv_obj_add_event_cb(o,badge_ui_click_guard,LV_EVENT_ALL,NULL);lv_obj_add_event_cb(o,cb,LV_EVENT_SHORT_CLICKED,(void*)(intptr_t)id);return o;
}
static void clear_panel(int mode){
    ui_transition_cache_forget(panel);
    bool dynamic=mode==APP_TIMETILES||mode==APP_PIXEL_FARM||mode==APP_WEATHER;
    ui_transition_cache_pause(dynamic);
    if(dynamic)ui_memory_pressure_set(UI_MEMORY_HIGH);
    timetiles_close();pixel_farm_close();weather_close();
    lv_obj_clean(panel);
    screen=mode;
    lv_obj_set_style_bg_color(panel,lv_color_hex(UI_BG),0);
}
static void back(lv_event_t *e){(void)e;if(screen==APP_LIST){visible=false;badge_ui_external_panel(panel,false);return;}show_list();}
static void heading(const char *text){button(panel,"‹",80,29,44,44,back,0);label(panel,text,123,34,142,&font24,UI_TEXT);}
static void show_timetiles(void){
    clear_panel(APP_TIMETILES);
    if(!timetiles_open(panel,show_list)){heading("TimeTiles");label(panel,"Out of memory",80,150,200,&font18,UI_TEXT);}
}
/* 像素农场自带全屏背景与返回键，不需要 heading()。 */
static void show_pixel_farm(void){
    clear_panel(APP_PIXEL_FARM);
    if(!pixel_farm_open(panel,show_list)){
        heading("像素农场");
        label(panel,"Out of memory",80,150,200,&font18,UI_TEXT);
    }
}
/* 像素天气同样自带全屏背景与返回/刷新键。 */
static void show_weather(void){
    clear_panel(APP_WEATHER);
    lv_obj_set_style_bg_color(panel,lv_color_hex(0x101820),0);
    if(!weather_open(panel,show_list)){
        heading("像素天气");
        label(panel,"Out of memory",80,150,200,&font18,UI_TEXT);
    }
}
static void choose(lv_event_t *e){int id=(int)(intptr_t)lv_event_get_user_data(e);if(id==5)show_timetiles();else if(id==7)show_pixel_farm();else show_weather();}
static void show_list(void){
    /* 3 个应用：两行布局，末行居中。id 沿用原编号，避免 badge_apps_launch 错位。 */
    clear_panel(APP_LIST);heading("应用");
    button(panel,"TimeTiles",58,100,120,52,choose,5);
    button(panel,"像素农场",182,100,120,52,choose,7);
    button(panel,"像素天气",120,166,120,52,choose,8);
    ui_transition_cache_request(panel);
}
static void tick(lv_timer_t *timer){
    (void)timer;uint32_t now=lv_tick_get();
    timetiles_poll(visible&&screen==APP_TIMETILES&&!badge_ui_is_asleep());
    if(!visible||badge_ui_is_asleep())return;
    if(screen==APP_PIXEL_FARM)pixel_farm_tick(now,true);
    if(screen==APP_WEATHER)weather_tick(now,true);
}
void badge_apps_create(lv_obj_t *p){parent=p;panel=box(parent,0,0,360,360,UI_BG,0);ui_transition_cache_register(panel,false);ui_transition_cache_identify(panel,33);show_list();lv_obj_add_flag(panel,LV_OBJ_FLAG_HIDDEN);lv_timer_create(tick,33,NULL);}
void badge_apps_open(void){
    if(!panel){panel=box(parent,0,0,360,360,UI_BG,0);ui_transition_cache_register(panel,false);ui_transition_cache_identify(panel,33);}
    visible=true;if(screen!=APP_LIST)show_list();badge_ui_external_panel(panel,true);
}
void badge_apps_hide(void){
    if(!visible){badge_panel_cancel(panel);return;}
    if(screen==APP_LIST){visible=false;badge_panel_cancel(panel);return;}
    visible=false;ui_transition_cache_pause(false);timetiles_close();pixel_farm_close();weather_close();
    badge_panel_cancel(panel);ui_transition_cache_forget(panel);lv_obj_clean(panel);
    show_list();
}
void badge_apps_launch(int app){badge_apps_open();if(app==9)show_weather();if(app==8)show_pixel_farm();if(app==6)show_timetiles();}
void badge_apps_motion(float x,float y,bool shake,bool available){
    /* Grok 已移除，保留空实现，避免 main.c 的调用点失效。 */
    (void)x;(void)y;(void)shake;(void)available;
}
void badge_apps_status(int *page,int *loading,int *error,int *tiles,int *count){*page=visible?screen:-1;*loading=0;*error=0;*tiles=0;*count=0;}

void badge_apps_probe(bool open){if(open)badge_apps_open();else {visible=false;badge_ui_external_panel(panel,false);}}

int badge_apps_cache_page(void){return visible&&screen==APP_LIST?33:-1;}

/* ---- 已移除应用的接口 stub ----
 * FluidBox 应用已从构建中移除，但 main.c（USB 命令分发）、management.c（状态 JSON 上报）、
 * motion_service.c（姿态数据转发）仍在调用这些接口。这里留空实现，保持 USB 管理与姿态
 * 通道不崩。fluidbox_info 必须返回合法 JSON —— management.c 会直接 cJSON_Parse 它。 */
void fluidbox_motion(const int16_t xyz[3], bool available){(void)xyz;(void)available;}
void fluidbox_mode(int mode){(void)mode;}
void fluidbox_reset(void){}
void fluidbox_info(char *out, size_t size){if(out&&size)snprintf(out,size,"{\"removed\":true}");}
