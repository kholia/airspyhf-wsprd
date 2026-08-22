#include "sample_queue.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
    return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

bool sample_queue_init(struct sample_queue *queue,
                       size_t element_size,
                       uint32_t block_capacity,
                       size_t slot_count)
{
    size_t block_bytes;

    if (queue == NULL || element_size == 0 || block_capacity == 0 || slot_count == 0 ||
        element_size > SIZE_MAX / block_capacity) {
        return false;
    }
    block_bytes = element_size * block_capacity;
    if (slot_count > SIZE_MAX / block_bytes) {
        return false;
    }

    memset(queue, 0, sizeof(*queue));
    queue->slots = calloc(slot_count, sizeof(*queue->slots));
    queue->storage = malloc(slot_count * block_bytes);
    if (queue->slots == NULL || queue->storage == NULL) {
        free(queue->slots);
        free(queue->storage);
        memset(queue, 0, sizeof(*queue));
        return false;
    }
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->slots);
        free(queue->storage);
        memset(queue, 0, sizeof(*queue));
        return false;
    }
    if (pthread_cond_init(&queue->ready, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->slots);
        free(queue->storage);
        memset(queue, 0, sizeof(*queue));
        return false;
    }

    queue->element_size = element_size;
    queue->block_capacity = block_capacity;
    queue->slot_count = slot_count;
    for (size_t index = 0; index < slot_count; index++) {
        queue->slots[index].samples = (unsigned char *)queue->storage + index * block_bytes;
    }
    return true;
}

void sample_queue_destroy(struct sample_queue *queue)
{
    if (queue == NULL || queue->slots == NULL) {
        return;
    }
    pthread_cond_destroy(&queue->ready);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->storage);
    free(queue->slots);
    memset(queue, 0, sizeof(*queue));
}

void sample_queue_reset(struct sample_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->pending_device_drops = 0;
    queue->pending_queue_drops = 0;
    pthread_mutex_unlock(&queue->mutex);
}

void sample_queue_stop(struct sample_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopping = true;
    pthread_cond_broadcast(&queue->ready);
    pthread_mutex_unlock(&queue->mutex);
}

bool sample_queue_push(struct sample_queue *queue,
                       const void *samples,
                       uint32_t sample_count,
                       uint64_t device_dropped_samples)
{
    struct sample_queue_slot *slot;

    if (queue == NULL || samples == NULL || sample_count == 0) {
        return false;
    }

    pthread_mutex_lock(&queue->mutex);
    if (queue->stopping) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    if (sample_count > queue->block_capacity || queue->count == queue->slot_count) {
        queue->pending_device_drops = saturating_add(
            queue->pending_device_drops, device_dropped_samples);
        queue->pending_queue_drops = saturating_add(
            queue->pending_queue_drops, sample_count);
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }

    slot = &queue->slots[queue->head];
    memcpy(slot->samples, samples, sample_count * queue->element_size);
    slot->sample_count = sample_count;
    slot->device_dropped_samples = saturating_add(
        queue->pending_device_drops, device_dropped_samples);
    slot->queue_dropped_samples = queue->pending_queue_drops;
    queue->pending_device_drops = 0;
    queue->pending_queue_drops = 0;
    queue->head = (queue->head + 1) % queue->slot_count;
    queue->count++;
    pthread_cond_signal(&queue->ready);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

bool sample_queue_wait(struct sample_queue *queue)
{
    bool available;

    pthread_mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->stopping) {
        pthread_cond_wait(&queue->ready, &queue->mutex);
    }
    available = queue->count != 0 && !queue->stopping;
    pthread_mutex_unlock(&queue->mutex);
    return available;
}

bool sample_queue_try_peek(struct sample_queue *queue,
                           struct sample_queue_slot *slot)
{
    bool available;

    pthread_mutex_lock(&queue->mutex);
    available = queue->count != 0 && !queue->stopping;
    if (available) {
        *slot = queue->slots[queue->tail];
    }
    pthread_mutex_unlock(&queue->mutex);
    return available;
}

bool sample_queue_release(struct sample_queue *queue)
{
    bool released = false;

    pthread_mutex_lock(&queue->mutex);
    if (queue->count != 0) {
        queue->tail = (queue->tail + 1) % queue->slot_count;
        queue->count--;
        released = true;
    }
    pthread_mutex_unlock(&queue->mutex);
    return released;
}
