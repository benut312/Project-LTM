/*
 * server.c - Concurrent TCP echo server voi poll va nonblocking socket.
 * Luong: accept -> recv theo quota -> decoder -> JSON handler -> TX queue
 *        -> POLLOUT gui tiep -> EOF/timeout/error cleanup.
 * Moi client so huu RX/TX rieng; khong dung thread hay database o giai doan 1.
 */
#include "server.h"
#include "frame.h"
#include "net.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IO_QUOTA (64u * 1024u)
#define READ_CHUNK 16384u
#define ACCEPT_QUOTA 16u
#define POLL_TICK_MS 100

typedef struct {
    int fd;
    int read_eof;
    frame_decoder_t decoder;
    frame_tx_queue_t tx;
    uint64_t frame_started_ms;
    uint64_t tx_progress_ms;
    uint64_t frames_received;
    uint64_t connection_id;
    const char *close_reason;
} client_t;

static volatile sig_atomic_t stop_requested = 0;

/*
 * request_stop
 * Y nghia : Signal handler chi dat co, khong goi ham I/O khong async-safe.
 * Dau vao : signal_number - SIGINT hoac SIGTERM.
 * Dau ra  : Khong tra ve; event loop thoat o lan check tiep theo.
 */
static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

/*
 * disconnect_client
 * Y nghia : Dong fd, free RX/TX va ghi ly do; goi lai khong double-close.
 * Dau vao : client - slot can thu hoi; reason - chuoi tinh mo ta nguyen nhan.
 * Dau ra  : Khong tra ve; fd=-1 danh dau slot rong.
 */
static void disconnect_client(client_t *client, const char *reason)
{
    if (client->fd < 0) return;
    fprintf(stderr, "DISCONNECT connection=%llu frames=%llu reason=%s\n",
            (unsigned long long)client->connection_id,
            (unsigned long long)client->frames_received, reason);
    close(client->fd);
    client->fd = -1;
    frame_decoder_destroy(&client->decoder);
    frame_tx_destroy(&client->tx);
}

/*
 * enqueue_payload
 * Y nghia : Copy response vao TX, khoi tao timer neu queue vua tu rong sang co.
 * Dau vao : client - connection; payload/length - byte JSON da validate/tao.
 * Dau ra  : 0 thanh cong, -1 queue day/het bo nho; caller con so huu payload.
 */
static int enqueue_payload(client_t *client, const void *payload, size_t length)
{
    int was_empty = client->tx.head == NULL;
    if (frame_tx_push(&client->tx, payload, length) != FRAME_OK) return -1;
    if (was_empty) client->tx_progress_ms = net_now_ms();
    return 0;
}

/*
 * on_frame
 * Y nghia : Validate envelope; echo nguyen payload, tra pong hoac SYS_ERROR.
 * Dau vao : payload/length - frame day du; context - client_t muon, khong free.
 * Dau ra  : 0 tiep tuc; -1 khi khong enqueue duoc, decoder se bao handler loi.
 */
static int on_frame(const uint8_t *payload, size_t length, void *context)
{
    client_t *client = context;
    protocol_message_t message;
    const char *error = NULL;
    char *response = NULL;
    int result;
    client->frames_received++;
    if (protocol_parse(payload, length, &message, &error) != 0) {
        response = protocol_error(NULL, "INVALID_MESSAGE", error);
    } else {
        if (strcmp(message.type, MSG_ECHO) == 0) {
            result = enqueue_payload(client, payload, length);
            protocol_message_destroy(&message);
            return result;
        }
        if (strcmp(message.type, MSG_PING) == 0) {
            response = protocol_build(MSG_PONG, message.request_id, NULL);
        } else {
            response = protocol_error(message.request_id, "UNKNOWN_TYPE",
                                      "Phase 1 accepts ECHO and SYS_PING only");
        }
        protocol_message_destroy(&message);
    }
    if (response == NULL) return -1;
    result = enqueue_payload(client, response, strlen(response));
    cJSON_free(response);
    return result;
}

