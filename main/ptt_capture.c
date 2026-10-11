#include "ptt_capture.h"
#include <string.h>

void ptt_reset(ptt_capture_t *p) {
    p->used=p->samples=0;
    p->down=p->active=p->begun=p->released=false;
}
static void start_stream(ptt_capture_t *p) {
    if(!p->begun) {p->event(p->context,true,0);p->begun=true;}
    if(p->used) {p->audio(p->context,p->pending,p->used);p->used=0;}
}
unsigned ptt_process(ptt_capture_t *p,bool pressed,bool ready,const int16_t *pcm,unsigned samples) {
    unsigned result=0;
    if(pressed&&!p->down) {
        if(p->released)result|=PTT_ABORTED;
        ptt_reset(p);p->active=true;result|=PTT_STARTED;
    }
    // A lost live connection invalidates this hold. It must not resume later.
    if(!ready&&p->begun) {
        ptt_reset(p);p->down=pressed;return result|PTT_ABORTED;
    }
    if(p->active&&(pressed||p->down)) {
        // Include the final fetched AFE frame on the release edge, then commit;
        // no subsequent unpressed frames can enter the turn.
        if(ready) {start_stream(p);p->audio(p->context,pcm,samples);}
        else {
            if(samples>p->capacity-p->used) {
                ptt_reset(p);p->down=pressed;return result|PTT_ABORTED;
            }
            memcpy(p->pending+p->used,pcm,samples*sizeof(int16_t));p->used+=samples;
        }
        p->samples+=samples;
    }
    if(!pressed&&p->down&&p->active) {p->active=false;p->released=true;}
    p->down=pressed;
    if(ready&&p->released) {
        if(p->samples) {start_stream(p);p->event(p->context,false,p->samples);result|=PTT_SUBMITTED;}
        ptt_reset(p);
    }
    return result;
}
bool ptt_button_update(ptt_button_t *b,bool raw,uint32_t now_ms) {
    if(raw!=b->raw) {b->raw=raw;b->changed_ms=now_ms;}
    if((uint32_t)(now_ms-b->changed_ms)>=25)b->stable=b->raw;
    return b->stable;
}
