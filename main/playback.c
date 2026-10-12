#include "terminal.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#define NSLOTS 16
#define RING_SAMPLES 192000
#define PROMPT_SLOT (-2)
typedef struct { char id[128]; uint32_t seq, received, played, reported, total; bool done,cancelled,drained,started_reported; int16_t tail[3]; int ntail; int64_t received_us,first_play_us; } response_t;
typedef struct { int16_t value; uint8_t slot, credit; } sample_t;
typedef struct { void *ptr; size_t bytes; int slot; uint32_t credit,level; } dma_t;
static response_t responses[NSLOTS];
static dma_t dma[6];
static sample_t *ring;
static uint32_t rd,wr;
static char blocked[64][128]; static unsigned blocked_pos;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t guard;
volatile bool xy_speaking;
static volatile uint32_t level_milli;
unsigned playback_level(void){return level_milli*1024/1000;}
static volatile uint32_t dma_count;
extern const uint8_t wake_pcm_start[] asm("_binary_wake_pcm_start");
extern const uint8_t wake_pcm_end[] asm("_binary_wake_pcm_end");
extern const uint8_t sleep_pcm_start[] asm("_binary_sleep_pcm_start");
extern const uint8_t sleep_pcm_end[] asm("_binary_sleep_pcm_end");
static struct { int16_t *pcm; uint32_t samples; } prompts[2];
static struct {
    int id; uint32_t queued,played; bool active,started_reported,done_reported;
    int64_t queued_us,started_us,finished_us,quiet_until_us;
} cue;

static bool sent(i2s_chan_handle_t ch, i2s_event_data_t *event, void *ctx) {
    portENTER_CRITICAL_ISR(&lock);
    dma_count++;
    int d=0;for(;d<6;d++) if(dma[d].ptr==event->dma_buf || !dma[d].ptr) break;
    if(d==6){portEXIT_CRITICAL_ISR(&lock);return false;}
    dma_t *m=&dma[d];
    bool audible=m->credit>0&&(m->slot==PROMPT_SLOT || (m->slot>=0&&!responses[m->slot].cancelled));
    if(m->credit && m->slot==PROMPT_SLOT) {
        if(!cue.started_us)cue.started_us=esp_timer_get_time();
        cue.played+=m->credit;level_milli=m->level;
        if(cue.played==prompts[cue.id].samples) {
            cue.active=false;cue.finished_us=esp_timer_get_time();cue.quiet_until_us=cue.finished_us+180000;
        }
    } else if(m->credit && m->slot>=0 && !responses[m->slot].cancelled) {
        if(!responses[m->slot].first_play_us)responses[m->slot].first_play_us=esp_timer_get_time();
        responses[m->slot].played+=m->credit;
        level_milli=m->level;
    }
    m->ptr=event->dma_buf;m->bytes=event->size;m->slot=-1;m->credit=0;m->level=0;
    int32_t *out=event->dma_buf;unsigned count=event->size/8;
    memset(out,0,event->size);
    uint32_t abs_sum=0;
    for(unsigned i=0;i<count;) {
        if(cue.active && cue.queued<prompts[cue.id].samples) {
            int16_t value=prompts[cue.id].pcm[cue.queued++];
            m->slot=PROMPT_SLOT;m->credit++;
            int32_t v=(int32_t)value*65536;
            out[i*2]=v;out[i*2+1]=v;abs_sum+=abs((int)value);i++;continue;
        }
        // A DMA descriptor has one owner, so a cue cannot acquire cloud credits.
        if(m->slot==PROMPT_SLOT)break;
        if(rd==wr) break;
        sample_t s=ring[rd%RING_SAMPLES];
        if(responses[s.slot].cancelled) {rd++;continue;}
        if(m->slot>=0 && m->slot!=s.slot) break;
        rd++;m->slot=s.slot;m->credit+=s.credit;
        int32_t v=(int32_t)s.value*65536;
        out[i*2]=v;out[i*2+1]=v;abs_sum+=abs((int)s.value);i++;
    }
    m->level=abs_sum/(count*7);if(m->level>1000)m->level=1000;
    bool pending=rd!=wr;
    for(int j=0;j<6;j++) pending|=dma[j].credit>0;
    xy_speaking=audible;
    if(!pending)level_milli=0;
    portEXIT_CRITICAL_ISR(&lock);
    return false;
}

