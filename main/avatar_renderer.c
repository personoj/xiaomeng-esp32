#include "avatar_renderer.h"
#include <string.h>

enum { WIDTH=360,HEIGHT=360,POINTS=320,SUBROWS=720,CROSSINGS=8 };
static const unsigned counts[5]={32,32,128,64,64};
typedef struct {
    const int16_t *frames;
    uint16_t *background;
    uint32_t frame_count,last_ms;
    int32_t current[POINTS*2];
    int16_t target[POINTS*2],shape[POINTS*2];
    int16_t cuts[SUBROWS][CROSSINGS],clip_left[SUBROWS],clip_right[SUBROWS];
    uint8_t cut_count[SUBROWS];
    unsigned level,overflow;
    bool initialized;
} renderer_t;
static int clamp(int v,int a,int b){return v<a?a:v>b?b:v;}
static uint16_t swap16(uint16_t v){return (uint16_t)((v<<8)|(v>>8));}
static int ceil_div(int a,int b){return a>=0?(a+b-1)/b:a/b;}
size_t avatar_renderer_size(void){return sizeof(renderer_t);}
unsigned avatar_overflows(void *memory){return ((renderer_t*)memory)->overflow;}

bool avatar_init(void *memory,const uint8_t *asset,size_t length,uint16_t *background){
    if(length<36||memcmp(asset,"XYVG",4))return false;
    const uint32_t *h=(const uint32_t*)asset;
    if(h[1]!=WIDTH||h[2]!=HEIGHT||h[3]!=6||h[4]<48||h[5]!=POINTS||h[6]!=POINTS*4)return false;
    if(h[7]%4||h[8]!=36+h[7]||(size_t)h[8]+(size_t)h[3]*h[4]*h[6]>length)return false;
    renderer_t *r=memory;memset(r,0,sizeof(*r));r->frame_count=h[4];r->frames=(const int16_t*)(asset+h[8]);r->background=background;
    const uint16_t *runs=(const uint16_t*)(asset+36);unsigned pixels=0;
    for(unsigned i=0;i<h[7]/4;i++){
        unsigned n=runs[2*i];uint16_t color=swap16(runs[2*i+1]);
        if(pixels+n>WIDTH*HEIGHT)return false;
        while(n--)background[pixels++]=color;
    }
    return pixels==WIDTH*HEIGHT;
}

static int sample(renderer_t *r,unsigned bank,unsigned index,unsigned next,unsigned fraction,unsigned point){
    const int16_t *a=r->frames+((size_t)bank*r->frame_count+index)*POINTS*2;
    const int16_t *b=r->frames+((size_t)bank*r->frame_count+next)*POINTS*2;
    return a[point]+((b[point]-a[point])*(int)fraction)/256;
}
static void target(renderer_t *r,uint32_t ms,unsigned pose,unsigned level){
    uint64_t phase=(uint64_t)ms*30;
    unsigned index=(phase/1000)%r->frame_count,fraction=(phase%1000)*256/1000;
    unsigned next=index+1<r->frame_count?index+1:index;
    unsigned low=pose,high=pose,mix=0;
    if(pose==3){
        unsigned value=level>870?870:level;
        if(value<358){low=3;high=4;mix=value*256/358;}
        else {low=4;high=5;mix=(value-358)*256/(870-358);}
    }
    // Ease the loop seam in geometry space, with a continuous state-independent clock.
    unsigned seam=0;
    if(index>=r->frame_count-24){
        unsigned t=((index-(r->frame_count-24))*256+fraction)/24;
        seam=t*t*(768-2*t)/65536;
    }
    for(unsigned i=0;i<POINTS*2;i++){
        int a=sample(r,low,index,next,fraction,i),b=sample(r,high,index,next,fraction,i);
        int value=a+(b-a)*(int)mix/256;
        if(seam){
            int first_a=r->frames[(size_t)low*r->frame_count*POINTS*2+i];
            int first_b=r->frames[(size_t)high*r->frame_count*POINTS*2+i];
            int first=first_a+(first_b-first_a)*(int)mix/256;
            value+=(first-value)*(int)seam/256;
        }
        r->target[i]=(int16_t)value;
    }
}

