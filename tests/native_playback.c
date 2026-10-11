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

int main(void){
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
    free(ring);
    terminal_button(true);assert(!xy_ptt_pressed&&xy_state==XY_SLEEP);
    puts("PASS: local cues, DMA completion, re-wake, PTT mute, late-chunk ACK/discard, silent disconnect");
    return 0;
}
