/* Fail a benchmark clock read after work starts: no timing report may escape. */
#include "native_process_cpu.h"
static bool fail_after_first_read(uint64_t *value) {
    static unsigned reads;
    if(reads++==0)return native_cpu_now_ns(value);
#ifdef _WIN32
    SetLastError(ERROR_ACCESS_DENIED);
#else
    errno=EIO;
#endif
    return false;
}
#define native_cpu_now_ns fail_after_first_read
#include "native_playback_bench.c"