void playback_init(void) {
    ring=heap_caps_malloc(RING_SAMPLES*sizeof(sample_t),MALLOC_CAP_SPIRAM);
    assert(ring);guard=xSemaphoreCreateMutex();
    const uint8_t *starts[]={wake_pcm_start,sleep_pcm_start},*ends[]={wake_pcm_end,sleep_pcm_end};
    for(int i=0;i<2;i++) {
        size_t bytes=ends[i]-starts[i];assert(bytes>0&&bytes%2==0);
        prompts[i].pcm=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM);assert(prompts[i].pcm);
        memcpy(prompts[i].pcm,starts[i],bytes);prompts[i].samples=bytes/2;
    }
    for(int i=0;i<6;i++)dma[i].slot=-1;
    i2s_event_callbacks_t cb={.on_sent=sent};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(xy_tx,&cb,NULL));
}

// Called under the playback mutex and the ISR lock.
static void stop_prompt(void) {
    for(int d=0;d<6;d++)if(dma[d].slot==PROMPT_SLOT && dma[d].ptr) {
        memset(dma[d].ptr,0,dma[d].bytes);dma[d].slot=-1;dma[d].credit=0;
    }
    cue.active=false;cue.done_reported=true;cue.quiet_until_us=esp_timer_get_time()+180000;
}
void playback_prompt(xy_prompt_t prompt) {
    if(!guard)return;
    xSemaphoreTake(guard,portMAX_DELAY);portENTER_CRITICAL(&lock);
    stop_prompt();memset(&cue,0,sizeof(cue));
    cue.id=prompt;cue.active=true;cue.queued_us=esp_timer_get_time();
    portEXIT_CRITICAL(&lock);xSemaphoreGive(guard);
}
void playback_prompt_cancel(void) {
    if(!guard)return;
    xSemaphoreTake(guard,portMAX_DELAY);portENTER_CRITICAL(&lock);stop_prompt();
    portEXIT_CRITICAL(&lock);xSemaphoreGive(guard);
}
bool playback_prompt_busy(void) {
    portENTER_CRITICAL(&lock);
    bool busy=cue.active || esp_timer_get_time()<cue.quiet_until_us;
    portEXIT_CRITICAL(&lock);return busy;
}

