#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t *pending;
    unsigned capacity, used, samples;
    bool down, active, begun, released;
    void (*event)(void *, bool begin, unsigned samples);
    void (*audio)(void *, const int16_t *, unsigned);
    void *context;
} ptt_capture_t;
enum { PTT_STARTED=1, PTT_SUBMITTED=2, PTT_ABORTED=4 };
void ptt_reset(ptt_capture_t *p);
unsigned ptt_process(ptt_capture_t *p, bool pressed, bool ready, const int16_t *pcm, unsigned samples);

typedef struct { bool raw, stable; uint32_t changed_ms; } ptt_button_t;
bool ptt_button_update(ptt_button_t *button, bool raw, uint32_t now_ms);