/*
 * read_client
 * Y nghia : Doc toi da quota, feed decoder; giu frame do va response khi EOF.
 * Dau vao : client - socket nonblocking; deadline duoc check ngoai ham.
 * Dau ra  : Khong tra ve; cap nhat EOF/timer hoac disconnect khi loi fatal.
 */
static void read_client(client_t *client)
{
    uint8_t buffer[READ_CHUNK];
    size_t budget = IO_QUOTA;
    while (budget != 0) {
        size_t amount = budget < sizeof(buffer) ? budget : sizeof(buffer);
        ssize_t count = recv(client->fd, buffer, amount, 0);
        if (count > 0) {
            int was_partial = frame_decoder_in_progress(&client->decoder);
            uint64_t previous_frames = client->frames_received;
            uint64_t now = net_now_ms();
            int result = frame_decoder_feed(&client->decoder, buffer, (size_t)count,
                                            on_frame, client);
            if (result != FRAME_OK) {
                disconnect_client(client, result == FRAME_INVALID ? "invalid_frame_length" :
                                   result == FRAME_NO_MEMORY ? "out_of_memory" : "tx_queue_limit");
                return;
            }
            if (!frame_decoder_in_progress(&client->decoder)) {
                client->frame_started_ms = 0;
            } else if (!was_partial || previous_frames != client->frames_received) {
                /* Frame moi: deadline tu byte dau, khong reset khi chi nhan them. */
                client->frame_started_ms = now;
            }
            budget -= (size_t)count;
        } else if (count == 0) {
            client->close_reason = frame_decoder_in_progress(&client->decoder) ?
                                   "truncated_frame" : "peer_eof";
            client->read_eof = 1;
            client->frame_started_ms = 0;
            frame_decoder_destroy(&client->decoder);
            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        } else {
            disconnect_client(client, "recv_error");
            return;
        }
    }
}

/*
 * accept_clients
 * Y nghia : Accept theo quota; them vao slot rong, tu choi neu dat client cap.
 * Dau vao : listener - fd nonblocking; clients/capacity - registry slot;
 *           next_id - bo dem ket noi tang dan.
 * Dau ra  : 0 binh thuong, -1 loi listener/tai nguyen; fd bi tu choi duoc close.
 */
static int accept_clients(int listener, client_t *clients, unsigned capacity,
                          uint64_t *next_id)
{
    for (unsigned accepted = 0; accepted < ACCEPT_QUOTA; ++accepted) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            return -1;
        }
        unsigned slot;
        for (slot = 0; slot < capacity && clients[slot].fd >= 0; ++slot) {}
        if (slot == capacity || net_set_nonblocking(fd) < 0) {
            fprintf(stderr, "REJECT reason=capacity_or_socket_flags\n");
            close(fd);
            continue;
        }
        int enabled = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
        client_t *client = &clients[slot];
        memset(client, 0, sizeof(*client));
        client->fd = fd;
        client->connection_id = ++*next_id;
        frame_decoder_init(&client->decoder);
        frame_tx_init(&client->tx, FRAME_DEFAULT_TX_LIMIT);
        fprintf(stderr, "CONNECT connection=%llu\n", (unsigned long long)client->connection_id);
    }
    return 0;
}

/*
 * expire_client
 * Y nghia : Ngat frame nhan qua cham hoac TX khong co tien trien.
 * Dau vao : client - slot song; config - timeout ms; now - monotonic ms.
 * Dau ra  : Khong tra ve; co the thu hoi slot neu vuot deadline.
 */
static void expire_client(client_t *client, const server_config_t *config, uint64_t now)
{
    if (client->frame_started_ms != 0 &&
        now - client->frame_started_ms >= config->frame_timeout_ms) {
        disconnect_client(client, "frame_timeout");
    } else if (client->tx.head != NULL &&
               now - client->tx_progress_ms >= config->write_timeout_ms) {
        disconnect_client(client, "write_timeout");
    }
}

