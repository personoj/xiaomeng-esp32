/* Synthetic fixtures shared by host tests and the microbenchmark. */
static int32_t audio_buffers[6][320];
static unsigned audio_next_dma;
static int16_t fixture_prompt[16];
static void reset_audio(void){
    assert(!native_critical_depth&&!native_guard_depth);
    native_before_lock=native_after_unlock=NULL;
    if(!ring)ring=calloc(RING_SAMPLES,sizeof(*ring));
    assert(ring);memset(ring,0,RING_SAMPLES*sizeof(*ring));
    memset(responses,0,sizeof(responses));memset(dma,0,sizeof(dma));
    memset(blocked,0,sizeof(blocked));memset(&cue,0,sizeof(cue));
    memset(audio_buffers,0,sizeof(audio_buffers));
    for(int i=0;i<6;i++)dma[i].slot=-1;
    guard=(void*)1;rd=wr=0;blocked_pos=0;audio_next_dma=0;dma_count=0;
    xy_speaking=false;xy_ptt_pressed=false;xy_state=XY_LISTENING;
    level_milli=0;native_critical_entries=0;test_us=1000000;
    cloud_drains=cloud_starts=audio_acks=interrupts=buffer_full_errors=0;
    for(int i=0;i<2;i++){prompts[i].pcm=fixture_prompt;prompts[i].samples=16;}
}
static void audio_tick(bool progress){
    i2s_event_data_t e={audio_buffers[audio_next_dma++%6],sizeof(audio_buffers[0])};
    test_us+=10000;sent(NULL,&e,NULL);if(progress)playback_progress();
}
static cJSON *make_delta(const char *id,uint32_t seq,const int16_t *pcm,size_t samples){
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t size=samples*2,encoded=(size+2)/3*4;
    uint8_t *bytes=malloc(size?size:1);char *base64=malloc(encoded+1);assert(bytes&&base64);
    for(size_t i=0;i<samples;i++){bytes[i*2]=(uint8_t)pcm[i];bytes[i*2+1]=(uint8_t)((uint16_t)pcm[i]>>8);}
    for(size_t i=0,j=0;i<size;i+=3,j+=4){
        uint32_t bits=(uint32_t)bytes[i]<<16;
        if(i+1<size)bits|=(uint32_t)bytes[i+1]<<8;
        if(i+2<size)bits|=bytes[i+2];
        base64[j]=alphabet[bits>>18];base64[j+1]=alphabet[(bits>>12)&63];
        base64[j+2]=i+1<size?alphabet[(bits>>6)&63]:'=';
        base64[j+3]=i+2<size?alphabet[bits&63]:'=';
    }
    base64[encoded]=0;
    cJSON *event=cJSON_CreateObject();assert(event);
    cJSON_AddStringToObject(event,"type","audio.delta");cJSON_AddStringToObject(event,"response_id",id);
    cJSON_AddNumberToObject(event,"seq",seq);cJSON_AddStringToObject(event,"audio",base64);
    free(bytes);free(base64);return event;
}
static void audio_delta(const char *id,uint32_t seq,const int16_t *pcm,size_t samples){
    cJSON *event=make_delta(id,seq,pcm,samples);playback_event(event);cJSON_Delete(event);
}
static void audio_end(const char *id,unsigned samples){
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"type","audio.end");
    cJSON_AddStringToObject(event,"response_id",id);cJSON_AddNumberToObject(event,"samples",samples);
    playback_event(event);cJSON_Delete(event);
}
static void audio_cancel(const char *id){
    cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"type","audio.clear");
    cJSON *ids=cJSON_AddArrayToObject(event,"response_ids");cJSON_AddItemToArray(ids,cJSON_CreateString(id));
    playback_event(event);cJSON_Delete(event);
}
static void check_sample(uint32_t offset,int slot,int16_t value,unsigned credit){
    sample_t s=ring[offset%RING_SAMPLES];assert(s.slot==slot&&s.value==value&&s.credit==credit);
}
