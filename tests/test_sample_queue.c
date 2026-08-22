#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "sample_queue.h"

struct wait_context {
    struct sample_queue *queue;
    bool result;
};

static void *wait_for_queue(void *argument)
{
    struct wait_context *context = argument;

    context->result = sample_queue_wait(context->queue);
    return NULL;
}

int main(void)
{
    struct sample_queue queue;
    struct sample_queue_slot slot;
    struct wait_context context = {0};
    pthread_t waiter;
    const uint32_t first[] = {1, 2, 3, 4};
    const uint32_t second[] = {5, 6};
    const uint32_t lost[] = {7, 8, 9};
    const uint32_t recovered[] = {10};

    assert(sample_queue_init(&queue, sizeof(uint32_t), 4, 2));
    assert(sample_queue_push(&queue, first, 4, 0));
    assert(sample_queue_push(&queue, second, 2, 11));
    assert(!sample_queue_push(&queue, lost, 3, 13));

    assert(sample_queue_try_peek(&queue, &slot));
    assert(slot.sample_count == 4);
    assert(slot.device_dropped_samples == 0);
    assert(slot.queue_dropped_samples == 0);
    assert(((uint32_t *)slot.samples)[3] == 4);
    assert(sample_queue_release(&queue));

    assert(sample_queue_try_peek(&queue, &slot));
    assert(slot.sample_count == 2);
    assert(slot.device_dropped_samples == 11);
    assert(sample_queue_release(&queue));

    assert(sample_queue_push(&queue, recovered, 1, 17));
    assert(sample_queue_try_peek(&queue, &slot));
    assert(slot.sample_count == 1);
    assert(slot.device_dropped_samples == 30);
    assert(slot.queue_dropped_samples == 3);
    assert(((uint32_t *)slot.samples)[0] == 10);
    assert(sample_queue_release(&queue));

    sample_queue_reset(&queue);
    assert(!sample_queue_try_peek(&queue, &slot));
    context.queue = &queue;
    assert(pthread_create(&waiter, NULL, wait_for_queue, &context) == 0);
    assert(sample_queue_push(&queue, recovered, 1, 0));
    assert(pthread_join(waiter, NULL) == 0);
    assert(context.result);
    assert(sample_queue_try_peek(&queue, &slot));
    assert(sample_queue_release(&queue));
    sample_queue_stop(&queue);
    assert(!sample_queue_wait(&queue));
    sample_queue_destroy(&queue);
    puts("sample queue: ordering and overrun accounting verified");
    return 0;
}