/*
 * server_run
 * Y nghia : Tao listener, cai signal handler va multiplex moi client bang poll.
 * Dau vao : config - cau hinh hop le do main validate, ton tai suot event loop.
 * Dau ra  : 0 khi SIGINT/SIGTERM, -1 loi; moi socket/buffer duoc cleanup.
 */
int server_run(const server_config_t *config)
{
    client_t clients[SERVER_MAX_CLIENTS];
    struct pollfd fds[SERVER_MAX_CLIENTS + 1u];
    struct sigaction action = {0};
    struct sigaction old_int, old_term;
    uint64_t next_id = 0;
    uint16_t actual_port = 0;
    int result = 0;
    memset(clients, 0, sizeof(clients));
    for (unsigned i = 0; i < SERVER_MAX_CLIENTS; ++i) clients[i].fd = -1;

    /* Buoc 1: Tao listener va signal handler; handler chi danh dau stop. */
    int listener = net_listen(config->bind_address, config->port, &actual_port);
    if (listener < 0) { perror("listen"); return -1; }
    stop_requested = 0;
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) < 0) {
        close(listener);
        return -1;
    }
    if (sigaction(SIGTERM, &action, &old_term) < 0) {
        (void)sigaction(SIGINT, &old_int, NULL);
        close(listener);
        return -1;
    }
    printf("LISTENING %s %u\n", config->bind_address, (unsigned)actual_port);
    fflush(stdout);

    /* Buoc 2: Moi vong tinh lai events, chi dang ky POLLOUT khi co TX. */
    while (!stop_requested) {
        fds[0] = (struct pollfd){listener, POLLIN, 0};
        uint64_t now = net_now_ms();
        for (unsigned i = 0; i < config->max_clients; ++i) {
            if (clients[i].fd >= 0) expire_client(&clients[i], config, now);
            short events = clients[i].read_eof ? 0 : POLLIN;
            if (clients[i].tx.head != NULL) events |= POLLOUT;
            fds[i + 1u] = (struct pollfd){clients[i].fd, events, 0};
        }
        int ready = poll(fds, config->max_clients + 1u, POLL_TICK_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll"); result = -1; break;
        }
        if (stop_requested) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            result = -1; break;
        }

        /* Buoc 3: Xu ly cac client cu truoc accept, tranh tai su dung slot
         * vao revents cua ket noi da dong trong cung mot vong. */
        now = net_now_ms();
        for (unsigned i = 0; i < config->max_clients; ++i) {
            client_t *client = &clients[i];
            short events = fds[i + 1u].revents;
            if (client->fd < 0) continue;
            expire_client(client, config, now);
            if (client->fd < 0) continue;
            if (events & POLLNVAL) { disconnect_client(client, "invalid_fd"); continue; }
            if (!client->read_eof && (events & (POLLIN | POLLHUP))) read_client(client);
            if (client->fd < 0) continue;
            if (events & POLLERR) { disconnect_client(client, "socket_error"); continue; }
            if (client->tx.head != NULL && events != 0) {
                size_t written = 0;
                if (net_flush(client->fd, &client->tx, IO_QUOTA, &written) < 0) {
                    disconnect_client(client, "send_error"); continue;
                }
                if (written != 0) client->tx_progress_ms = net_now_ms();
            }
            if (client->read_eof && client->tx.head == NULL) {
                disconnect_client(client, client->close_reason);
            }
        }
        if ((fds[0].revents & POLLIN) &&
            accept_clients(listener, clients, config->max_clients, &next_id) < 0) {
            result = -1; break;
        }
    }

    /* Buoc 4: Dong moi connection va khoi phuc signal handler cua caller. */
    close(listener);
    for (unsigned i = 0; i < config->max_clients; ++i) {
        disconnect_client(&clients[i], "server_shutdown");
    }
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    return result;
}
