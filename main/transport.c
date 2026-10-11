#include "terminal.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#define MAX_JSON 98304
typedef struct { char *json; bool microphone; uint32_t epoch; } outgoing_t;
static QueueHandle_t outbox;
static volatile bool ready=false,wifi_ip=false;
static volatile uint32_t epoch=0;
static bool wifi_mode=false,ws_started=false;
static esp_websocket_client_handle_t ws;
static char ssid[33],password[65],server[160],token[96];
static char *ws_message;
static int ws_used,ws_frame_offset;
static volatile TickType_t host_seen_tick;

static void usb_write(const char *text) {
    int remain=strlen(text),offset=0;
    while(remain>0) {int n=usb_serial_jtag_write_bytes(text+offset,remain,pdMS_TO_TICKS(250));if(n<=0)return;offset+=n;remain-=n;}
    usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(100));
}
void transport_usb_message(const char *json) {usb_write(json);}
bool transport_ready(void) {return ready && xy_state!=XY_SLEEP;}
void transport_send(cJSON *event) {
    if(!event)return;
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(event,"type"));
    bool microphone=type&&(!strcmp(type,"input.audio")||!strcmp(type,"input.begin")||!strcmp(type,"input.commit"));
    cJSON_AddNumberToObject(event,"device_epoch",epoch);
    outgoing_t msg={.json=cJSON_PrintUnformatted(event),.microphone=microphone,.epoch=epoch};
    cJSON_Delete(event);
    if(!msg.json)return;
    if(!outbox||xQueueSend(outbox,&msg,pdMS_TO_TICKS(microphone?40:100))!=pdTRUE)free(msg.json);
}
void transport_pcm(const int16_t *pcm,int samples) {
    for(int offset=0;offset<samples;offset+=320) {
        if(!transport_ready())return;
        int count=samples-offset;if(count>320)count=320;
        char data[860];size_t written;
        if(mbedtls_base64_encode((uint8_t*)data,sizeof(data)-1,&written,(const uint8_t*)(pcm+offset),count*2))return;
        data[written]=0;
        cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","input.audio");cJSON_AddStringToObject(e,"audio",data);transport_send(e);
    }
}
void transport_wake(void) {
    epoch++;ready=false;
    host_seen_tick=xTaskGetTickCount();
    if(!wifi_mode){cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.wake");transport_send(e);}
}
void transport_idle(void) {
    ready=false;
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.sleep");transport_send(e);
}
static void tx_task(void *arg) {
    outgoing_t msg;
    while(1) {
        if(xQueueReceive(outbox,&msg,portMAX_DELAY)!=pdTRUE)continue;
        if(msg.microphone && (!transport_ready()||msg.epoch!=epoch)) {free(msg.json);continue;}
        if(!wifi_mode)usb_write(msg.json);
        else {
            cJSON *e=cJSON_Parse(msg.json);
            const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(e,"type"));
            if(type&&strncmp(type,"device.",7) && ws && esp_websocket_client_is_connected(ws))
                esp_websocket_client_send_text(ws,msg.json,strlen(msg.json),pdMS_TO_TICKS(500));
            else if(type&&!strcmp(type,"device.standby.ack")&&ws&&esp_websocket_client_is_connected(ws))
                esp_websocket_client_send_text(ws,msg.json,strlen(msg.json),pdMS_TO_TICKS(500));
            if(type&&!strncmp(type,"device.",7))usb_write(msg.json);
            cJSON_Delete(e);
        }
        free(msg.json);
    }
}
static void dispatch(const char *data,int len,bool usb) {
    cJSON *e=cJSON_ParseWithLength(data,len);if(!e)return;
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(e,"type"));
    if(!type){cJSON_Delete(e);return;}
    if(usb)host_seen_tick=xTaskGetTickCount();
    const cJSON *version=cJSON_GetObjectItem(e,"device_epoch");
    if(cJSON_IsNumber(version)&&version->valueint!=epoch && strcmp(type,"device.wake")){cJSON_Delete(e);return;}
    if(usb&&!strcmp(type,"device.configure")) {
        const char *mode=cJSON_GetStringValue(cJSON_GetObjectItem(e,"mode"));
        const char *s=cJSON_GetStringValue(cJSON_GetObjectItem(e,"ssid"));
        const char *p=cJSON_GetStringValue(cJSON_GetObjectItem(e,"password"));
        const char *host=cJSON_GetStringValue(cJSON_GetObjectItem(e,"server"));
        const char *key=cJSON_GetStringValue(cJSON_GetObjectItem(e,"token"));
        bool valid=mode && (!strcmp(mode,"usb") || (!strcmp(mode,"wifi")&&s&&host&&key&&strlen(s)<33&&(!p||strlen(p)<65)&&strlen(host)<160&&strlen(key)<96));
        if(valid) {
            terminal_sleep("configure");nvs_handle_t nv;ESP_ERROR_CHECK(nvs_open("xiaoyun",NVS_READWRITE,&nv));
            nvs_set_u8(nv,"wifi",!strcmp(mode,"wifi"));
            if(s)nvs_set_str(nv,"ssid",s);
            if(p)nvs_set_str(nv,"password",p);
            if(host)nvs_set_str(nv,"server",host);
            if(key)nvs_set_str(nv,"token",key);
            nvs_commit(nv);nvs_close(nv);terminal_status("configured",mode);vTaskDelay(pdMS_TO_TICKS(300));esp_restart();
        } else terminal_status("error","invalid_config");
        cJSON_Delete(e);return;
    }
    if(usb&&wifi_mode){cJSON_Delete(e);return;}
    if(!strcmp(type,"ready")&&xy_state!=XY_SLEEP) {
        const char *mode=cJSON_GetStringValue(cJSON_GetObjectItem(e,"input_mode"));
        const char *turn=cJSON_GetStringValue(cJSON_GetObjectItem(e,"turn_detection"));
        ready=mode&&turn&&!strcmp(mode,"push_to_talk")&&!strcmp(turn,"manual");
        if(!ready) {terminal_status("error","backend_push_to_talk_required");terminal_sleep("input_mode_mismatch");}
    }
    if(!strcmp(type,"attached")&&wifi_mode&&xy_state!=XY_SLEEP){cJSON *start=cJSON_CreateObject();cJSON_AddStringToObject(start,"type","session.start");cJSON_AddStringToObject(start,"input_mode","push_to_talk");transport_send(start);}
    cJSON_Delete(e);terminal_receive(data,len);
}
static void usb_task(void *arg) {
    char *message=heap_caps_malloc(MAX_JSON,MALLOC_CAP_SPIRAM);assert(message);
    char buf[512];int used=0;
    while(1) {
        int n=usb_serial_jtag_read_bytes(buf,sizeof(buf),pdMS_TO_TICKS(100));
        if(!wifi_mode&&xy_state!=XY_SLEEP&&xTaskGetTickCount()-host_seen_tick>pdMS_TO_TICKS(6000))terminal_sleep("usb_disconnected");
        for(int i=0;i<n;i++) {
            if(buf[i]=='\n') {if(used)dispatch(message,used,true);used=0;}
            else if(used<MAX_JSON-1)message[used++]=buf[i];
            else used=0;
        }
    }
}
static void websocket_event(void *arg,esp_event_base_t base,int32_t id,void *event) {
    esp_websocket_event_data_t *e=event;
    if(id==WEBSOCKET_EVENT_DATA) {
        if(e->op_code!=1 && e->op_code!=0)return;
        if(e->op_code==1&&e->payload_offset==0)ws_used=0;
        if(e->payload_offset==0)ws_frame_offset=0;
        if(ws_used+e->data_len>=MAX_JSON||e->payload_offset!=ws_frame_offset){ws_used=0;return;}
        memcpy(ws_message+ws_used,e->data_ptr,e->data_len);ws_used+=e->data_len;
        ws_frame_offset+=e->data_len;
        if(e->fin&&ws_frame_offset==e->payload_len){dispatch(ws_message,ws_used,false);ws_used=0;}
    } else if(id==WEBSOCKET_EVENT_DISCONNECTED) {ready=false;if(xy_state!=XY_SLEEP)terminal_sleep("network_disconnected");}
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_START)esp_wifi_connect();
    if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){wifi_ip=false;esp_wifi_connect();}
    if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){wifi_ip=true;terminal_status("network","wifi_connected");}
}
static void network_task(void *arg) {
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());esp_netif_create_default_wifi_sta();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    wifi_config_t config={0};strlcpy((char*)config.sta.ssid,ssid,sizeof(config.sta.ssid));strlcpy((char*)config.sta.password,password,sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&config));ESP_ERROR_CHECK(esp_wifi_start());esp_wifi_set_ps(WIFI_PS_NONE);
    char uri[192],headers[128];snprintf(uri,sizeof(uri),"ws://%s/device/ws",server);snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",token);
    esp_websocket_client_config_t wc={.uri=uri,.headers=headers,.buffer_size=4096,.disable_auto_reconnect=true,.task_stack=8192,.network_timeout_ms=5000};
    ws_message=heap_caps_malloc(MAX_JSON,MALLOC_CAP_SPIRAM);assert(ws_message);
    ws=esp_websocket_client_init(&wc);esp_websocket_register_events(ws,WEBSOCKET_EVENT_ANY,websocket_event,NULL);
    while(1) {
        if(xy_state!=XY_SLEEP&&wifi_ip&&!ws_started){ws_started=true;esp_websocket_client_start(ws);}
        else if(xy_state==XY_SLEEP&&ws_started){vTaskDelay(pdMS_TO_TICKS(150));esp_websocket_client_stop(ws);ws_started=false;}
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
void transport_start(void) {
    nvs_handle_t nv;
    if(nvs_open("xiaoyun",NVS_READONLY,&nv)==ESP_OK) {
        uint8_t mode=0;nvs_get_u8(nv,"wifi",&mode);wifi_mode=mode;
        size_t n=sizeof(ssid);nvs_get_str(nv,"ssid",ssid,&n);n=sizeof(password);nvs_get_str(nv,"password",password,&n);
        n=sizeof(server);nvs_get_str(nv,"server",server,&n);n=sizeof(token);nvs_get_str(nv,"token",token,&n);nvs_close(nv);
    }
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=8192,.rx_buffer_size=32768};ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    outbox=xQueueCreate(64,sizeof(outgoing_t));
    xTaskCreate(tx_task,"transport_tx",4096,NULL,5,NULL);xTaskCreate(usb_task,"transport_rx",8192,NULL,5,NULL);
    if(wifi_mode)xTaskCreate(network_task,"network",6144,NULL,4,NULL);
}
