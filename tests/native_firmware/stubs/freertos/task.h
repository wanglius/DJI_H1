#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
void vTaskDelay(TickType_t);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned,
                                 void *, unsigned, TaskHandle_t *, int);
