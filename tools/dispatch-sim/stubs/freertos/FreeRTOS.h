#pragma once
#include <cstdint>
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
