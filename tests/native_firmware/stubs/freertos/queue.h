#pragma once
#include "FreeRTOS.h"
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
int xQueuePeek(QueueHandle_t, void *, TickType_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
int xQueueReset(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
