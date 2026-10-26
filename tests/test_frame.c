/*
 * test_frame.c - Kiem thu RX bi chia/gop va TX partial write khong can socket.
 * Chay: make unit-test; assert that bai se dung chuong trinh voi ma loi.
 */
#include "frame.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *expected;
    size_t expected_length;
    size_t calls;
} received_t;

/*
 * collect
 * Y nghia : Assert payload/length va NUL phu, dem frame da nhan.
 * Dau vao : payload/length - callback decoder; context - expected data muon.
 * Dau ra  : 0 neu dung; assert dung chuong trinh neu du lieu sai.
 */
static int collect(const uint8_t *payload, size_t length, void *context)
{
    received_t *received = context;
    assert(length == received->expected_length);
    assert(memcmp(payload, received->expected, length) == 0);
    assert(payload[length] == '\0');
    received->calls++;
    return 0;
}

/*
 * fail_handler
 * Y nghia : Gia lap consumer khong xu ly duoc frame de thu duong cleanup.
 * Dau vao : payload/length/context - callback, khong su dung.
 * Dau ra  : Luon -1; decoder phai tra FRAME_HANDLER_ERROR.
 */
static int fail_handler(const uint8_t *payload, size_t length, void *context)
{
    (void)payload;
    (void)length;
    (void)context;
    return -1;
}

/*
 * fragmented_and_coalesced
 * Y nghia : Thu header tung byte va nhieu frame + frame do trong cung feed.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert dung chuong trinh neu mat/nham byte/frame.
 */
static void fragmented_and_coalesced(void)
{
    static const uint8_t content[] = "{\"text\":\"xin chao\"}";
    frame_decoder_t decoder;
    frame_tx_queue_t queue;
    received_t received = {content, sizeof(content) - 1u, 0};
    const uint8_t *wire;
    size_t wire_length;
    frame_decoder_init(&decoder);
    frame_tx_init(&queue, FRAME_DEFAULT_TX_LIMIT);
    assert(frame_tx_push(&queue, content, sizeof(content) - 1u) == FRAME_OK);
    wire_length = frame_tx_peek(&queue, &wire);
    /* Exercise every boundary inside the header and payload. */
    for (size_t i = 0; i < wire_length; ++i) {
        assert(frame_decoder_feed(&decoder, wire + i, 1, collect, &received) == FRAME_OK);
    }
    assert(received.calls == 1);
    assert(!frame_decoder_in_progress(&decoder));

    uint8_t *combined = malloc(wire_length * 3u);
    assert(combined != NULL);
    for (size_t i = 0; i < 3; ++i) {
        memcpy(combined + i * wire_length, wire, wire_length);
    }
    /* One read contains two whole frames and part of a third. */
    assert(frame_decoder_feed(&decoder, combined, wire_length * 2u + 2u,
                              collect, &received) == FRAME_OK);
    assert(received.calls == 3);
    assert(frame_decoder_in_progress(&decoder));
    assert(frame_decoder_feed(&decoder, combined + wire_length * 2u + 2u,
                              wire_length - 2u, collect, &received) == FRAME_OK);
    assert(received.calls == 4);
    assert(!frame_decoder_in_progress(&decoder));
    free(combined);
    frame_tx_destroy(&queue);
    frame_decoder_destroy(&decoder);
}

/*
 * maximum_and_invalid_lengths
 * Y nghia : Thu frame max, big endian, length 0/qua lon va callback that bai.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert neu sai gioi han hoac ma loi.
 */