static bool is_blocked(const char *id) {
    for(int i=0;i<64;i++)if(!strcmp(id,blocked[i]))return true;
    return false;
}
static int find_response(const char *id, bool create) {
    if(!id || strlen(id)>=128 || is_blocked(id))return -1;
    for(int i=0;i<NSLOTS;i++)if(!strcmp(responses[i].id,id))return responses[i].cancelled?-1:i;
    if(!create)return -1;
    for(int i=0;i<NSLOTS;i++)if(!responses[i].id[0] || responses[i].drained || responses[i].cancelled) {
        portENTER_CRITICAL(&lock);
        memset(&responses[i],0,sizeof(response_t));
        strlcpy(responses[i].id,id,128);
        portEXIT_CRITICAL(&lock);
        return i;
    }
    return -1;
}
static bool put(int slot, int16_t value, unsigned credit) {
    bool ok=false;portENTER_CRITICAL(&lock);
    if(wr-rd<RING_SAMPLES) {ring[wr++%RING_SAMPLES]=(sample_t){value,slot,credit};ok=true;}
    portEXIT_CRITICAL(&lock);return ok;
}
static void report(int i,bool force) {
    response_t *r=&responses[i];if(!r->id[0])return;
    uint32_t played;int64_t first_play;
    portENTER_CRITICAL(&lock);played=r->played;first_play=r->first_play_us;portEXIT_CRITICAL(&lock);
    if(first_play&&!r->started_reported) {
        r->started_reported=true;
        cJSON *start=cJSON_CreateObject();cJSON_AddStringToObject(start,"type","device.playback_started");
        cJSON_AddStringToObject(start,"response_id",r->id);
        cJSON_AddNumberToObject(start,"uptime_ms",first_play/1000);
        cJSON_AddNumberToObject(start,"receive_to_play_ms",(first_play-r->received_us)/1000);
        transport_send(start);
    }
    bool drained=r->done && !r->cancelled && played==r->total && r->received==r->total;
    if(!force && played==r->reported && (!drained || r->drained))return;
    r->reported=played;r->drained=drained;
    cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","playback.progress");
    cJSON_AddStringToObject(e,"response_id",r->id);cJSON_AddNumberToObject(e,"played_samples",played);
    cJSON_AddBoolToObject(e,"drained",drained);transport_send(e);
}
static void cancel(const cJSON *ids) {
    for(int i=0;i<NSLOTS;i++) {
        response_t *r=&responses[i];if(!r->id[0]||r->drained||r->cancelled)continue;
        bool match=ids==NULL;const cJSON *id;
        cJSON_ArrayForEach(id,ids)if(cJSON_IsString(id)&&!strcmp(r->id,id->valuestring))match=true;
        if(!match)continue;
        portENTER_CRITICAL(&lock);r->cancelled=true;
        for(int d=0;d<6;d++)if(dma[d].slot==i && dma[d].ptr) {memset(dma[d].ptr,0,dma[d].bytes);dma[d].credit=0;dma[d].slot=-1;}
        portEXIT_CRITICAL(&lock);
        strlcpy(blocked[blocked_pos++%64],r->id,128);report(i,true);
    }
    // Remove cancelled samples before a slot is reused; leave other responses intact.
    portENTER_CRITICAL(&lock);
    uint32_t dst=rd;
    for(uint32_t p=rd;p!=wr;p++)if(!responses[ring[p%RING_SAMPLES].slot].cancelled)ring[dst++%RING_SAMPLES]=ring[p%RING_SAMPLES];
    wr=dst;xy_speaking=rd!=wr;level_milli=0;
    portEXIT_CRITICAL(&lock);
}
void playback_clear(void) { if(!guard)return;xSemaphoreTake(guard,portMAX_DELAY);cancel(NULL);xSemaphoreGive(guard); }
void playback_progress(void) {
    if(!guard)return;
    xSemaphoreTake(guard,portMAX_DELAY);
    for(int i=0;i<NSLOTS;i++)report(i,false);
    portENTER_CRITICAL(&lock);
    bool started=cue.started_us&&!cue.started_reported,done=cue.finished_us&&!cue.done_reported;
    int id=cue.id;int64_t queued=cue.queued_us,first=cue.started_us,finished=cue.finished_us;
    if(started)cue.started_reported=true;
    if(done)cue.done_reported=true;
    portEXIT_CRITICAL(&lock);
    if(started||done) {
        cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","device.prompt");
        cJSON_AddStringToObject(e,"name",id==XY_PROMPT_WAKE?"wake":"sleep");
        cJSON_AddStringToObject(e,"state",done?"completed":"started");
        cJSON_AddNumberToObject(e,"start_latency_ms",(first-queued)/1000);
        if(done)cJSON_AddNumberToObject(e,"duration_ms",(finished-first)/1000);
        transport_send(e);
    }
    xSemaphoreGive(guard);
}
void playback_diagnostics(cJSON *e) {
    cJSON_AddNumberToObject(e,"dma_count",dma_count);
    cJSON_AddNumberToObject(e,"queued_output_samples",wr-rd);
    cJSON *a=cJSON_AddArrayToObject(e,"responses");
    for(int i=0;i<NSLOTS;i++)if(responses[i].id[0]) {
        response_t *r=&responses[i];cJSON *v=cJSON_CreateObject();cJSON_AddItemToArray(a,v);
        cJSON_AddStringToObject(v,"id",r->id);cJSON_AddNumberToObject(v,"seq",r->seq);
        cJSON_AddNumberToObject(v,"received",r->received);cJSON_AddNumberToObject(v,"played",r->played);
        cJSON_AddNumberToObject(v,"total",r->total);cJSON_AddBoolToObject(v,"cancelled",r->cancelled);
    }
}

