#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct sample_queue_slot {
    void *samples;
    uint32_t sample_count;
    uint64_t device_dropped_samples;
    uint64_t queue_dropped_samples;
};

struct sample_queue {
    struct sample_queue_slot *slots;
    void *storage;
    size_t element_size;
    uint32_t block_capacity;
    size_t slot_count;
    size_t head;
    size_t tail;
    size_t count;
    uint64_t pending_device_drops;
    uint64_t pending_queue_drops;
    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
};

bool sample_queue_init(struct sample_queue *queue,
                       size_t element_size,
                       uint32_t block_capacity,
                       size_t slot_count);
void sample_queue_destroy(struct sample_queue *queue);
void sample_queue_reset(struct sample_queue *queue);
void sample_queue_stop(struct sample_queue *queue);

/* Never waits. A false result means the block was counted as a queue overrun. */
bool sample_queue_push(struct sample_queue *queue,
                       const void *samples,
                       uint32_t sample_count,
                       uint64_t device_dropped_samples);

/* Wait for work, then peek and release one slot while the consumer owns it. */
bool sample_queue_wait(struct sample_queue *queue);
bool sample_queue_try_peek(struct sample_queue *queue,
                           struct sample_queue_slot *slot);
bool sample_queue_release(struct sample_queue *queue);
