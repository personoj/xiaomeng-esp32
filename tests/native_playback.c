#include "firmware_under_test.c"

static int32_t buffers[6][320];
static unsigned next_dma;
static int16_t wake_data[640],sleep_data[1120];
static void tick(void){
    i2s_event_data_t e={buffers[next_dma++%6],sizeof(buffers[0])};
    test_us+=10000;sent(NULL,&e,NULL);playback_progress();
}
static void settle(void){for(int i=0;i<50;i++)tick();assert(!playback_prompt_busy());}
static void receive(const char *json){terminal_receive(json,(int)strlen(json));}

static void test_terminal_states(void){
    guard=(void*)1;ring=calloc(RING_SAMPLES,sizeof(sample_t));assert(ring);
    for(int i=0;i<6;i++)dma[i].slot=-1;
    for(int i=0;i<640;i++)wake_data[i]=500;
    for(int i=0;i<1120;i++)sleep_data[i]=800;
    prompts[0].pcm=wake_data;prompts[0].samples=640;
    prompts[1].pcm=sleep_data;prompts[1].samples=1120;

    terminal_wake("ni_hao_xiao_yun");
    assert(xy_state==XY_CONNECTING&&cue.active&&wakes==1);
    receive("{\"type\":\"device.wake\"}");assert(wakes==1);
    for(int i=0;i<6;i++)tick();
    assert(cue.played==0&&playback_prompt_busy()); /* queued is not played */
    for(int i=0;i<4;i++)tick();
    assert(cue.played==640&&!cue.active&&playback_prompt_busy()); /* acoustic tail */
    settle();assert(prompt_completions==1&&cloud_drains==0);
    receive("{\"type\":\"ready\"}");assert(xy_state==XY_LISTENING);

    int slot=find_response("cloud-old",true);assert(slot>=0);
    responses[slot].received=3200;responses[slot].total=3200;responses[slot].done=true;
    responses[slot].received_us=test_us;
    for(int i=0;i<3200;i++)assert(put(slot,600,1));
    for(int i=0;i<8;i++)tick();
    assert(responses[slot].played>0&&responses[slot].played<3200);
    receive("{\"type\":\"device.standby\",\"reason\":\"spoken_exit\",\"request_id\":\"exit-1\"}");
    assert(xy_state==XY_SLEEP&&sleeps==1&&responses[slot].cancelled&&rd==wr);
    assert(cue.active&&cue.id==XY_PROMPT_SLEEP);
    terminal_wake("echo");assert(wakes==1);
    receive("{\"type\":\"state\",\"state\":\"disconnected\"}");
    receive("{\"type\":\"audio.clear\"}");
    receive("{\"type\":\"device.standby\",\"reason\":\"spoken_exit\"}");
    receive("{\"type\":\"audio.delta\",\"response_id\":\"late\",\"seq\":1,\"audio\":\"AAAA\"}");
    assert(cue.active&&sleeps==1&&find_response("late",false)==-1);
    settle();assert(cue.played==1120&&prompt_completions==2&&cloud_drains==0);

    terminal_wake("ni_hao_xiao_yun");assert(wakes==2&&cue.id==XY_PROMPT_WAKE);
    settle();assert(prompt_completions==3);
    terminal_button(true);assert(xy_ptt_pressed&&xy_state==XY_LISTENING);
    unsigned acks_before=audio_acks;
    receive("{\"type\":\"audio.delta\",\"response_id\":\"held-late\",\"seq\":1,\"audio\":\"AAAA\"}");
    assert(audio_acks==acks_before+1&&find_response("held-late",false)==-1);
    terminal_button(false);assert(!xy_ptt_pressed);
    receive("{\"type\":\"audio.delta\",\"response_id\":\"held-late\",\"seq\":2,\"audio\":\"AAAA\"}");
    assert(audio_acks==acks_before+2&&find_response("held-late",false)==-1);
    terminal_sleep("usb_disconnected");
    settle();assert(prompt_completions==3); /* errors do not speak a false exit confirmation */
    free(ring);ring=NULL;
    terminal_button(true);assert(!xy_ptt_pressed&&xy_state==XY_SLEEP);
    puts("PASS: local cues, DMA completion, re-wake, PTT mute, late-chunk ACK/discard, silent disconnect");
}

