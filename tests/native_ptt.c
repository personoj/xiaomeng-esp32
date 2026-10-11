#include "../main/ptt_capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int16_t saved[256];
static unsigned used,begins,commits,committed;
static bool open;
static void event(void *ctx,bool begin,unsigned samples) {
    if(begin){assert(!open);open=true;begins++;}
    else {assert(open);open=false;commits++;committed=samples;}
}
static void audio(void *ctx,const int16_t *pcm,unsigned samples) {
    assert(open&&used+samples<=256);memcpy(saved+used,pcm,samples*2);used+=samples;
}
static void clear(void){used=begins=commits=committed=0;open=false;}
int main(void) {
    int16_t pending[64],frame[]={11,22,33,44};
    ptt_capture_t p={.pending=pending,.capacity=64,.event=event,.audio=audio};
    for(int i=0;i<8;i++)ptt_process(&p,false,true,frame,4);
    assert(used==0&&begins==0);
    assert(ptt_process(&p,true,true,frame,4)&PTT_STARTED);
    ptt_process(&p,true,true,frame,4);
    assert(ptt_process(&p,false,true,frame,4)&PTT_SUBMITTED);
    for(int i=0;i<8;i++)ptt_process(&p,false,true,frame,4);
    assert(begins==1&&commits==1&&committed==12&&used==12);
    for(unsigned i=0;i<used;i++)assert(saved[i]==frame[i%4]);

    clear();ptt_reset(&p);
    ptt_process(&p,true,false,frame,4);ptt_process(&p,false,false,frame,4);
    assert(used==0&&begins==0&&p.used==8);
    assert(ptt_process(&p,false,true,frame,4)&PTT_SUBMITTED);
    assert(begins==1&&commits==1&&used==8&&committed==8);

    clear();ptt_reset(&p);ptt_process(&p,true,true,frame,4);
    assert(ptt_process(&p,true,false,frame,4)&PTT_ABORTED);
    open=false; /* The peer also discards its input on disconnect. */
    ptt_process(&p,true,true,frame,4);ptt_process(&p,false,true,frame,4);
    assert(begins==1&&commits==0&&used==4);
    ptt_process(&p,true,true,frame,4);ptt_process(&p,false,true,frame,4);
    assert(begins==2&&commits==1&&committed==8);

    clear();ptt_reset(&p);p.capacity=4;
    ptt_process(&p,true,false,frame,4);
    assert(ptt_process(&p,true,false,frame,4)&PTT_ABORTED);
    ptt_process(&p,true,true,frame,4);ptt_process(&p,false,true,frame,4);
    assert(used==0&&begins==0&&commits==0);
    p.capacity=64;ptt_process(&p,true,false,frame,4);ptt_reset(&p);
    ptt_process(&p,false,true,frame,4);assert(used==0&&commits==0);

    ptt_button_t b={0};
    assert(!ptt_button_update(&b,true,5));
    assert(!ptt_button_update(&b,false,10));
    assert(!ptt_button_update(&b,true,15));
    assert(!ptt_button_update(&b,true,39));
    assert(ptt_button_update(&b,true,40));
    assert(ptt_button_update(&b,false,50));
    assert(ptt_button_update(&b,true,55));
    assert(ptt_button_update(&b,false,60));
    assert(!ptt_button_update(&b,false,85));
    puts("PASS: PTT gating, single commit, delayed ready, stale capture discard, overflow, button debounce");
    return 0;
}
