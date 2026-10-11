/* Synthetic host CPU microbenchmark. Does not simulate ESP32 lock timing. */
#include "native_process_cpu.h"
#include "firmware_under_test.c"
#include "native_audio_helpers.h"

static volatile uint64_t benchmark_checksum;
int main(void){
    static const unsigned sizes[]={96,960,3840};
    enum { REPEATS=9, BLOCK_ITERATIONS=4000, MAX_ITERATIONS=1024000 };
    /* Windows process accounting can be much coarser than its 100ns unit.
     * Accumulate useful CPU time instead of publishing a quantized/zero short batch. */
    const uint64_t minimum_cpu_ns=UINT64_C(250000000);
    struct { unsigned iterations[REPEATS],locks;double cpu_ns[REPEATS]; } results[3]={0};
    int16_t pcm[3840];for(unsigned i=0;i<3840;i++)pcm[i]=(int16_t)((i*7919)%60001-30000);
    for(unsigned size_index=0;size_index<sizeof(sizes)/sizeof(sizes[0]);size_index++){
        unsigned samples=sizes[size_index];reset_audio();
        int slot=find_response("bench",true);assert(slot==0);
        cJSON *event=make_delta("bench",1,pcm,samples);unsigned expected=samples/3*2;
        for(unsigned repeat=0;repeat<REPEATS;repeat++){
            uint64_t start,end;unsigned iterations=0;native_critical_entries=0;
            if(!native_cpu_now_ns(&start)) {native_cpu_print_error();cJSON_Delete(event);free(ring);return EXIT_FAILURE;}
            do {
                for(unsigned i=0;i<BLOCK_ITERATIONS;i++){
                    rd=wr=0;responses[slot].seq=responses[slot].received=0;responses[slot].ntail=0;
                    playback_event(event);
                    assert(wr==expected&&responses[slot].played==0);
                }
                iterations+=BLOCK_ITERATIONS;
                if(!native_cpu_now_ns(&end)) {native_cpu_print_error();cJSON_Delete(event);free(ring);return EXIT_FAILURE;}
                if(end<start || (end-start<minimum_cpu_ns && iterations==MAX_ITERATIONS)) {
                    fputs("Process CPU clock moved backwards or could not resolve a sufficient batch\n",stderr);
                    cJSON_Delete(event);free(ring);return EXIT_FAILURE;
                }
            } while(end-start<minimum_cpu_ns);
            results[size_index].locks=native_critical_entries/iterations;
            assert(native_critical_entries==results[size_index].locks*iterations);
            results[size_index].iterations[repeat]=iterations;
            results[size_index].cpu_ns[repeat]=(double)(end-start)/iterations;
            /* A checksum per repeat is independent of the adaptive iteration count. */
            benchmark_checksum+=(uint16_t)ring[expected-1].value;
        }
        cJSON_Delete(event);
    }
    /* Emit a report only after every clock read and batch succeeds. */
    printf("{\"kind\":\"synthetic_host_playback_event\",\"clock\":\"%s\","
           "\"minimum_cpu_ns_per_repeat\":%llu,\"locks\":\"instrumented host substitutes, not FreeRTOS\",\"runs\":[\n",
           NATIVE_CPU_TIMER_NAME,(unsigned long long)minimum_cpu_ns);
    for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++){
        printf("%s{\"source_samples\":%u,\"output_samples\":%u,\"iterations_per_repeat\":[",s?",\n":"",sizes[s],sizes[s]/3*2);
        for(unsigned r=0;r<REPEATS;r++)printf("%s%u",r?",":"",results[s].iterations[r]);
        printf("],\"cpu_ns_per_event\":[");
        for(unsigned r=0;r<REPEATS;r++)printf("%s%.3f",r?",":"",results[s].cpu_ns[r]);
        printf("],\"critical_entries_per_event\":%u}",results[s].locks);
    }
    printf("\n],\"checksum\":%llu}\n",(unsigned long long)benchmark_checksum);
    free(ring);return 0;
}
