/*
 * main.c - Cau hinh va khoi dong server TCP giai doan 1.
 * Bien dich: make; chay: ./bin/ltm-server [-b IP] [-p PORT] [-m CLIENTS]
 *           [-t FRAME_TIMEOUT_MS] [-w WRITE_TIMEOUT_MS]. Ctrl+C de dung sach.
 */
#include "server.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * usage
 * Y nghia : In cach su dung command line cua server.
 * Dau vao : name - ten executable; stream - stdout/stderr.
 * Dau ra  : Khong tra ve; ghi huong dan ra stream.
 */
static void usage(const char *name, FILE *stream)
{
    fprintf(stream, "Usage: %s [-b IPv4] [-p port] [-m clients] [-t frame_ms] [-w write_ms]\n"
                    "Defaults: 127.0.0.1:9090, 128 clients, timeouts 10000 ms.\n"
                    "Port 0 asks the OS to choose a free port.\n", name);
}

/*
 * main
 * Y nghia : Parse tham so co gioi han, khoi dong event loop.
 * Dau vao : argc/argv - tham so command line, -h hien huong dan.
 * Dau ra  : 0 ket thuc sach, 1 loi runtime, 2 tham so khong hop le.
 */
int main(int argc, char *argv[])
{
    server_config_t config = {"127.0.0.1", SERVER_DEFAULT_PORT,
                              SERVER_MAX_CLIENTS, 10000, 10000};
    int option;
    while ((option = getopt(argc, argv, "b:p:m:t:w:h")) != -1) {
        unsigned value = 0;
        if (option == 'h') {
            usage(argv[0], stdout);
            return 0;
        }
        if (option == 'b') {
            config.bind_address = optarg;
            continue;
        }
        unsigned maximum = option == 'p' ? 65535u :
                           option == 'm' ? SERVER_MAX_CLIENTS : 3600000u;
        if ((option != 'p' && option != 'm' && option != 't' && option != 'w') ||
            net_parse_uint(optarg, maximum, &value) != 0 ||
            (option != 'p' && value == 0)) {
            usage(argv[0], stderr);
            return 2;
        }
        if (option == 'p') config.port = (uint16_t)value;
        if (option == 'm') config.max_clients = value;
        if (option == 't') config.frame_timeout_ms = value;
        if (option == 'w') config.write_timeout_ms = value;
    }
    if (optind != argc) {
        usage(argv[0], stderr);
        return 2;
    }
    return server_run(&config) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