#include "native_audio_helpers.h"

static void test_wire_pcm_fixture(void){
    reset_audio();int16_t pcm[]={1,-2,32767,-32768};
    cJSON *event=make_delta("wire",1,pcm,4);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(event,"audio")),"AQD+//9/AIA="));
    cJSON_Delete(event);
    const char *wire="{\"type\":\"audio.delta\",\"response_id\":\"wire\",\"seq\":1,\"audio\":\"AQD+//9/AIA=\"}";
    terminal_receive(wire,(int)strlen(wire));int slot=find_response("wire",false);assert(slot>=0);
    assert(responses[slot].received==4&&audio_acks==1);
    check_sample(0,slot,1,1);check_sample(1,slot,16382,2);
    audio_end("wire",4);check_sample(2,slot,-32768,1);
    puts("PASS: literal wire Base64 traverses production terminal_receive and PCM conversion");
}

static void test_streaming_conversion(void){
    /* Carry 1/2 source samples across chunk boundaries; preserve signed rounding. */
    for(unsigned n=1;n<=11;n++)for(unsigned split=0;split<=n;split++){
        reset_audio();int16_t pcm[11]={-32768,32767,-32768,100,-200,301,40,41,42,-7,8};
        audio_delta("convert",1,pcm,split);audio_delta("convert",2,pcm+split,n-split);
        int slot=find_response("convert",false);assert(slot>=0);
        response_t *r=&responses[slot];assert(r->received==n&&r->played==0&&r->ntail==(int)(n%3));
        uint32_t full=n/3*2;assert(wr-rd==full);
        for(unsigned i=0;i<n/3;i++){
            check_sample(i*2,slot,pcm[i*3],1);
            check_sample(i*2+1,slot,(int16_t)(((int32_t)pcm[i*3+1]+pcm[i*3+2])/2),2);
        }
        audio_end("convert",n);assert(wr-rd==full+n%3&&r->played==0&&!r->drained);
        for(unsigned i=0;i<n%3;i++)check_sample(full+i,slot,pcm[n/3*3+i],1);
        for(int i=0;i<6;i++)audio_tick(true);
        assert(r->played==0&&cloud_drains==0&&cloud_starts==0); /* DMA queued, not completed */
        audio_tick(true);assert(r->played==n&&r->drained&&cloud_drains==1&&cloud_starts==1);
        assert(r->first_play_us>r->received_us);
        for(int i=0;i<8;i++)audio_tick(true);
        assert(r->played==n&&cloud_drains==1&&cloud_starts==1);
    }
    puts("PASS: 77 chunk/tail splits, signed 24k-to-16k conversion, DMA-only played credits");
}

static void test_ring_wrap_and_multiple_responses(void){
    reset_audio();rd=wr=RING_SAMPLES-2;
    int16_t a[]={11,20,22,31,40,42,51,60,62,71,80};
    int16_t b[]={101,110,112};
    audio_delta("wrap-a",1,a,11);audio_end("wrap-a",11);
    audio_delta("wrap-b",1,b,3);audio_end("wrap-b",3);
    int sa=find_response("wrap-a",false),sb=find_response("wrap-b",false);
    assert(wr-rd==10);check_sample(RING_SAMPLES-2,sa,11,1);
    check_sample(RING_SAMPLES,sa,31,1);check_sample(RING_SAMPLES+5,sa,80,1);
    check_sample(RING_SAMPLES+6,sb,101,1);
    audio_tick(false);assert(dma[0].slot==sa&&dma[0].credit==11);
    assert(rd==RING_SAMPLES+6&&responses[sa].played==0&&responses[sb].played==0);
    audio_tick(false);assert(dma[1].slot==sb&&dma[1].credit==3);assert(rd==wr);
    for(int i=0;i<4;i++)audio_tick(true);
    assert(cloud_drains==0);
    audio_tick(true);assert(responses[sa].played==11&&responses[sb].played==0);
    audio_tick(true);assert(responses[sb].played==3&&cloud_drains==2);
    puts("PASS: physical ring wrap, tail wrap, separate DMA owners and drain receipts");
}