void playback_event(const cJSON *event) {
    const char *type=cJSON_GetStringValue(cJSON_GetObjectItem(event,"type"));if(!type)return;
    xSemaphoreTake(guard,portMAX_DELAY);
    if(!strcmp(type,"audio.clear")) {cancel(cJSON_GetObjectItem(event,"response_ids"));goto done;}
    const char *rid=cJSON_GetStringValue(cJSON_GetObjectItem(event,"response_id"));
    // Acknowledge late USB chunks even while recording; otherwise the bridge's
    // reader would wait for an ACK and delay the input.begin cancellation.
    if(xy_ptt_pressed) {
        if(rid&&*rid) {
            bool known=false;for(unsigned i=0;i<64;i++)if(!strcmp(blocked[i],rid)){known=true;break;}
            if(!known)strlcpy(blocked[blocked_pos++%64],rid,128);
        }
        goto done;
    }
    int slot=find_response(rid,true);if(slot<0)goto done;
    response_t *r=&responses[slot];
    if(!strcmp(type,"audio.end")) {
        r->total=(uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(event,"samples"));
        for(int j=0;j<r->ntail;j++)put(slot,r->tail[j],1);
        r->ntail=0;r->done=true;report(slot,true);goto done;
    }
    uint32_t seq=(uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(event,"seq"));
    if(seq<=r->seq||r->done)goto done;
    const char *b64=cJSON_GetStringValue(cJSON_GetObjectItem(event,"audio"));if(!b64)goto done;
    size_t cap=strlen(b64)*3/4+4,n=0;
    uint8_t *pcm=heap_caps_malloc(cap,MALLOC_CAP_SPIRAM);
    if(!pcm)goto done;
    if(mbedtls_base64_decode(pcm,cap,&n,(const uint8_t*)b64,strlen(b64)) || n%2) {free(pcm);goto done;}
    uint32_t free_samples,write_pos;
    portENTER_CRITICAL(&lock);
    // RING_SAMPLES does not divide 2^32. Normalize before counter rollover so
    // free logical positions cannot alias unread cells across that discontinuity.
    if(wr>UINT32_MAX-RING_SAMPLES) {
        uint32_t base=rd-rd%RING_SAMPLES;rd-=base;wr-=base;
    }
    write_pos=wr;free_samples=RING_SAMPLES-(write_pos-rd);portEXIT_CRITICAL(&lock);
    if(n/2+4>free_samples) {
        free(pcm);cancel(NULL);terminal_status("error","playback_buffer_full");
        cJSON *e=cJSON_CreateObject();cJSON_AddStringToObject(e,"type","interrupt");transport_send(e);goto done;
    }
    if(!r->received)r->received_us=esp_timer_get_time();
    r->seq=seq;r->received+=n/2;
    // guard serializes producers, cancellation/compaction and slot reuse. Reserve
    // against rd above, then write only into free slots without blocking the ISR.
    // sent() can advance rd, but sees only the old wr until the final locked
    // publication. The existing conservative capacity check covers all outputs
    // plus the carried tail; available space can only grow while guard is held.
    for(size_t j=0;j<n/2;j++) {
        r->tail[r->ntail++]=((int16_t*)pcm)[j];
        if(r->ntail==3) {
            // 24k -> 16k streaming conversion; source credits remain in 24k units.
            ring[write_pos++%RING_SAMPLES]=(sample_t){r->tail[0],slot,1};
            ring[write_pos++%RING_SAMPLES]=(sample_t){((int32_t)r->tail[1]+r->tail[2])/2,slot,2};
            r->ntail=0;
        }
    }
    portENTER_CRITICAL(&lock);wr=write_pos;portEXIT_CRITICAL(&lock);
    free(pcm);
done:
    if(!strcmp(type,"audio.delta")) {
        cJSON *ack=cJSON_CreateObject();cJSON_AddStringToObject(ack,"type","device.audio_ack");
        const cJSON *id=cJSON_GetObjectItem(event,"response_id");
        const cJSON *sq=cJSON_GetObjectItem(event,"seq");
        if(id)cJSON_AddItemToObject(ack,"response_id",cJSON_Duplicate(id,true));
        if(sq)cJSON_AddItemToObject(ack,"seq",cJSON_Duplicate(sq,true));
        transport_send(ack);
    }
    xSemaphoreGive(guard);
}
