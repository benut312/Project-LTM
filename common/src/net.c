/*
 * net.c - Helper POSIX TCP nonblocking tren Linux.
 * Luong server: socket -> nonblocking -> bind -> listen.
 * Luong client: resolve -> connect -> poll voi timeout -> SO_ERROR.
 * Gui du lieu qua frame TX queue de giu lai phan short write/EAGAIN.
 */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * net_set_nonblocking
 * Y nghia : Bat O_NONBLOCK va FD_CLOEXEC, giu cac flag da ton tai.
 * Dau vao : fd - file descriptor hop le, caller van so huu.
 * Dau ra  : 0 thanh cong, -1 loi va errno tu fcntl; khong tu close fd.
 */
int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/*
 * net_now_ms
 * Y nghia : Doc dong ho monotonic de tinh deadline khong phu thuoc gio he thong.
 * Dau vao : Khong co.
 * Dau ra  : Millisecond monotonic; thoat that bai neu clock_gettime loi.
 */
uint64_t net_now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

/*
 * net_parse_uint
 * Y nghia : Parse so nguyen thap phan, cam dau am/duong va trailing character.
 * Dau vao : text - chuoi; maximum - gia tri lon nhat; value - bien dau ra.
 * Dau ra  : 0 va gan *value neu hop le; -1 neu rong/sai/overflow/qua maximum.
 */
int net_parse_uint(const char *text, unsigned maximum, unsigned *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL || *text == '\0') {
        return -1;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') {
            return -1;
        }
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > maximum) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

/*
 * net_listen
 * Y nghia : Tao listener IPv4 nonblocking, bat SO_REUSEADDR, bind va listen.
 * Dau vao : address - IPv4 dang so; port - cong (0 de OS chon); actual_port -
 *           noi nhan cong thuc te sau bind.
 * Dau ra  : fd >= 0 (caller close) hoac -1; socket tao do dang duoc cleanup.
 */
int net_listen(const char *address, uint16_t port, uint16_t *actual_port)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    char service[6];
    int listener = -1;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    int status = getaddrinfo(address, service, &hints, &addresses);
    if (status != 0) {
        fprintf(stderr, "bind address: %s\n", gai_strerror(status));
        return -1;
    }
    for (const struct addrinfo *item = addresses; item != NULL; item = item->ai_next) {
        int enabled = 1;
        listener = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (listener < 0) {
            continue;
        }
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0 &&
            net_set_nonblocking(listener) == 0 &&
            bind(listener, item->ai_addr, item->ai_addrlen) == 0 && listen(listener, 128) == 0) {
            break;
        }
        close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener >= 0) {
        struct sockaddr_in bound;
        socklen_t size = sizeof(bound);
        if (getsockname(listener, (struct sockaddr *)&bound, &size) < 0) {
            close(listener);
            return -1;
        }
        *actual_port = ntohs(bound.sin_port);
    }
    return listener;
}

/*
 * net_connect
 * Y nghia : Resolve IPv4 va connect nonblocking, cho POLLOUT roi check SO_ERROR.
 * Dau vao : host - IP/hostname; port - cong dich; timeout_ms - han cho connect.
 * Dau ra  : fd da ket noi >= 0 (caller close), -1 neu loi/timeout.
 *           getaddrinfo dong bo chi dung o client terminal, ngoai GUI loop.
 */
int net_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    char service[6];
    int connected = -1;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    int status = getaddrinfo(host, service, &hints, &addresses);
    if (status != 0) {
        fprintf(stderr, "host: %s\n", gai_strerror(status));
        return -1;
    }
    uint64_t deadline = net_now_ms() + (uint64_t)timeout_ms;
    for (const struct addrinfo *item = addresses; item != NULL; item = item->ai_next) {
        int fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (net_set_nonblocking(fd) < 0) {
            close(fd);
            continue;
        }
        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd ready = {fd, POLLOUT, 0};
            int result;
            do {
                uint64_t now = net_now_ms();
                int remaining = now >= deadline ? 0 : (int)(deadline - now);
                result = poll(&ready, 1, remaining);
            } while (result < 0 && errno == EINTR);
            int error = 0;
            socklen_t length = sizeof(error);
            if (result > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                error == 0) {
                connected = fd;
                break;
            }
            if (result == 0) {
                errno = ETIMEDOUT;
            } else if (error != 0) {
                errno = error;
            }
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

/*
 * net_flush
 * Y nghia : Gui toi da budget byte, cap nhat offset theo ket qua send thuc te.
 * Dau vao : fd - socket nonblocking; queue - TX; budget - quota byte/vong;
 *           written - tong byte gui thanh cong, ke ca khi sau do gap loi.
 * Dau ra  : 0 khi thanh cong/EAGAIN, -1 khi loi fatal (errno); retry EINTR,
 *           giu suffix chua gui va dung MSG_NOSIGNAL tranh SIGPIPE.
 */
int net_flush(int fd, frame_tx_queue_t *queue, size_t budget, size_t *written)
{
    const uint8_t *data;
    size_t length;
    *written = 0;
    while (budget != 0 && (length = frame_tx_peek(queue, &data)) != 0) {
        if (length > budget) {
            length = budget;
        }
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
        if (count > 0) {
            if (frame_tx_advance(queue, (size_t)count) != FRAME_OK) {
                errno = EINVAL;
                return -1;
            }
            budget -= (size_t)count;
            *written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            if (count == 0) {
                errno = EPIPE;
            }
            return -1;
        }
    }
    return 0;
}