static void test_cancel_isolation_and_reuse(void){
    reset_audio();rd=wr=RING_SAMPLES-90;
    int16_t old_pcm[1200],keep_pcm[21],new_pcm[12];
    for(int i=0;i<1200;i++)old_pcm[i]=300;
    for(int i=0;i<21;i++)keep_pcm[i]=700;
    for(int i=0;i<12;i++)new_pcm[i]=900;
    audio_delta("old",1,old_pcm,1200);audio_end("old",1200);
    audio_delta("keep",1,keep_pcm,21);audio_end("keep",21);
    int old=find_response("old",false),keep=find_response("keep",false);
    for(int i=0;i<2;i++)audio_tick(false);
    assert(responses[old].played==0&&dma[0].credit==240&&dma[1].credit==240);
    audio_cancel("old");assert(responses[old].cancelled&&wr-rd==14);
    for(int d=0;d<2;d++){
        assert(!dma[d].credit&&dma[d].slot==-1);
        for(unsigned j=0;j<sizeof(audio_buffers[d])/sizeof(int32_t);j++)assert(audio_buffers[d][j]==0);
    }
    for(uint32_t p=rd;p!=wr;p++)assert(ring[p%RING_SAMPLES].slot==keep);
    audio_delta("new",1,new_pcm,12);audio_end("new",12);
    int newer=find_response("new",false);assert(newer==old); /* reused slot cannot inherit old DMA credit */
    unsigned acks=audio_acks;audio_delta("old",2,old_pcm,1200);
    assert(audio_acks==acks+1&&find_response("old",false)==-1&&responses[newer].received==12);
    for(int i=0;i<20;i++)audio_tick(true);
    assert(responses[keep].played==21&&responses[newer].played==12&&cloud_drains==2);
    assert(rd==wr);
    puts("PASS: selective cancellation, ring compaction, DMA zeroing, blocked late chunks, safe slot reuse");
}

static void test_full_buffer_and_rejected_chunks(void){
    reset_audio();int16_t pcm[9]={1,2,3,4,5,6,7,8,9};
    int slot=find_response("full",true);
    for(unsigned i=0;i<RING_SAMPLES-8;i++)assert(put(slot,123,1));
    responses[slot].received=RING_SAMPLES-8;
    uint32_t before=wr;audio_delta("full",1,pcm,4); /* exactly n/2 + 4 free remains accepted */
    assert(wr==before+2&&responses[slot].ntail==1&&!responses[slot].cancelled);
    audio_delta("full",2,pcm,3); /* only six free, existing conservative check rejects seven */
    assert(responses[slot].cancelled&&rd==wr&&interrupts==1&&buffer_full_errors==1&&audio_acks==2);
    for(int i=0;i<8;i++)audio_tick(true);
    assert(cloud_drains==0);

    reset_audio();audio_delta("seq",1,pcm,6);slot=find_response("seq",false);
    before=wr;audio_delta("seq",1,pcm,6);audio_delta("seq",0,pcm,6);
    assert(wr==before&&responses[slot].received==6&&audio_acks==3);
    const char *odd="{\"type\":\"audio.delta\",\"response_id\":\"seq\",\"seq\":2,\"audio\":\"AA==\"}";
    cJSON *bad=cJSON_ParseWithLength(odd,strlen(odd));
    assert(bad);playback_event(bad);cJSON_Delete(bad); /* odd decoded byte count */
    assert(wr==before&&responses[slot].seq==1&&audio_acks==4);
    const char *invalid="{\"type\":\"audio.delta\",\"response_id\":\"seq\",\"seq\":2,\"audio\":\"!!!!\"}";
    bad=cJSON_ParseWithLength(invalid,strlen(invalid));assert(bad);playback_event(bad);cJSON_Delete(bad);
    assert(wr==before&&responses[slot].seq==1&&audio_acks==5);
    audio_end("seq",6);audio_delta("seq",2,pcm,3);assert(wr==before&&audio_acks==6);
    puts("PASS: unchanged conservative buffer-full interrupt, duplicate/done/invalid chunk ACK behavior");
}

