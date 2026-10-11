/* Synthetic host CPU microbenchmark. Does not simulate ESP32 lock timing. */
#include "firmware_under_test.c"
#include "native_audio_helpers.h"
#include <time.h>

static volatile uint64_t benchmark_checksum;
int main(void){
    static const unsigned sizes[]={96,960,3840};
    enum { REPEATS=9, ITERATIONS=4000 };
    int16_t pcm[3840];for(unsigned i=0;i<3840;i++)pcm[i]=(int16_t)((i*7919)%60001-30000);
    puts("{\"kind\":\"synthetic_host_playback_event\",\"clock\":\"C clock() process CPU\","
         "\"locks\":\"instrumented host substitutes, not FreeRTOS\",\"runs\":[");
    for(unsigned size_index=0;size_index<sizeof(sizes)/sizeof(sizes[0]);size_index++){
        unsigned samples=sizes[size_index];reset_audio();
        int slot=find_response("bench",true);assert(slot==0);
        cJSON *event=make_delta("bench",1,pcm,samples);unsigned expected=samples/3*2;
        printf("%s{\"source_samples\":%u,\"output_samples\":%u,\"iterations_per_repeat\":%u,\"cpu_ns_per_event\":[",
               size_index?",\n":"",samples,expected,ITERATIONS);
        unsigned locks=0;
        for(unsigned repeat=0;repeat<REPEATS;repeat++){
            native_critical_entries=0;clock_t start=clock();
            for(unsigned i=0;i<ITERATIONS;i++){
                rd=wr=0;responses[slot].seq=responses[slot].received=0;responses[slot].ntail=0;
                playback_event(event);
                assert(wr==expected&&responses[slot].played==0);
                benchmark_checksum+=(uint16_t)ring[expected-1].value;
            }
            clock_t end=clock();assert(end>start);
            locks=native_critical_entries/ITERATIONS;
            assert(native_critical_entries==locks*ITERATIONS);
            printf("%s%.3f",repeat?",":"",(double)(end-start)*1e9/CLOCKS_PER_SEC/ITERATIONS);
        }
        printf("],\"critical_entries_per_event\":%u}",locks);cJSON_Delete(event);
    }
    printf("\n],\"checksum\":%llu}\n",(unsigned long long)benchmark_checksum);
    free(ring);return 0;
}
