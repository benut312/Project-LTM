/*
 * main.c - Client terminal kiem thu JSON framed TCP giai doan 1.
 * Luong: connect nonblocking -> nhap/tao request -> poll RX/TX -> in response.
 * Chay: ./bin/ltm-client [-H host] [-p port] [-m "noi dung" | -P].
 * Khong co -m/-P: nhap nhieu dong, /ping kiem tra, /quit de thoat.
 */
#include "frame.h"
#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_MS 5000
#define CLIENT_MAX_TEXT 16384u

typedef struct {
    const char *request_id;
    const char *expected_type;
    int received;
} response_context_t;

/*
 * usage
 * Y nghia : In huong dan client terminal va che do gui mot lan.
 * Dau vao : name - executable; stream - stdout/stderr.
 * Dau ra  : Khong tra ve; ghi huong dan ra stream.
 */
static void usage(const char *name, FILE *stream)
{
    fprintf(stream, "Usage: %s [-H host] [-p port] [-m text | -P]\n"
                    "Defaults: 127.0.0.1:9090. Interactive: /ping, /quit.\n", name);
}

/*
 * on_response
 * Y nghia : Validate response va request id; in JSON/UTF-8 ra stdout.
 * Dau vao : payload/length - frame server; context - request dang cho.
 * Dau ra  : 0 khi response dung; -1 khi sai/loi server; context.received=1
 *           sau response hop le. Khong giu pointer vao payload.
 */
static int on_response(const uint8_t *payload, size_t length, void *context)
{
    response_context_t *expected = context;
    protocol_message_t message;
    const char *error = NULL;
    if (protocol_parse(payload, length, &message, &error) != 0) {
        fprintf(stderr, "Invalid response: %s\n", error);
        return -1;
    }
    int valid = !expected->received && message.request_id != NULL &&
                strcmp(message.request_id, expected->request_id) == 0 &&
                strcmp(message.type, expected->expected_type) == 0;
    if (valid) {
        (void)fwrite(payload, 1, length, stdout);
        putchar('\n');
        fflush(stdout);
        expected->received = 1;
    } else {
        fprintf(stderr, "Unexpected response: %.*s\n", (int)length, (const char *)payload);
    }
    protocol_message_destroy(&message);
    return valid ? 0 : -1;
}

/*
 * exchange_request
 * Y nghia : Enqueue request va poll TX/RX den khi nhan response dung hoac het han.
 * Dau vao : fd - socket nonblocking da connect; type - ECHO/SYS_PING;
 *           text - noi dung ECHO (NULL neu ping); number - so thu tu request.
 * Dau ra  : 0 thanh cong, -1 loi/timeout; free RX/TX/JSON, khong tu close fd.
 */
static int exchange_request(int fd, const char *type, const char *text, unsigned long number)
{
    char request_id[48];
    (void)snprintf(request_id, sizeof(request_id), "cli-%lu", number);
    cJSON *data = cJSON_CreateObject();
    if (data == NULL || (text != NULL && cJSON_AddStringToObject(data, "text", text) == NULL)) {
        cJSON_Delete(data);
        return -1;
    }
    char *json = protocol_build(type, request_id, data);
    cJSON_Delete(data);
    if (json == NULL) return -1;

    /* Buoc 1: Wire frame duoc copy vao TX; JSON goc co the free ngay. */
    frame_tx_queue_t tx;
    frame_decoder_t rx;
    frame_tx_init(&tx, FRAME_DEFAULT_TX_LIMIT);
    frame_decoder_init(&rx);
    int status = frame_tx_push(&tx, json, strlen(json));
    cJSON_free(json);
    response_context_t expected = {request_id, strcmp(type, MSG_PING) == 0 ? MSG_PONG : MSG_ECHO, 0};
    uint64_t deadline = net_now_ms() + CLIENT_TIMEOUT_MS;
    int result = -1;

    /* Buoc 2: Chi cho socket ready, khong cho recv du frame bang blocking loop. */
    while (status == FRAME_OK && !expected.received) {
        uint64_t now = net_now_ms();
        if (now >= deadline) { fprintf(stderr, "Response timeout\n"); break; }
        short events = POLLIN;
        if (tx.head != NULL) events |= POLLOUT;
        struct pollfd ready = {fd, events, 0};
        int available = poll(&ready, 1, (int)(deadline - now));
        if (available < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }
        if (available == 0) continue;
        if (ready.revents & POLLNVAL) break;
        if (ready.revents & (POLLIN | POLLHUP)) {
            uint8_t buffer[16384];
            ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                status = frame_decoder_feed(&rx, buffer, (size_t)count, on_response, &expected);
            } else if (count == 0) {
                fprintf(stderr, "Server closed the connection\n"); break;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recv"); break;
            }
        }
        if (ready.revents & POLLERR) break;
        if (ready.revents & POLLOUT) {
            size_t written;
            if (net_flush(fd, &tx, 65536, &written) < 0) { perror("send"); break; }
        }
        if (status == FRAME_OK && expected.received) result = 0;
    }
    /* Buoc 3: Thu hoi buffer cua request; socket duoc main tai su dung/close. */
    frame_decoder_destroy(&rx);
    frame_tx_destroy(&tx);
    return result;
}