static unsigned inject_entries,inject_calls;
static uint32_t old_write_pos;
static void inject_before_publish(void){
    inject_entries++;
    if(inject_entries!=2)return; /* response already exists: reservation, then publish */
    native_before_lock=NULL;
    assert(native_guard_depth==1); /* excludes cancellation, another writer and slot reuse */
    assert(wr==old_write_pos); /* staged output must remain invisible to the consumer */
    audio_tick(false);inject_calls++;
    assert(rd==old_write_pos); /* ISR consumed only previously published samples */
    assert(dma[0].credit==3);
    assert(audio_buffers[0][0]==(int32_t)111*65536&&audio_buffers[0][2]==(int32_t)222*65536);
    for(unsigned i=4;i<sizeof(audio_buffers[0])/sizeof(int32_t);i++)assert(audio_buffers[0][i]==0);
}
static void test_publish_interleave(void){
#ifdef NATIVE_EXPECT_BATCH_APPEND
    reset_audio();rd=wr=RING_SAMPLES-1;
    int16_t previous[]={111,221,223},next[960];
    for(int i=0;i<960;i++)next[i]=(int16_t)(i-480);
    audio_delta("publish",1,previous,3);old_write_pos=wr;
    inject_calls=inject_entries=0;native_critical_entries=0;native_before_lock=inject_before_publish;
    audio_delta("publish",2,next,960);native_before_lock=NULL;
    assert(inject_calls==1&&inject_entries==2&&native_critical_entries==2);
    assert(wr==old_write_pos+640&&rd==old_write_pos);
    int slot=find_response("publish",false);assert(responses[slot].played==0);
    for(unsigned i=0;i<320;i++){
        check_sample(old_write_pos+i*2,slot,next[i*3],1);
        check_sample(old_write_pos+i*2+1,slot,(int16_t)(((int32_t)next[i*3+1]+next[i*3+2])/2),2);
    }
    audio_end("publish",963);for(int i=0;i<20;i++)audio_tick(true);
    assert(responses[slot].played==963&&cloud_drains==1);
    puts("PASS: ISR before single publication sees only old wr; 960 source samples use two lock entries");
#endif
}

static void inject_while_nearly_full(void){
    inject_entries++;if(inject_entries!=2)return;
    native_before_lock=NULL;assert(wr==old_write_pos&&native_guard_depth==1);
    audio_tick(false);inject_calls++;
    assert(dma[0].credit==160);
    for(unsigned i=0;i<320;i++)assert(audio_buffers[0][i]==(int32_t)321*65536);
}
static void test_nearly_full_interleave(void){
#ifdef NATIVE_EXPECT_BATCH_APPEND
    reset_audio();rd=wr=RING_SAMPLES-80;int slot=find_response("nearly-full",true);
    unsigned existing=RING_SAMPLES-964;
    for(unsigned i=0;i<existing;i++)assert(put(slot,321,1));
    responses[slot].received=existing;old_write_pos=wr;
    int16_t next[960];for(unsigned i=0;i<960;i++)next[i]=-654;
    inject_calls=inject_entries=0;native_critical_entries=0;native_before_lock=inject_while_nearly_full;
    audio_delta("nearly-full",1,next,960);native_before_lock=NULL;
    assert(inject_calls==1&&native_critical_entries==2&&wr==old_write_pos+640);
    for(uint32_t p=rd;p!=old_write_pos;p++)check_sample(p,slot,321,1);
    for(unsigned i=0;i<640;i++)check_sample(old_write_pos+i,slot,-654,i%2?2:1);
    assert(responses[slot].played==0&&!interrupts);
    puts("PASS: nearly-full reservation and concurrent DMA consumption preserve all unread samples");
#endif
}

