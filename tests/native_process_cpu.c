/* Real clock semantics plus deterministic OS failure/conversion checks. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static enum { REAL_TIME, FAILED_READ, FIXED_TIME, OVERFLOW_TIME } read_mode;
#ifdef _WIN32
#include <windows.h>
static BOOL test_get_process_times(HANDLE process,LPFILETIME created,LPFILETIME exited,LPFILETIME kernel,LPFILETIME user) {
    if(read_mode==FAILED_READ) {SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    if(read_mode!=REAL_TIME) {
        *created=*exited=(FILETIME){0,0};
        *kernel=(FILETIME){1234,read_mode==OVERFLOW_TIME?UINT32_MAX:0};
        *user=(FILETIME){5678,0};return TRUE;
    }
    return GetProcessTimes(process,created,exited,kernel,user);
}
#define NATIVE_CPU_GET_PROCESS_TIMES test_get_process_times
static uint64_t wall_ns(void) {
    LARGE_INTEGER now,frequency;
    assert(QueryPerformanceCounter(&now)&&QueryPerformanceFrequency(&frequency));
    return (uint64_t)((double)now.QuadPart*1e9/(double)frequency.QuadPart);
}
static void sleep_for_test(void) {Sleep(300);}
#else
#include <errno.h>
#include <time.h>
static int test_clock_gettime(clockid_t which,struct timespec *stamp) {
    assert(which==CLOCK_PROCESS_CPUTIME_ID);
    if(read_mode==FAILED_READ) {errno=EIO;return -1;}
    if(read_mode!=REAL_TIME) {
        *stamp=(struct timespec){2,read_mode==OVERFLOW_TIME?1000000000L:3456789};return 0;
    }
    return clock_gettime(which,stamp);
}
#define NATIVE_CPU_CLOCK_GETTIME test_clock_gettime
static uint64_t wall_ns(void) {
    struct timespec stamp;assert(clock_gettime(CLOCK_MONOTONIC,&stamp)==0);
    return (uint64_t)stamp.tv_sec*1000000000+(uint64_t)stamp.tv_nsec;
}
static void sleep_for_test(void) {
    struct timespec remaining={0,300000000};
    while(nanosleep(&remaining,&remaining)!=0)assert(errno==EINTR);
}
#endif
#include "native_process_cpu.h"

static volatile uint64_t busy_value=1;
int main(void) {
    uint64_t value=42;
    read_mode=FAILED_READ;
    assert(!native_cpu_now_ns(&value)&&value==42);
    read_mode=OVERFLOW_TIME;
    assert(!native_cpu_now_ns(&value)&&value==42);
    read_mode=FIXED_TIME;assert(native_cpu_now_ns(&value));
#ifdef _WIN32
    assert(value==(1234+5678)*UINT64_C(100));
#else
    assert(value==UINT64_C(2003456789));
#endif
    read_mode=REAL_TIME;
    uint64_t start,end,deadline=wall_ns()+UINT64_C(10000000000);
    assert(native_cpu_now_ns(&start));
    do {
        for(unsigned i=0;i<10000;i++)busy_value=busy_value*1664525+1013904223;
        assert(native_cpu_now_ns(&end)&&end>=start);
        if(wall_ns()>deadline) {fputs("CPU clock did not advance during work within 10s\n",stderr);return EXIT_FAILURE;}
    } while(end-start<UINT64_C(30000000));
    /* Zero/unchanged reads are legal at a coarse resolution, but work must accrue CPU. */
    uint64_t wall_start=wall_ns();assert(native_cpu_now_ns(&start));
    sleep_for_test();
    assert(native_cpu_now_ns(&end)&&end>=start);
    uint64_t elapsed=wall_ns()-wall_start,used=end-start;
    /* A relative, wide margin tolerates timer quantization and scheduling delays.
     * A wall-clock fallback would charge essentially the whole sleep and fail. */
    assert(elapsed>0&&used<elapsed/4);
    printf("PASS: %s; busy work accrues CPU; sleep CPU/wall %.6f; failed/invalid reads preserve output\n",
           NATIVE_CPU_TIMER_NAME,(double)used/elapsed);
    return 0;
}
