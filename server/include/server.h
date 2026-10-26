#ifndef LTM_SERVER_H
#define LTM_SERVER_H

#include <stdint.h>

#define SERVER_MAX_CLIENTS 128u
#define SERVER_DEFAULT_PORT 9090u

typedef struct {
    const char *bind_address;
    uint16_t port;
    unsigned max_clients;
    unsigned frame_timeout_ms;
    unsigned write_timeout_ms;
} server_config_t;

int server_run(const server_config_t *config);

#endif
