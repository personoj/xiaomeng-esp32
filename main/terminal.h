#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_ops.h"

typedef enum { XY_SLEEP, XY_CONNECTING, XY_LISTENING, XY_THINKING } xy_state_t;
extern volatile xy_state_t xy_state;
extern volatile bool xy_ptt_pressed;
unsigned playback_level(void); /* Actual DMA output level, Q10. */
extern volatile bool xy_speaking;
extern i2c_master_bus_handle_t xy_i2c;
extern i2s_chan_handle_t xy_tx, xy_rx;
void board_init(void);
void display_start(void);
void speech_start(void);
void speech_diagnostics(cJSON *event);
void playback_init(void);
void playback_event(const cJSON *event);
void playback_clear(void);
typedef enum { XY_PROMPT_WAKE, XY_PROMPT_SLEEP } xy_prompt_t;
void playback_prompt(xy_prompt_t prompt);
void playback_prompt_cancel(void);
bool playback_prompt_busy(void); /* Includes a short acoustic tail guard. */
void playback_progress(void);
void playback_diagnostics(cJSON *event);
extern volatile uint32_t xy_display_frames;
extern volatile uint32_t xy_display_decode_us,xy_display_transfer_us;
void terminal_wake(const char *source);
void terminal_sleep(const char *source);
void terminal_receive(const char *json, int length);
void transport_start(void);
void transport_wake(void);
void transport_idle(void);
bool transport_ready(void);
void transport_send(cJSON *event); /* takes ownership */
void transport_pcm(const int16_t *pcm, int samples);
void transport_usb_message(const char *json);
void terminal_status(const char *kind, const char *message);
extern esp_lcd_panel_handle_t xy_panel;
void display_present(const uint16_t *frame);
bool display_wait_te(void);
extern volatile uint32_t xy_te_count,xy_te_timeouts;
