#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#if defined(_WIN32)
#define AVATAR_API __declspec(dllexport)
#else
#define AVATAR_API
#endif
AVATAR_API size_t avatar_renderer_size(void);
AVATAR_API bool avatar_init(void *memory,const uint8_t *asset,size_t length,uint16_t *background);
/* RGB565 output is byte-swapped for the panel; level is 0..1024. */
AVATAR_API void avatar_render(void *memory,uint16_t *output,uint32_t elapsed_ms,unsigned pose,unsigned level);
AVATAR_API unsigned avatar_overflows(void *memory);
