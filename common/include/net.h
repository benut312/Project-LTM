#ifndef LTM_NET_H
#define LTM_NET_H

#include "frame.h"
#include <stdint.h>

int net_set_nonblocking(int fd);
int net_listen(const char *address, uint16_t port, uint16_t *actual_port);
int net_connect(const char *host, uint16_t port, int timeout_ms);
uint64_t net_now_ms(void);
int net_parse_uint(const char *text, unsigned maximum, unsigned *value);

/* Flush at most budget bytes. EAGAIN retains the queued suffix; EINTR retries.
 * Returns 0 for success/would-block and -1 for fatal transport errors. */
int net_flush(int fd, frame_tx_queue_t *queue, size_t budget, size_t *written);

#endif
