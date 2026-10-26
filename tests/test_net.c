/*
 * test_net.c - Ep socket nonblocking gap EAGAIN va kiem tra TX khong mat byte.
 * Socketpair local giup kiem thu partial I/O thuc, khong phu thuoc mang ngoai.
 */
#include "frame.h"
#include "net.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * short_write_and_eagain
 * Y nghia : Dung send buffer nho, gui frame lon, drain tung phan va so wire byte.
 * Dau vao : Khong co.
 * Dau ra  : Khong tra ve; assert neu suffix mat/sai thu tu hoac khong gap EAGAIN.
 */
static void short_write_and_eagain(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_set_nonblocking(sockets[0]) == 0 && net_set_nonblocking(sockets[1]) == 0);
    int size = 1024;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    uint8_t *payload = malloc(FRAME_MAX_PAYLOAD);
    uint8_t *received = malloc(FRAME_MAX_PAYLOAD + FRAME_HEADER_SIZE);
    assert(payload != NULL && received != NULL);
    for (size_t i = 0; i < FRAME_MAX_PAYLOAD; ++i) payload[i] = (uint8_t)(i % 251u);
    frame_tx_queue_t queue;
    frame_tx_init(&queue, FRAME_DEFAULT_TX_LIMIT);
    assert(frame_tx_push(&queue, payload, FRAME_MAX_PAYLOAD) == FRAME_OK);
    size_t written = 0;
    assert(net_flush(sockets[0], &queue, FRAME_MAX_PAYLOAD + 4u, &written) == 0);
    assert(written > 0 && written < FRAME_MAX_PAYLOAD + 4u && queue.head != NULL);
    assert(net_flush(sockets[0], &queue, FRAME_MAX_PAYLOAD + 4u, &written) == 0);
    assert(written == 0); /* Peer chua doc: send buffer phai dang day. */

    size_t used = 0;
    uint64_t deadline = net_now_ms() + 3000;
    while (used < FRAME_MAX_PAYLOAD + 4u && net_now_ms() < deadline) {
        ssize_t count = recv(sockets[1], received + used, FRAME_MAX_PAYLOAD + 4u - used, 0);
        if (count > 0) used += (size_t)count;
        else assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        assert(net_flush(sockets[0], &queue, 4096, &written) == 0);
    }
    assert(used == FRAME_MAX_PAYLOAD + 4u && queue.head == NULL);
    const uint8_t header[] = {0, 1, 0, 0};
    assert(memcmp(received, header, 4) == 0);
    assert(memcmp(received + 4, payload, FRAME_MAX_PAYLOAD) == 0);
    frame_tx_destroy(&queue);
    /* Gui vao peer da close phai tra loi, khong lam process chet vi SIGPIPE. */
    close(sockets[1]);
    assert(frame_tx_push(&queue, payload, 10) == FRAME_OK);
    assert(net_flush(sockets[0], &queue, 64, &written) == -1);
    frame_tx_destroy(&queue);
    close(sockets[0]);
    free(payload);
    free(received);
}

/*
 * main
 * Y nghia : Thu parse tham so va partial socket I/O; thong bao PASS.
 * Dau vao : Khong co.
 * Dau ra  : 0 neu moi assert dung, process that bai khi assert sai.
 */
int main(void)
{
    unsigned value = 0;
    assert(net_parse_uint("9090", 65535, &value) == 0 && value == 9090);
    assert(net_parse_uint("-1", 65535, &value) == -1);
    assert(net_parse_uint("1x", 65535, &value) == -1);
    assert(net_parse_uint("65536", 65535, &value) == -1);
    short_write_and_eagain();
    puts("network tests: PASS (short write / EAGAIN / SIGPIPE)");
    return 0;
}
