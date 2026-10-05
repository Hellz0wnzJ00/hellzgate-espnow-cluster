#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY 0xffffffffu

// one thread in the test, so a critical section has nothing to keep out
#define portENTER_CRITICAL(m)     ((void)(m))
#define portEXIT_CRITICAL(m)      ((void)(m))
#define portENTER_CRITICAL_ISR(m) ((void)(m))
#define portEXIT_CRITICAL_ISR(m)  ((void)(m))
#define portENTER_CRITICAL_SAFE(m) ((void)(m))
#define portEXIT_CRITICAL_SAFE(m)  ((void)(m))

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif
