#include "terminal.h"
#include "avatar_renderer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

volatile uint32_t xy_display_frames;
volatile uint32_t xy_display_decode_us,xy_display_transfer_us;
typedef struct {int from,to;unsigned frames,max_gap_us,max_render_us,max_transfer_us;int64_t first_end,last_end;} transition_t;
static void report_transition(const transition_t *t){
    if(t->frames<2)return;
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.animation");
    cJSON_AddNumberToObject(e,"from",t->from);cJSON_AddNumberToObject(e,"to",t->to);
    cJSON_AddNumberToObject(e,"frames",t->frames);
    cJSON_AddNumberToObject(e,"fps",(t->frames-1)*1000000.0/(t->last_end-t->first_end));
    cJSON_AddNumberToObject(e,"max_frame_gap_ms",t->max_gap_us/1000.0);
    cJSON_AddNumberToObject(e,"max_render_ms",t->max_render_us/1000.0);
    cJSON_AddNumberToObject(e,"max_transfer_ms",t->max_transfer_us/1000.0);transport_send(e);
}
static void display_task(void *arg){
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"assets");
    assert(p);
    const uint8_t *asset=NULL;esp_partition_mmap_handle_t mapping;
    ESP_ERROR_CHECK(esp_partition_mmap(p,0,p->size,ESP_PARTITION_MMAP_DATA,(const void**)&asset,&mapping));
    void *renderer=heap_caps_malloc(avatar_renderer_size(),MALLOC_CAP_INTERNAL);
    uint16_t *background=heap_caps_malloc(360*360*2,MALLOC_CAP_SPIRAM);
    uint16_t *frame=heap_caps_malloc(360*360*2,MALLOC_CAP_SPIRAM);
    assert(renderer&&background&&frame);
    if(!avatar_init(renderer,asset,p->size,background)){
        terminal_status("error","vector_avatar_missing");vTaskDelete(NULL);return;
    }
    int64_t animation_start=esp_timer_get_time();TickType_t last=xTaskGetTickCount();
    int previous=-1;transition_t transition={0};bool measuring=false;
    while(1){
        int64_t begin=esp_timer_get_time();
        unsigned pose=xy_ptt_pressed?1:xy_speaking?3:(xy_state==XY_THINKING||xy_state==XY_CONNECTING)?2:0;
        if((int)pose!=previous){
            if(measuring)report_transition(&transition);
            transition=(transition_t){.from=previous,.to=pose};previous=pose;measuring=true;
        }
        avatar_render(renderer,frame,(begin-animation_start)/1000,pose,playback_level());
        xy_display_decode_us=esp_timer_get_time()-begin;
        display_wait_te();begin=esp_timer_get_time();
        // Send one immutable frame at vertical blank, without separately scheduled stripes.
        display_present(frame);
        xy_display_frames++;xy_display_transfer_us=esp_timer_get_time()-begin;
        if(measuring){
            int64_t now=esp_timer_get_time();
            if(!transition.frames)transition.first_end=now;
            else if(now-transition.last_end>transition.max_gap_us)transition.max_gap_us=now-transition.last_end;
            transition.last_end=now;transition.frames++;
            if(xy_display_decode_us>transition.max_render_us)transition.max_render_us=xy_display_decode_us;
            if(xy_display_transfer_us>transition.max_transfer_us)transition.max_transfer_us=xy_display_transfer_us;
            if(transition.frames==12){report_transition(&transition);measuring=false;}
        }
        if(xTaskGetTickCount()-last>=pdMS_TO_TICKS(33)){last=xTaskGetTickCount();vTaskDelay(1);}
        else vTaskDelayUntil(&last,pdMS_TO_TICKS(33));
    }
}
void display_start(void){
    // Integer rendering can migrate to an available core. Float arithmetic here
    // would automatically pin this task and compete with AFE on core 0.
    xTaskCreate(display_task,"avatar",6144,NULL,4,NULL);
}
