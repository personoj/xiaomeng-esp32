/* Host-only process CPU clock. Include before other headers on POSIX. */
#ifndef XY_NATIVE_PROCESS_CPU_H
#define XY_NATIVE_PROCESS_CPU_H
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#ifndef NATIVE_CPU_GET_PROCESS_TIMES
#define NATIVE_CPU_GET_PROCESS_TIMES GetProcessTimes
#endif
#define NATIVE_CPU_TIMER_NAME "GetProcessTimes (kernel + user)"
static bool native_cpu_now_ns(uint64_t *value) {
    FILETIME created,exited,kernel,user;
    if(!NATIVE_CPU_GET_PROCESS_TIMES(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
    uint64_t k=((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime;
    uint64_t u=((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime;
    if(k>UINT64_MAX-u || k+u>UINT64_MAX/100) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);return false;
    }
    *value=(k+u)*100; /* FILETIME units are 100ns, not the clock's effective resolution. */
    return true;
}
static void native_cpu_print_error(void) {
    fprintf(stderr,"%s failed (Windows error %lu)\n",NATIVE_CPU_TIMER_NAME,(unsigned long)GetLastError());
}
#else
#include <errno.h>
#include <time.h>
#ifndef CLOCK_PROCESS_CPUTIME_ID
#error "This benchmark requires a POSIX process CPU clock; elapsed-time fallbacks are not supported."
#endif
#ifndef NATIVE_CPU_CLOCK_GETTIME
#define NATIVE_CPU_CLOCK_GETTIME clock_gettime
#endif
#define NATIVE_CPU_TIMER_NAME "clock_gettime(CLOCK_PROCESS_CPUTIME_ID)"
static bool native_cpu_now_ns(uint64_t *value) {
    struct timespec stamp;
    if(NATIVE_CPU_CLOCK_GETTIME(CLOCK_PROCESS_CPUTIME_ID,&stamp)!=0)return false;
    if(stamp.tv_sec<0 || stamp.tv_nsec<0 || stamp.tv_nsec>=1000000000L ||
       (uint64_t)stamp.tv_sec>(UINT64_MAX-(uint64_t)stamp.tv_nsec)/1000000000) {
        errno=EOVERFLOW;return false;
    }
    *value=(uint64_t)stamp.tv_sec*1000000000+(uint64_t)stamp.tv_nsec;
    return true;
}
static void native_cpu_print_error(void) {
    fprintf(stderr,"%s failed (errno %d)\n",NATIVE_CPU_TIMER_NAME,errno);
}
#endif
#endif
