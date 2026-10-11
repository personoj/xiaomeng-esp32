/* Host-only RTOS/driver substitutions for exercising the real C playback/state code. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cJSON.h"
#include "../main/ptt_capture.h"
extern volatile bool xy_ptt_pressed;
typedef void *i2s_chan_handle_t;
typedef void *SemaphoreHandle_t;
typedef int portMUX_TYPE;
typedef int xy_prompt_t;
enum { XY_PROMPT_WAKE, XY_PROMPT_SLEEP };
typedef enum { XY_SLEEP, XY_CONNECTING, XY_LISTENING, XY_THINKING } xy_state_t;
typedef struct { void *dma_buf; size_t size; } i2s_event_data_t;
typedef struct { bool (*on_sent)(i2s_chan_handle_t,i2s_event_data_t*,void*); } i2s_event_callbacks_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define portENTER_CRITICAL_ISR(x) ((void)0)
#define portEXIT_CRITICAL_ISR(x) ((void)0)
#define portMAX_DELAY 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 1
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define pdMS_TO_TICKS(x) (x)
static int64_t test_us=1000000;
static int64_t esp_timer_get_time(void){return test_us;}
static void *heap_caps_malloc(size_t n,int caps){return malloc(n);}
static int heap_caps_get_free_size(int caps){return 1000000;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
static void xSemaphoreTake(void *s,int timeout){}
static void xSemaphoreGive(void *s){}
static int i2s_channel_register_event_callback(void *c,const i2s_event_callbacks_t *cb,void *ctx){return 0;}
static size_t strlcpy(char *dst,const char *src,size_t n){size_t len=strlen(src);if(n){size_t copy=len<n-1?len:n-1;memcpy(dst,src,copy);dst[copy]=0;}return len;}
static int mbedtls_base64_decode(uint8_t *dst,size_t cap,size_t *len,const uint8_t *src,size_t size){return -1;}
static int nvs_flash_init(void){return 0;}
static void esp_log_set_vprintf(void *p){}
static int gpio_get_level(int pin){return 1;}
static void vTaskDelay(int ticks){}
static void xTaskCreate(void (*task)(void*),const char *name,int stack,void *arg,int priority,void *handle){}
static void board_init(void){}
static void display_start(void){}
static void speech_start(void){}
static void speech_diagnostics(cJSON *e){}
static void transport_start(void){}
static unsigned wakes,sleeps,prompt_starts,prompt_completions,cloud_drains,audio_acks;
static void transport_wake(void){wakes++;}
static void transport_idle(void){sleeps++;}
static void transport_send(cJSON *e){
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(e,"type"));
    if(!strcmp(type,"device.prompt")){
        const char *state=cJSON_GetStringValue(cJSON_GetObjectItem(e,"state"));
        if(!strcmp(state,"completed"))prompt_completions++;else prompt_starts++;
    }
    if(!strcmp(type,"playback.progress")&&cJSON_IsTrue(cJSON_GetObjectItem(e,"drained")))cloud_drains++;
    if(!strcmp(type,"device.audio_ack"))audio_acks++;
    cJSON_Delete(e);
}
static void terminal_status(const char *kind,const char *message);
static i2s_chan_handle_t xy_tx;
static volatile uint32_t xy_display_frames,xy_display_decode_us,xy_display_transfer_us;
static volatile uint32_t xy_te_count,xy_te_timeouts;
/* playback_init is link-checked; runtime tests initialize deterministic fixture PCM. */
const uint8_t wake_pcm_start[]={0,0},wake_pcm_end[]={0,0},sleep_pcm_start[]={0,0},sleep_pcm_end[]={0,0};
