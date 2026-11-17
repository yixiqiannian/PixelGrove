#include "ui/apps.h"
#include "wallpaper_service.h"
#include "esp_netif_sntp.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include <stdlib.h>

/* 地图瓦片下载与矿工存档服务已在精简版本中移除；这里只保留共用的 SNTP 校时通道
 * —— TimeTiles 与像素农场都依赖 badge_apps_clock_sync()。 */

/* LVGL 自带 lodepng 的自定义分配钩子（原本也住在这个文件里）：PNG 临时缓冲只进
 * PSRAM，不占内部 DMA RAM。删地图代码时这三个符号必须留下，否则 liblvgl 链接失败。 */
void *lodepng_malloc(size_t n){return n<=2100000?heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;}
void *lodepng_realloc(void *p,size_t n){return n<=2100000?heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;}
void lodepng_free(void *p){free(p);}

static bool clock_available;
static atomic_bool clock_started;

bool badge_apps_clock_sync(void){
    char ip[24];wifi_wallpaper_address(ip,sizeof(ip));
    if(!clock_available||!*ip)return false;
    if(!atomic_exchange(&clock_started,true)&&esp_netif_sntp_start()!=ESP_OK){atomic_store(&clock_started,false);return false;}
    return true;
}
void app_service_init(void){
    esp_sntp_config_t clock_config=ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");clock_config.start=false;
    clock_available=esp_netif_sntp_init(&clock_config)==ESP_OK;
}
/* badge_ui.c / ui_transition_cache.c 以 extern 引用，恒为 false（无地图任务）。 */
bool app_service_map_busy(void){return false;}
void app_service_poll(void){}