static void maximum_and_invalid_lengths(void)
{
    frame_decoder_t decoder;
    frame_tx_queue_t queue;
    uint8_t *payload = malloc(FRAME_MAX_PAYLOAD);
    const uint8_t *wire;
    size_t wire_length;
    assert(payload != NULL);
    memset(payload, 'x', FRAME_MAX_PAYLOAD);
    received_t received = {payload, FRAME_MAX_PAYLOAD, 0};
    frame_decoder_init(&decoder);
    frame_tx_init(&queue, FRAME_DEFAULT_TX_LIMIT);
    assert(frame_tx_push(&queue, payload, FRAME_MAX_PAYLOAD) == FRAME_OK);
    wire_length = frame_tx_peek(&queue, &wire);
    assert(wire_length == FRAME_MAX_PAYLOAD + FRAME_HEADER_SIZE);
    assert(wire[0] == 0 && wire[1] == 1 && wire[2] == 0 && wire[3] == 0);
    assert(frame_decoder_feed(&decoder, wire, wire_length, collect, &received) == FRAME_OK);
    assert(received.calls == 1);
    const uint8_t zero[] = {0, 0, 0, 0};
    const uint8_t oversize[] = {0, 1, 0, 1};
    const uint8_t huge[] = {255, 255, 255, 255};
    assert(frame_decoder_feed(&decoder, zero, 4, collect, &received) == FRAME_INVALID);
    assert(frame_decoder_feed(&decoder, oversize, 4, collect, &received) == FRAME_INVALID);
    assert(frame_decoder_feed(&decoder, huge, 4, collect, &received) == FRAME_INVALID);
    assert(frame_tx_push(&queue, payload, 0) == FRAME_INVALID);
    assert(frame_tx_push(&queue, payload, FRAME_MAX_PAYLOAD + 1u) == FRAME_INVALID);
    assert(frame_decoder_feed(&decoder, wire, wire_length, fail_handler, NULL) ==
           FRAME_HANDLER_ERROR);
    frame_decoder_destroy(&decoder);
    frame_tx_destroy(&queue);
    free(payload);
}

/*
 * partial_writes_and_backpressure
 * Y nghia : Gia lap moi write chi gui 2 byte; thu byte cap va frame-count cap.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert neu wire data/ownership/gioi han khong dung.
 */
static void partial_writes_and_backpressure(void)
{
    static const uint8_t payload[] = "abc";
    frame_tx_queue_t queue;
    const uint8_t *wire;
    uint8_t captured[14];
    size_t used = 0;
    frame_tx_init(&queue, 14);
    assert(frame_tx_push(&queue, payload, 3) == FRAME_OK);
    assert(frame_tx_push(&queue, payload, 3) == FRAME_OK);
    assert(frame_tx_push(&queue, payload, 3) == FRAME_QUEUE_FULL);
    assert(frame_tx_advance(&queue, 8) == FRAME_INVALID);
    while (frame_tx_peek(&queue, &wire) != 0) {
        size_t available = frame_tx_peek(&queue, &wire);
        /* Model a transport which accepts at most two bytes per write. */
        size_t written = available > 2 ? 2 : available;
        memcpy(captured + used, wire, written);
        used += written;
        assert(frame_tx_advance(&queue, written) == FRAME_OK);
    }
    const uint8_t expected[] = {0, 0, 0, 3, 'a', 'b', 'c',
                                0, 0, 0, 3, 'a', 'b', 'c'};
    assert(used == sizeof(expected));
    assert(memcmp(captured, expected, sizeof(expected)) == 0);
    assert(queue.frame_count == 0 && queue.allocated_bytes == 0);
    assert(frame_tx_advance(&queue, 1) == FRAME_INVALID);
    frame_tx_destroy(&queue);

    frame_tx_init(&queue, FRAME_DEFAULT_TX_LIMIT);
    for (size_t i = 0; i < FRAME_MAX_QUEUED; ++i) {
        assert(frame_tx_push(&queue, payload, 3) == FRAME_OK);
    }
    assert(frame_tx_push(&queue, payload, 3) == FRAME_QUEUE_FULL);
    frame_tx_destroy(&queue);
}

/*
 * main
 * Y nghia : Chay cac ca frame va thong bao ket qua.
 * Dau vao : Khong co.
 * Dau ra  : 0 khi moi assert dung; process that bai neu bat ky assert sai.
 */
int main(void)
{
    fragmented_and_coalesced();
    maximum_and_invalid_lengths();
    partial_writes_and_backpressure();
    puts("frame tests: PASS");
    return 0;
}