static uint16_t covered(uint16_t dest,uint16_t color,unsigned coverage){
    unsigned inv=32-coverage;dest=swap16(dest);
    unsigned red=((dest>>11)*inv+(color>>11)*coverage)/32;
    unsigned green=(((dest>>5)&63)*inv+((color>>5)&63)*coverage)/32;
    unsigned blue=((dest&31)*inv+(color&31)*coverage)/32;
    return swap16((uint16_t)((red<<11)|(green<<5)|blue));
}
static void fill_span(uint16_t *row,int begin,int end,uint16_t color){
    if(begin>=end)return;
    if(begin&1)row[begin++]=color;
    uint32_t pair=((uint32_t)color<<16)|color;
    uint32_t *wide=(uint32_t*)(row+begin);unsigned pairs=(end-begin)/2,j=0;
    for(;j+4<=pairs;j+=4){wide[j]=pair;wide[j+1]=pair;wide[j+2]=pair;wide[j+3]=pair;}
    for(;j<pairs;j++)wide[j]=pair;
    begin+=pairs*2;if(begin<end)row[begin]=color;
}
static unsigned span_coverage(int left,int right,int x){
    int a=left>x*16?left:x*16,b=right<(x+1)*16?right:(x+1)*16;
    return b>a?(unsigned)(b-a):0;
}
static void polygon(renderer_t *r,uint16_t *output,const int16_t *p,unsigned n,uint16_t color,unsigned part){
    memset(r->cut_count,0,sizeof(r->cut_count));
    int first=SUBROWS,last=0;
    for(unsigned i=0;i<n;i++){
        unsigned j=(i+1)%n;
        int ax=p[i*2],ay=p[i*2+1],bx=p[j*2],by=p[j*2+1];
        if(ay==by)continue;
        if(ay>by){int t=ax;ax=bx;bx=t;t=ay;ay=by;by=t;}
        int start=clamp(ceil_div(ay-4,8),0,SUBROWS),end=clamp(ceil_div(by-4,8),0,SUBROWS);
        if(start<first)first=start;
        if(end>last)last=end;
        for(int s=start;s<end;s++){
            int x=ax+(bx-ax)*(s*8+4-ay)/(by-ay);
            unsigned count=r->cut_count[s];
            if(count>=CROSSINGS){r->overflow++;continue;}
            unsigned k=count;
            while(k&&r->cuts[s][k-1]>x){r->cuts[s][k]=r->cuts[s][k-1];k--;}
            r->cuts[s][k]=(int16_t)x;r->cut_count[s]++;
        }
    }
    if(part==2){
        for(int s=0;s<SUBROWS;s++){
            unsigned count=r->cut_count[s];
            r->clip_left[s]=count?r->cuts[s][0]:0;
            r->clip_right[s]=count?r->cuts[s][count-1]:0;
        }
    }
    uint8_t coverage[WIDTH];uint16_t solid=swap16(color);
    for(int y=first/2;y<(last+1)/2;y++){
        // Fill opaque convex spans in pairs; only their edges need AA math.
        if(r->cut_count[y*2]<=2&&r->cut_count[y*2+1]<=2){
            int l[2]={0},h[2]={0};
            for(int sub=0;sub<2;sub++){
                int s=y*2+sub;
                if(r->cut_count[s]==2){l[sub]=r->cuts[s][0];h[sub]=r->cuts[s][1];}
                if(part>=3){if(l[sub]<r->clip_left[s])l[sub]=r->clip_left[s];if(h[sub]>r->clip_right[s])h[sub]=r->clip_right[s];}
                l[sub]=clamp(l[sub],0,WIDTH*16);h[sub]=clamp(h[sub],l[sub],WIDTH*16);
            }
            int a=(l[0]<l[1]?l[0]:l[1])/16,b=((h[0]>h[1]?h[0]:h[1])+15)/16;
            int full_a=((l[0]>l[1]?l[0]:l[1])+15)/16,full_b=(h[0]<h[1]?h[0]:h[1])/16;
            uint16_t *row=output+y*WIDTH;
            if(full_a<full_b){
                fill_span(row,full_a,full_b,solid);
                for(int x=a;x<full_a;x++){unsigned n=span_coverage(l[0],h[0],x)+span_coverage(l[1],h[1],x);if(n)row[x]=covered(row[x],color,n);}
                for(int x=full_b;x<b;x++){unsigned n=span_coverage(l[0],h[0],x)+span_coverage(l[1],h[1],x);if(n)row[x]=covered(row[x],color,n);}
            }else for(int x=a;x<b;x++){unsigned n=span_coverage(l[0],h[0],x)+span_coverage(l[1],h[1],x);if(n)row[x]=covered(row[x],color,n);}
            continue;
        }
        memset(coverage,0,sizeof(coverage));int left_pixel=WIDTH,right_pixel=0;
        for(int sub=0;sub<2;sub++){
            int s=y*2+sub;
            for(unsigned k=0;k+1<r->cut_count[s];k+=2){
                int left=r->cuts[s][k],right=r->cuts[s][k+1];
                if(part>=3){if(left<r->clip_left[s])left=r->clip_left[s];if(right>r->clip_right[s])right=r->clip_right[s];}
                left=clamp(left,0,WIDTH*16);right=clamp(right,0,WIDTH*16);
                if(left>=right)continue;
                int a=left/16,b=(right-1)/16;
                if(a<left_pixel)left_pixel=a;
                if(b+1>right_pixel)right_pixel=b+1;
                if(a==b)coverage[a]+=right-left;
                else {
                    coverage[a]+=16-left%16;coverage[b]+=right-b*16;
                    for(int x=a+1;x<b;x++)coverage[x]+=16;
                }
            }
        }
        uint16_t *row=output+y*WIDTH;
        for(int x=left_pixel;x<right_pixel;x++){
            unsigned amount=coverage[x];
            if(amount>=32)row[x]=solid;
            else if(amount)row[x]=covered(row[x],color,amount);
        }
    }
}

void avatar_render(void *memory,uint16_t *output,uint32_t ms,unsigned pose,unsigned level){
    renderer_t *r=memory;
    unsigned dt=r->initialized?ms-r->last_ms:33;if(dt>200)dt=200;
    if(pose>3)pose=0;
    if(level>1024)level=1024;
    unsigned gain=dt*256/(70+dt);
    r->level=r->initialized?(unsigned)((int)r->level+((int)level-(int)r->level)*(int)gain/256):level;
    target(r,ms,pose,r->level);
    for(unsigned i=0;i<POINTS*2;i++){
        int32_t desired=(int32_t)r->target[i]*256;
        if(!r->initialized)r->current[i]=desired;
        else r->current[i]+=(desired-r->current[i])*(int)gain/256;
        r->shape[i]=(int16_t)(r->current[i]/256);
    }
    r->initialized=true;r->last_ms=ms;
    memcpy(output,r->background,WIDTH*HEIGHT*2);
    unsigned offset=0;
    for(unsigned part=0;part<5;part++){
        polygon(r,output,r->shape+offset*2,counts[part],part<3?0xdba4:0xffff,part);
        offset+=counts[part];
    }
}
