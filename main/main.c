#include "terminal.h"
#include "ptt_capture.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_timer.h"

volatile xy_state_t xy_state=XY_SLEEP;
volatile bool xy_ptt_pressed;
static volatile bool starting=false;
static int log_usb(const char *fmt,va_list args) {
    char line[640];int n=vsnprintf(line,sizeof(line),fmt,args);
    terminal_status("log",line);return n;
}
void terminal_status(const char *kind,const char *message) {
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.status");
    cJSON_AddStringToObject(e,"kind",kind);cJSON_AddStringToObject(e,"message",message);transport_send(e);
}
void terminal_wake(const char *source) {
    if(xy_state!=XY_SLEEP || starting || playback_prompt_busy())return;
    starting=true;xy_state=XY_CONNECTING;
    // Acknowledge locally before starting USB/Wi-Fi/cloud negotiation.
    playback_prompt(XY_PROMPT_WAKE);
    transport_wake();terminal_status("wake",source);starting=false;
}
void terminal_sleep(const char *source) {
    if(xy_state==XY_SLEEP)return;
    xy_state=XY_SLEEP;xy_ptt_pressed=false;
    playback_clear();
    if(!strcmp(source,"spoken_exit") || !strcmp(source,"button"))playback_prompt(XY_PROMPT_SLEEP);
    else playback_prompt_cancel();
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","session.stop");transport_send(e);
    transport_idle();terminal_status("standby",source);
}
void terminal_receive(const char *json,int length) {
    cJSON *e=cJSON_ParseWithLength(json,length);if(!e)return;
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(e,"type"));if(!type)goto done;
    if(!strcmp(type,"device.wake"))terminal_wake("host_test");
    else if(!strcmp(type,"device.standby")) {
        const char *reason=cJSON_GetStringValue(cJSON_GetObjectItem(e,"reason"));
        terminal_sleep(reason?reason:"remote_stop");
        cJSON *ack=cJSON_CreateObject();cJSON_AddStringToObject(ack,"type","device.standby.ack");
        const char *id=cJSON_GetStringValue(cJSON_GetObjectItem(e,"request_id"));
        if(id)cJSON_AddStringToObject(ack,"request_id",id);
        transport_send(ack);
    }
    else if(!strcmp(type,"audio.clear"))playback_event(e);
    else if(xy_state!=XY_SLEEP) {
        if(!strcmp(type,"ready"))xy_state=XY_LISTENING;
        else if(!strncmp(type,"audio.",6))playback_event(e);
        else if(!strcmp(type,"state")) {
            const char *s=cJSON_GetStringValue(cJSON_GetObjectItem(e,"state"));
            if(s && (!strcmp(s,"thinking")||!strcmp(s,"working")))xy_state=XY_THINKING;
            else if(s && !strcmp(s,"listening"))xy_state=XY_LISTENING;
            else if(s && !strcmp(s,"disconnected"))terminal_sleep("disconnected");
        } else if(!strcmp(type,"session.idle"))terminal_sleep("session_idle");
        else if(!strcmp(type,"error")) {
            const char *code=cJSON_GetStringValue(cJSON_GetObjectItem(e,"code"));
            terminal_status("server_error",code?code:"error");
        }
    }
done:cJSON_Delete(e);
}
void terminal_button(bool down) {
    bool pressed=down&&xy_state!=XY_SLEEP&&!playback_prompt_busy();
    if(pressed==xy_ptt_pressed)return;
    xy_ptt_pressed=pressed;
    if(pressed) {playback_clear();xy_state=XY_LISTENING;}
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.capture");
    cJSON_AddStringToObject(e,"state",pressed?"pressed":"released");
    cJSON_AddNumberToObject(e,"uptime_ms",esp_timer_get_time()/1000);transport_send(e);
}
static void button_task(void *arg) {
    ptt_button_t button={0};
    while(1) {
        terminal_button(ptt_button_update(&button,gpio_get_level(0)==0,(uint32_t)(esp_timer_get_time()/1000)));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    transport_start();terminal_status("boot","xiaomeng-v0.4.0 wake-then-BOOT-PTT V2 IDF6.1");
    esp_log_set_vprintf(log_usb);
    board_init();display_start();speech_start();
    xTaskCreate(button_task,"talk_button",3072,NULL,6,NULL);
    terminal_status("ready","local_multinet_ni_hao_xiao_meng");
    unsigned tick=0;
    while(1) {
        playback_progress();
        if(++tick%50==0) {
            cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.telemetry");
            cJSON_AddNumberToObject(e,"state",xy_state);cJSON_AddBoolToObject(e,"playing",xy_speaking);
            cJSON_AddBoolToObject(e,"ptt_pressed",xy_ptt_pressed);
            cJSON_AddNumberToObject(e,"uptime_ms",esp_timer_get_time()/1000);
            cJSON_AddNumberToObject(e,"free_internal",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(e,"free_psram",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            cJSON_AddNumberToObject(e,"display_frames",xy_display_frames);playback_diagnostics(e);speech_diagnostics(e);transport_send(e);
            cJSON *perf=cJSON_CreateObject();cJSON_AddStringToObject(perf,"type","device.display");
            cJSON_AddNumberToObject(perf,"decode_us",xy_display_decode_us);cJSON_AddNumberToObject(perf,"transfer_us",xy_display_transfer_us);
            cJSON_AddNumberToObject(perf,"te_count",xy_te_count);cJSON_AddNumberToObject(perf,"te_timeouts",xy_te_timeouts);transport_send(perf);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
