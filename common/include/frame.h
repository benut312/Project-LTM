#ifndef LTM_FRAME_H
#define LTM_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_HEADER_SIZE 4u
#define FRAME_MAX_PAYLOAD 65536u
#define FRAME_DEFAULT_TX_LIMIT (512u * 1024u)
#define FRAME_MAX_QUEUED 256u

enum frame_result {
    FRAME_OK = 0,
    FRAME_INVALID = -1,
    FRAME_NO_MEMORY = -2,
    FRAME_HANDLER_ERROR = -3,
    FRAME_QUEUE_FULL = -4
};

/* Payload is NUL-terminated for convenience, but length always counts bytes.
 * The callback must copy any data it needs after returning. */
typedef int (*frame_handler_fn)(const uint8_t *payload, size_t length,
                                void *context);

typedef struct {
    uint8_t header[FRAME_HEADER_SIZE];
    size_t header_used;
    uint32_t payload_length;
    size_t payload_used;
    uint8_t *payload;
} frame_decoder_t;

typedef struct frame_tx_node frame_tx_node_t;

typedef struct {
    frame_tx_node_t *head;
    frame_tx_node_t *tail;
    size_t allocated_bytes;
    size_t frame_count;
    size_t limit;
} frame_tx_queue_t;

void frame_decoder_init(frame_decoder_t *decoder);
void frame_decoder_destroy(frame_decoder_t *decoder);
int frame_decoder_in_progress(const frame_decoder_t *decoder);
int frame_decoder_feed(frame_decoder_t *decoder, const void *data, size_t length,
                       frame_handler_fn handler, void *context);

void frame_tx_init(frame_tx_queue_t *queue, size_t limit);
void frame_tx_destroy(frame_tx_queue_t *queue);
int frame_tx_push(frame_tx_queue_t *queue, const void *payload, size_t length);
size_t frame_tx_peek(const frame_tx_queue_t *queue, const uint8_t **data);
int frame_tx_advance(frame_tx_queue_t *queue, size_t written);

#endif