/*
 * send_text
 * Y nghia : Kiem tra gioi han va UTF-8 truoc khi tao ECHO request.
 * Dau vao : fd - socket da connect; text - chuoi NUL; number - request counter.
 * Dau ra  : 0 thanh cong, -1 neu dau vao sai/trao doi that bai.
 */
static int send_text(int fd, const char *text, unsigned long number)
{
    size_t length = strlen(text);
    if (length > CLIENT_MAX_TEXT || !protocol_valid_utf8((const uint8_t *)text, length)) {
        fprintf(stderr, "Text must be valid UTF-8, at most 16384 bytes\n");
        return -1;
    }
    return exchange_request(fd, MSG_ECHO, text, number);
}

/*
 * main
 * Y nghia : Connect mot lan, gui mot request hoac lap nhap/echo tu terminal.
 * Dau vao : argc/argv - host, port, -m text hoac -P ping; -h hien huong dan.
 * Dau ra  : 0 thanh cong/thoat chu dong, 1 loi runtime, 2 tham so sai;
 *           socket duoc close truoc khi ket thuc.
 */
int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    const char *text = NULL;
    unsigned port = 9090;
    int ping = 0, option;
    while ((option = getopt(argc, argv, "H:p:m:Ph")) != -1) {
        if (option == 'h') { usage(argv[0], stdout); return 0; }
        if (option == 'H') host = optarg;
        else if (option == 'm') text = optarg;
        else if (option == 'P') ping = 1;
        else if (option == 'p') {
            if (net_parse_uint(optarg, 65535, &port) != 0 || port == 0) return 2;
        } else { usage(argv[0], stderr); return 2; }
    }
    if (optind != argc || (ping && text != NULL)) { usage(argv[0], stderr); return 2; }
    int fd = net_connect(host, (uint16_t)port, CLIENT_TIMEOUT_MS);
    if (fd < 0) { perror("connect"); return 1; }
    int result = 0;
    if (ping) result = exchange_request(fd, MSG_PING, NULL, 1);
    else if (text != NULL) result = send_text(fd, text, 1);
    else {
        char line[CLIENT_MAX_TEXT + 2u];
        unsigned long number = 0;
        int terminal = isatty(STDIN_FILENO);
        while (1) {
            if (terminal) { printf("Nhap noi dung (/ping, /quit): "); fflush(stdout); }
            if (fgets(line, sizeof(line), stdin) == NULL) break;
            size_t length = strlen(line);
            if (length > 0 && line[length - 1u] == '\n') line[--length] = '\0';
            if (length > 0 && line[length - 1u] == '\r') line[--length] = '\0';
            if (length > CLIENT_MAX_TEXT) {
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {}
                fprintf(stderr, "Input line too long\n"); result = -1; break;
            }
            if (strcmp(line, "/quit") == 0) break;
            result = strcmp(line, "/ping") == 0 ? exchange_request(fd, MSG_PING, NULL, ++number) :
                                                  send_text(fd, line, ++number);
            if (result != 0) break;
        }
    }
    close(fd);
    return result == 0 ? 0 : 1;
}
