/* Real pool expiry/queue maintenance; queue operations are deterministic fakes.
 * Single-thread scheduling hooks inject the shutdown reset and append races. */
#include <stdlib.h>
#include <stdio.h>
#include "../../components/telemetry/telemetry.c"
#include "pool_unused_dependencies.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
struct test_queue { void *items[32]; unsigned count; };
static bool reset_during_receive;
static struct test_queue free_q, event_q, gps_q, ready_q;
static telemetry_entry_t pool[16];
int64_t esp_timer_get_time(void) { return 20000001; }
uint32_t gps_batch_message_sequence(const gps_batch_t *batch) { return 1; }
int xQueueSend(QueueHandle_t q, const void *p, TickType_t t)
{
    if (q->count == 32) return pdFALSE;
    q->items[q->count++] = *(void * const *)p; return pdTRUE;
}
int xQueuePeek(QueueHandle_t q, void *p, TickType_t t)
{
    if (!q->count) return pdFALSE;
    *(void **)p = q->items[0]; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *p, TickType_t t)
{
    if (reset_during_receive) {
        reset_during_receive = false;
        s_abort_requested = true; q->count = 0; return pdFALSE;
    }
    if (!xQueuePeek(q,p,t)) return pdFALSE;
    q->count--;
    for (unsigned i=0; i<q->count; i++) q->items[i]=q->items[i+1];
    return pdTRUE;
}
static void fixture(void)
{
    memset(pool, 0, sizeof(pool)); memset(&s_status, 0, sizeof(s_status));
    memset(&s_config, 0, sizeof(s_config));
    free_q.count = event_q.count = gps_q.count = ready_q.count = 0;
    s_pool=pool; s_config.pool_length=16; s_config.max_residency_ms=10000;
    s_free_queue=&free_q; s_event_queue=&event_q; s_gps_queue=&gps_q; s_ready_queue=&ready_q;
    s_abort_requested=false; s_status.healthy=true; reset_during_receive=false;
    s_pending_gps_batch=NULL; s_pending_reflectance=NULL;
}
static void admit(unsigned i, unsigned type, int64_t age, QueueHandle_t q)
{
    telemetry_entry_t *p=&pool[i];
    p->state=ENTRY_QUEUED; p->message_type=type; p->admitted_us=20000001-age;
    if (type==TELEMETRY_MESSAGE_GPS_BATCH) p->record.gps_batch.record_count=10;
    s_status.pool_used++; xQueueSend(q,&p,0);
}
static int pool_case(unsigned which)
{
    fixture();
    if (which == 0) {
        /* Outage lasts far longer than TTL. Repeated admissions keep working. */
        for (unsigned round=0; round<100; round++) {
            free_q.count=0;
            admit(0,TELEMETRY_MESSAGE_OPERATION_LOG,15000000,&event_q);
            admit(1,TELEMETRY_MESSAGE_GPS_BATCH,15000000,&gps_q);
            admit(2,TELEMETRY_MESSAGE_REFLECTANCE,15000000,&ready_q);
            expire_stale_entries(20000001); reclaim_expired_queues();
            CHECK(s_status.pool_used==0 && free_q.count==3);
            CHECK(!event_q.count && !gps_q.count && !ready_q.count);
        }
        CHECK(s_status.events_expired==100 && s_status.gps_expired==1000);
        CHECK(s_status.reflectance_expired==100 && s_status.healthy);
    } else if (which == 1) {
        admit(0,3,15000000,&ready_q); admit(1,3,1000000,&ready_q);
        admit(2,3,15000000,&ready_q);
        expire_stale_entries(20000001); reclaim_expired_queues();
        CHECK(free_q.count==1 && ready_q.count==2 && ready_q.items[0]==&pool[1]);
        CHECK(pool[1].state==ENTRY_QUEUED && pool[2].state==ENTRY_EXPIRED);
        /* Later expiry clears the remaining prefix without reordering. */
        expire_stale_entries(30000001); reclaim_expired_queues();
        CHECK(s_status.pool_used==0 && free_q.count==3 && ready_q.count==0);
    } else if (which == 2) {
        admit(0,3,15000000,&ready_q); expire_stale_entries(20000001);
        reset_during_receive=true; reclaim_expired_queues();
        CHECK(s_abort_requested && s_status.healthy && free_q.count==0);
        purge_all_entries(); CHECK(s_status.pool_used==0 && free_q.count==1);
    } else {
        admit(0,3,15000000,&ready_q); pool[0].state=ENTRY_ENQUEUING;
        expire_stale_entries(20000001); reclaim_expired_queues();
        CHECK(pool[0].state==ENTRY_ENQUEUING && free_q.count==0);
        pool[0].state=ENTRY_QUEUED;
        expire_stale_entries(20000001); reclaim_expired_queues();
        CHECK(s_status.pool_used==0 && free_q.count==1 && s_status.healthy);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    int line = pool_case(atoi(argv[2]));
    if (line) fprintf(stderr, "C assertion failed at line %d\n", line);
    return line ? 1 : 0;
}
