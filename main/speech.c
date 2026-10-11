#include "terminal.h"
#include "ptt_capture.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static const esp_mn_iface_t *mn;
static model_iface_data_t *mn_data;
static uint32_t raw_level[4],clean_level,mn_us,fetch_count;
static uint32_t vad_frames,mn_frames,mn_timeouts,mn_detections;
static int16_t *command;
static int command_size,command_used;
static bool vad_active;
static void report_recognition(const char *phase) {
    esp_mn_results_t *result=mn->get_results(mn_data);
    if(!result)return;
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"type","device.recognition_debug");
    cJSON_AddStringToObject(event,"phase",phase);
    cJSON_AddNumberToObject(event,"matches",result->num);
    cJSON_AddStringToObject(event,"text",result->string);
    cJSON_AddStringToObject(event,"raw_text",result->raw_string);
    if(result->num>0) {
        cJSON_AddNumberToObject(event,"command_id",result->command_id[0]);
        cJSON_AddNumberToObject(event,"probability",result->prob[0]);
    }
    cJSON_AddNumberToObject(event,"uptime_ms",esp_timer_get_time()/1000);
    transport_send(event);
}
void speech_diagnostics(cJSON *e) {
    cJSON *a=cJSON_AddArrayToObject(e,"raw_levels");
    for(int i=0;i<4;i++)cJSON_AddItemToArray(a,cJSON_CreateNumber(raw_level[i]));
    cJSON_AddNumberToObject(e,"clean_level",clean_level);cJSON_AddBoolToObject(e,"vad",vad_active);
    cJSON_AddNumberToObject(e,"mn_us",mn_us);cJSON_AddNumberToObject(e,"afe_frames",fetch_count);
    cJSON_AddNumberToObject(e,"vad_frames",vad_frames);cJSON_AddNumberToObject(e,"mn_frames",mn_frames);
    cJSON_AddNumberToObject(e,"mn_timeouts",mn_timeouts);cJSON_AddNumberToObject(e,"mn_detections",mn_detections);
}
static void recognize(const int16_t *pcm,int samples) {
    for(int i=0;i<samples && xy_state==XY_SLEEP;i++) {
        command[command_used++]=pcm[i];
        if(command_used==command_size) {
            int64_t before=esp_timer_get_time();
            esp_mn_state_t state=mn->detect(mn_data,command);command_used=0;
            mn_us=esp_timer_get_time()-before;mn_frames++;
            if(state==ESP_MN_STATE_DETECTED) {
                mn_detections++;
                esp_mn_results_t *result=mn->get_results(mn_data);
                cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"type","device.recognition");
                cJSON_AddNumberToObject(event,"matches",result->num);
                if(result->num>0) {
                    cJSON_AddNumberToObject(event,"command_id",result->command_id[0]);
                    cJSON_AddNumberToObject(event,"probability",result->prob[0]);
                }
                transport_send(event);
                if(result->num>0&&result->command_id[0]==1)terminal_wake("ni_hao_xiao_meng");
                mn->clean(mn_data);
            } else if(state==ESP_MN_STATE_TIMEOUT){mn_timeouts++;report_recognition("timeout");mn->clean(mn_data);}
        }
    }
}
static void feed_task(void *arg) {
    int bytes=afe->get_feed_chunksize(afe_data)*4*sizeof(int16_t);
    int16_t *raw=heap_caps_malloc(bytes,MALLOC_CAP_INTERNAL);assert(raw);
    while(1) {
        size_t read=0,part=0;
        while(read<bytes) {ESP_ERROR_CHECK(i2s_channel_read(xy_rx,(uint8_t*)raw+read,bytes-read,&part,1000));read+=part;}
        uint32_t sum[4]={0};for(int i=0;i<bytes/2;i++)sum[i%4]+=abs(raw[i]);
        for(int i=0;i<4;i++)raw_level[i]=sum[i]/(bytes/8);
        afe->feed(afe_data,raw);
    }
}
static void ptt_event(void *context,bool begin,unsigned samples) {
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type",begin?"input.begin":"input.commit");transport_send(e);
    if(!begin)xy_state=XY_THINKING;
    e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.capture");
    cJSON_AddStringToObject(e,"state",begin?"stream_started":"submitted");
    cJSON_AddNumberToObject(e,"samples",samples);
    cJSON_AddNumberToObject(e,"uptime_ms",esp_timer_get_time()/1000);transport_send(e);
}
static void ptt_audio(void *context,const int16_t *pcm,unsigned samples) {transport_pcm(pcm,samples);}
static void fetch_task(void *arg) {
    command_size=mn->get_samp_chunksize(mn_data);
    command=heap_caps_malloc(command_size*sizeof(int16_t),MALLOC_CAP_INTERNAL);
    int16_t *waiting=heap_caps_malloc(128000*sizeof(int16_t),MALLOC_CAP_SPIRAM);
    assert(command&&waiting);
    ptt_capture_t capture={.pending=waiting,.capacity=128000,.event=ptt_event,.audio=ptt_audio};
    bool recognizing=false,wake_vad=false;
    while(1) {
        afe_fetch_result_t *r=afe->fetch(afe_data);if(!r||r->ret_value==ESP_FAIL)continue;
        int samples=r->data_size/2;
        uint32_t sum=0;for(int i=0;i<samples;i++)sum+=abs(r->data[i]);clean_level=sum/samples;
        fetch_count++;vad_active=r->vad_state==VAD_SPEECH;
        if(vad_active)vad_frames++;
        // Keep feeding AEC, but do not upload or recognize our fixed local cues.
        // Clear pre-connection audio too, so the acknowledgement never becomes
        // a user turn when the cloud connection finishes.
        if(playback_prompt_busy()) {ptt_reset(&capture);recognizing=false;command_used=0;continue;}
        if(xy_state==XY_SLEEP) {
            // Feed a continuous stream: a VAD gate can discard the leading syllable
            // of a short wake phrase, or reset recognition across a brief pause.
            if(!recognizing){mn->clean(mn_data);command_used=0;recognizing=true;terminal_status("wake_armed","ni_hao_xiao_meng");}
            recognize(r->data,samples);
            if(wake_vad&&!vad_active)report_recognition("speech_end");
            wake_vad=vad_active;
        } else {recognizing=false;command_used=0;}
        if(xy_state==XY_SLEEP) {ptt_reset(&capture);continue;}
        // Only a physical hold can contribute audio. AFE/VAD still run locally,
        // but neither residual echo nor VAD can start or commit a cloud turn.
        unsigned result=ptt_process(&capture,xy_ptt_pressed,transport_ready(),r->data,samples);
        if(result&PTT_ABORTED)terminal_status("capture_cancelled","connection_or_buffer_limit_press_again");
    }
}
void speech_start(void) {
    srmodel_list_t *models=esp_srmodel_init("model");assert(models);
    char *name=esp_srmodel_filter(models,ESP_MN_PREFIX,ESP_MN_CHINESE);assert(name);
    mn=esp_mn_handle_from_name(name);mn_data=mn->create(name,6000);assert(mn_data);
    mn->set_det_threshold(mn_data,0.20f);
    ESP_ERROR_CHECK(esp_mn_commands_alloc(mn,mn_data));
    ESP_ERROR_CHECK(esp_mn_commands_add(1,"ni hao xiao meng"));
    esp_mn_error_t *command_error=esp_mn_commands_update();assert(command_error==NULL);
    mn->print_active_speech_commands(mn_data);
    ESP_LOGI("speech","continuous wake model=%s chunk=%d rate=%d threshold=0.20",name,mn->get_samp_chunksize(mn_data),mn->get_samp_rate(mn_data));
    // Keep ADC's four-slot layout; use the first microphone + physical reference.
    // A single-mic FD front-end leaves deterministic CPU time for audio and LCD.
    afe_config_t *cfg=afe_config_init("RMNN",models,AFE_TYPE_FD,AFE_MODE_LOW_COST);
    cfg->wakenet_init=false;cfg->aec_init=true;cfg->vad_init=true;
    cfg->aec_mode=AEC_MODE_FD_LOW_COST;cfg->aec_nlp_level=AEC_NLP_LEVEL_VERYAGGR;
    cfg->ns_init=false;cfg->agc_init=false;cfg->afe_ringbuf_size=32;
    cfg->se_init=false;
    cfg->vad_min_speech_ms=160;cfg->vad_min_noise_ms=320;
    cfg->vad_mode=VAD_MODE_3;
    cfg->memory_alloc_mode=AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core=0;cfg->afe_perferred_priority=5;
    afe=esp_afe_handle_from_config(cfg);afe_data=afe->create_from_config(cfg);assert(afe_data);afe_config_free(cfg);
    xTaskCreatePinnedToCore(feed_task,"mic_feed",4096,NULL,7,NULL,0);
    xTaskCreatePinnedToCore(fetch_task,"speech",8192,NULL,6,NULL,1);
}