static void test_streaming_interleave(void){
    /* Deterministic stream across the ring boundary, varying chunk sizes and ISR cadence. */
    reset_audio();rd=wr=RING_SAMPLES-100;
    int16_t pcm[4097];for(int i=0;i<4097;i++)pcm[i]=(int16_t)((i*7919)%60001-30000);
    unsigned offset=0,seq=0,seed=417;
    while(offset<4097){
        seed=seed*1664525u+1013904223u;unsigned count=seed%97+1;
        if(count>4097-offset)count=4097-offset;
        audio_delta("stream",++seq,pcm+offset,count);offset+=count;
        seed=seed*1664525u+1013904223u;
        if(seed%3==0)audio_tick(true);
        int slot=find_response("stream",false);
        assert(responses[slot].played<=responses[slot].received&&!responses[slot].drained);
    }
    audio_end("stream",4097);for(int i=0;i<100;i++)audio_tick(true);
    int slot=find_response("stream",false);assert(responses[slot].played==4097&&cloud_drains==1&&rd==wr);
    puts("PASS: randomized chunk sizes and DMA interleave preserve exact source credit totals");
}

static void test_counter_rollover(void){
    reset_audio();rd=wr=UINT32_MAX-RING_SAMPLES+13;
    int old=find_response("rollover-old",true),keep=find_response("rollover-keep",true);
    const unsigned existing=RING_SAMPLES-16,keep_count=21;
    for(unsigned i=0;i<existing-keep_count;i++)assert(put(old,321,1));
    for(unsigned i=0;i<keep_count;i++)assert(put(keep,765,1));
    responses[old].received=existing-keep_count;responses[keep].received=keep_count;
    uint32_t old_read=rd;assert(wr==UINT32_MAX-3);
    int16_t next[12];for(unsigned i=0;i<12;i++)next[i]=-654;
    audio_delta("rollover-new",1,next,12);int newer=find_response("rollover-new",false);
    /* Check data before checking normalization: the old implementation overwrites
     * unread physical cell 0 when its eight output writes cross UINT32_MAX. */
    for(uint32_t i=0;i<existing-keep_count;i++)check_sample(rd+i,old,321,1);
    for(uint32_t i=existing-keep_count;i<existing;i++)check_sample(rd+i,keep,765,1);
    for(uint32_t i=0;i<8;i++)check_sample(rd+existing+i,newer,-654,i%2?2:1);
    assert(rd==old_read%RING_SAMPLES&&wr-rd==existing+8&&wr<2*RING_SAMPLES);
    audio_tick(false);assert(dma[0].slot==old&&dma[0].credit==160);
    audio_cancel("rollover-old");assert(!dma[0].credit&&wr-rd==29);
    for(uint32_t i=0;i<keep_count;i++)check_sample(rd+i,keep,765,1);
    for(uint32_t i=0;i<8;i++)check_sample(rd+keep_count+i,newer,-654,i%2?2:1);
    audio_end("rollover-keep",keep_count);audio_end("rollover-new",12);
    for(int i=0;i<20;i++)audio_tick(true);
    assert(responses[keep].played==21&&responses[newer].played==12&&cloud_drains==2&&rd==wr);
    puts("PASS: near-uint32-rollover normalization preserves unread data, DMA credits and cancel compaction");
}

int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--counter-rollover-only")){
        test_counter_rollover();free(ring);return 0;
    }
    test_terminal_states();test_wire_pcm_fixture();test_streaming_conversion();test_ring_wrap_and_multiple_responses();
    test_cancel_isolation_and_reuse();test_full_buffer_and_rejected_chunks();
    test_publish_interleave();test_nearly_full_interleave();test_streaming_interleave();
#ifdef NATIVE_EXPECT_BATCH_APPEND
    test_counter_rollover();
#endif
    free(ring);ring=NULL;
    printf("PASS: production playback/main checks using cJSON %s\n",cJSON_Version());
    return 0;
}
