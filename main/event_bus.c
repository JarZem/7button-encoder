#include "event_bus.h"
#include "esp_log.h"
#include "fatal_error.h"

#define EVENT_QUEUE_LENGTH 32

static const char *TAG = "event_bus";
static QueueHandle_t s_queue;

void event_bus_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(input_event_t));
    FATAL_ERROR_IF(s_queue == NULL, "Nelze vytvořit frontu událostí");
}

bool event_bus_publish(const input_event_t *event)
{
    return s_queue != NULL && xQueueSend(s_queue, event, 0) == pdTRUE;
}

bool event_bus_receive(input_event_t *event, TickType_t timeout)
{
    return s_queue != NULL && xQueueReceive(s_queue, event, timeout) == pdTRUE;
}
