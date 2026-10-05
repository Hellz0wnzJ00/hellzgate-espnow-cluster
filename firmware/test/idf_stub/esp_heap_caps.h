#ifndef ESP_HEAP_CAPS_H
#define ESP_HEAP_CAPS_H

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0

static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t size, int caps) { (void)caps; return calloc(n, size); }
static inline void heap_caps_free(void *p) { free(p); }

#endif
